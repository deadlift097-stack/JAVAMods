#include "mod/LuaHost.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

#include <nlohmann/json.hpp>
#include <pl/ModMenu.hpp>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

namespace luahost {

namespace fs = std::filesystem;

namespace {

// Change when the build changes so old crash markers stop applying.
constexpr const char *kGuardTag = "luahost1-2026-11-09";

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string sanitizeId(const std::string &s) {
    std::string out;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u))
            out += static_cast<char>(std::tolower(u));
        else if (c == '_' || c == '-' || c == '.')
            out += c;
        else
            out += '_';
    }
    return out.empty() ? std::string("mod") : out;
}

bool parseBool(std::string_view v) { return v == "true" || v == "1" || v == "on"; }

std::string formatNumber(double v) {
    char buf[64];
    if (std::floor(v) == v && std::fabs(v) < 1e15)
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    else
        std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

} // namespace

struct SettingDef {
    std::string key, label, type; // type: toggle | slider | float
    std::string defValue, minValue, maxValue;
};

struct TimerEntry {
    int id = 0;
    int ref = LUA_NOREF;
    int64_t nextMs = 0;
    int64_t intervalMs = 0;
    bool repeat = false;
};

struct LuaMod {
    std::string id, name, version = "0.0.0", author, description;
    fs::path dir, mainFile, storageFile;
    bool singleFile = false;

    lua_State *L = nullptr;
    size_t memUsed = 0, memLimit = 0;
    int maxMs = 50;
    int64_t callStartMs = 0;
    int errors = 0;
    bool failed = false;
    bool moduleEnabled = true;

    // Menu definition (from menu.register).
    bool hasMenu = false, menuRegistered = false;
    std::string moduleId, menuName, menuDesc;
    bool menuDefaultEnabled = true;
    std::vector<SettingDef> settings;
    std::map<std::string, std::string> values;
    int onToggleRef = LUA_NOREF, onChangeRef = LUA_NOREF;

    std::map<std::string, std::vector<int>> handlers;
    std::vector<TimerEntry> timers;
    int nextTimerId = 1;

    nlohmann::json storage = nlohmann::json::object();
};

namespace {

// ---------------------------------------------------------------- Lua plumbing

LuaMod *modOf(lua_State *L) {
    void *ud = nullptr;
    lua_getallocf(L, &ud);
    return static_cast<LuaMod *>(ud);
}

// Allocator that enforces the per-mod memory limit.
void *luaAlloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    auto *m = static_cast<LuaMod *>(ud);
    if (nsize == 0) {
        if (ptr)
            m->memUsed -= osize;
        std::free(ptr);
        return nullptr;
    }
    const size_t old = ptr ? osize : 0;
    if (nsize > old && m->memUsed + (nsize - old) > m->memLimit)
        return nullptr;
    void *np = std::realloc(ptr, nsize);
    if (np)
        m->memUsed = m->memUsed - old + nsize;
    return np;
}

// Stops scripts that run too long.
void timeHook(lua_State *L, lua_Debug *) {
    LuaMod *m = modOf(L);
    if (nowMs() - m->callStartMs > m->maxMs)
        luaL_error(L, "script ran longer than %d ms", m->maxMs);
}

int traceback(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (msg == nullptr) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
            return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// Calls the function sitting below `nargs` arguments on the stack.
bool callTop(LuaMod &m, int nargs, int nresults) {
    lua_State *L = m.L;
    const int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    m.callStartMs = nowMs();
    const int rc = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    if (rc != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        LuaHost::instance().log("ERROR", m.id, msg ? msg : "(error object)");
        lua_pop(L, 1);
        if (++m.errors >= 5 && !m.failed) {
            m.failed = true;
            LuaHost::instance().log("ERROR", m.id, "stopped after too many errors");
        }
        return false;
    }
    return true;
}

void pushSettingValue(lua_State *L, const SettingDef &s, const std::string &v) {
    if (s.type == "toggle") {
        lua_pushboolean(L, parseBool(v));
    } else {
        const double d = std::strtod(v.c_str(), nullptr);
        if (s.type == "slider")
            lua_pushinteger(L, static_cast<lua_Integer>(d));
        else
            lua_pushnumber(L, d);
    }
}

void pushJson(lua_State *L, const nlohmann::json &j) {
    if (j.is_boolean())
        lua_pushboolean(L, j.get<bool>());
    else if (j.is_number_integer())
        lua_pushinteger(L, static_cast<lua_Integer>(j.get<long long>()));
    else if (j.is_number())
        lua_pushnumber(L, j.get<double>());
    else if (j.is_string()) {
        const std::string s = j.get<std::string>();
        lua_pushlstring(L, s.data(), s.size());
    } else
        lua_pushnil(L);
}

// ---------------------------------------------------------------- Lua API

std::string joinArgs(lua_State *L) {
    const int n = lua_gettop(L);
    std::string out;
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1)
            out += ' ';
        out.append(s, len);
        lua_pop(L, 1);
    }
    return out;
}

int l_logInfo(lua_State *L) {
    LuaHost::instance().log("INFO", modOf(L)->id, joinArgs(L));
    return 0;
}
int l_logWarn(lua_State *L) {
    LuaHost::instance().log("WARN", modOf(L)->id, joinArgs(L));
    return 0;
}
int l_logError(lua_State *L) {
    LuaHost::instance().log("ERROR", modOf(L)->id, joinArgs(L));
    return 0;
}

int l_on(lua_State *L) {
    LuaMod *m = modOf(L);
    const std::string event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (event != "attack" && event != "key" && event != "mouse" && event != "touch" && event != "tick")
        return luaL_error(L, "unknown event '%s' (use attack, key, mouse, touch or tick)", event.c_str());
    if (m->handlers[event].size() >= 64)
        return luaL_error(L, "too many handlers for '%s'", event.c_str());
    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    m->handlers[event].push_back(ref);
    LuaHost::instance().noteHandler(event);
    return 0;
}

std::string tableString(lua_State *L, int idx, const char *key, const std::string &def) {
    lua_getfield(L, idx, key);
    std::string v = def;
    if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER) {
        size_t len = 0;
        const char *s = lua_tolstring(L, -1, &len);
        v.assign(s, len);
    }
    lua_pop(L, 1);
    return v;
}

// Reads a number/boolean/string field as the text the menu expects.
std::string tableValueText(lua_State *L, int idx, const char *key, const std::string &def) {
    lua_getfield(L, idx, key);
    std::string v = def;
    if (lua_type(L, -1) == LUA_TBOOLEAN)
        v = lua_toboolean(L, -1) ? "true" : "false";
    else if (lua_type(L, -1) == LUA_TNUMBER)
        v = formatNumber(lua_tonumber(L, -1));
    else if (lua_type(L, -1) == LUA_TSTRING)
        v = lua_tostring(L, -1);
    lua_pop(L, 1);
    return v;
}

bool validKey(const std::string &k) {
    if (k.empty() || k.size() > 64)
        return false;
    for (char c : k)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    return true;
}

int l_menuRegister(lua_State *L) {
    LuaMod *m = modOf(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    if (m->hasMenu)
        return luaL_error(L, "menu.register can only be called once per mod");

    m->menuName = tableString(L, 1, "name", m->name.empty() ? m->id : m->name);
    m->menuDesc = tableString(L, 1, "description", m->description);
    lua_getfield(L, 1, "enabled");
    m->menuDefaultEnabled = lua_isnil(L, -1) ? true : (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "settings");
    if (lua_istable(L, -1)) {
        const int n = static_cast<int>(luaL_len(L, -1));
        for (int i = 1; i <= n && m->settings.size() < 64; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                SettingDef s;
                s.key = tableString(L, -1, "key", "");
                s.label = tableString(L, -1, "label", s.key);
                s.type = tableString(L, -1, "type", "toggle");
                if (s.type != "toggle" && s.type != "slider" && s.type != "float")
                    s.type = "toggle";
                if (s.type == "toggle") {
                    s.defValue = tableValueText(L, -1, "default", "false");
                    if (s.defValue != "true")
                        s.defValue = "false";
                } else {
                    s.defValue = tableValueText(L, -1, "default", "0");
                    s.minValue = tableValueText(L, -1, "min", "0");
                    s.maxValue = tableValueText(L, -1, "max", "100");
                }
                if (validKey(s.key))
                    m->settings.push_back(s);
                else
                    LuaHost::instance().log("WARN", m->id, "skipped a setting with an invalid key");
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "on_toggle");
    if (lua_isfunction(L, -1))
        m->onToggleRef = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);
    lua_getfield(L, 1, "on_change");
    if (lua_isfunction(L, -1))
        m->onChangeRef = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    m->hasMenu = true;
    LuaHost::instance().prepareSettings(*m);
    return 0;
}

int l_storageGet(lua_State *L) {
    LuaMod *m = modOf(L);
    const std::string key = luaL_checkstring(L, 1);
    const nlohmann::json &user = m->storage["user"];
    if (user.is_object() && user.contains(key)) {
        pushJson(L, user[key]);
        return 1;
    }
    if (lua_gettop(L) >= 2)
        lua_pushvalue(L, 2);
    else
        lua_pushnil(L);
    return 1;
}

int l_storageSet(lua_State *L) {
    LuaMod *m = modOf(L);
    const std::string key = luaL_checkstring(L, 1);
    if (key.empty() || key.size() > 128)
        return luaL_error(L, "bad storage key");
    nlohmann::json &user = m->storage["user"];
    if (!user.is_object())
        user = nlohmann::json::object();
    switch (lua_type(L, 2)) {
    case LUA_TNIL:
    case LUA_TNONE:
        user.erase(key);
        break;
    case LUA_TBOOLEAN:
        user[key] = (lua_toboolean(L, 2) != 0);
        break;
    case LUA_TNUMBER:
        if (lua_isinteger(L, 2))
            user[key] = static_cast<long long>(lua_tointeger(L, 2));
        else
            user[key] = static_cast<double>(lua_tonumber(L, 2));
        break;
    case LUA_TSTRING: {
        size_t len = 0;
        const char *s = lua_tolstring(L, 2, &len);
        if (len > 65536)
            return luaL_error(L, "storage value too long");
        user[key] = std::string(s, len);
        break;
    }
    default:
        return luaL_error(L, "storage can only hold booleans, numbers and strings");
    }
    LuaHost::instance().saveStorage(*m);
    return 0;
}

int addTimer(lua_State *L, bool repeat) {
    LuaMod *m = modOf(L);
    lua_Number ms = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (m->timers.size() >= 200)
        return luaL_error(L, "too many timers");
    if (ms < 10)
        ms = 10;
    lua_pushvalue(L, 2);
    TimerEntry t;
    t.id = m->nextTimerId++;
    t.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    t.intervalMs = static_cast<int64_t>(ms);
    t.nextMs = nowMs() + t.intervalMs;
    t.repeat = repeat;
    m->timers.push_back(t);
    lua_pushinteger(L, t.id);
    return 1;
}

int l_timerAfter(lua_State *L) { return addTimer(L, false); }
int l_timerEvery(lua_State *L) { return addTimer(L, true); }

int l_timerCancel(lua_State *L) {
    LuaMod *m = modOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    auto it = std::find_if(m->timers.begin(), m->timers.end(), [&](const TimerEntry &t) { return t.id == id; });
    if (it != m->timers.end()) {
        luaL_unref(L, LUA_REGISTRYINDEX, it->ref);
        m->timers.erase(it);
    }
    return 0;
}

int l_timeMs(lua_State *L) {
    lua_pushinteger(L, static_cast<lua_Integer>(nowMs()));
    return 1;
}

// require("name") loads <mod folder>/name.lua (folder mods only).
int l_require(lua_State *L) {
    LuaMod *m = modOf(L);
    const std::string name = luaL_checkstring(L, 1);
    if (name.empty() || name.size() > 100 || name.find("..") != std::string::npos || name[0] == '/' || name[0] == '.')
        return luaL_error(L, "bad module name '%s'", name.c_str());
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '/'))
            return luaL_error(L, "bad module name '%s'", name.c_str());

    lua_getfield(L, LUA_REGISTRYINDEX, "luahost.loaded");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "luahost.loaded");
    }
    lua_getfield(L, -1, name.c_str());
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 1);

    std::string rel = name;
    std::replace(rel.begin(), rel.end(), '.', '/');
    const fs::path file = m->dir / (rel + ".lua");
    if (luaL_loadfilex(L, file.string().c_str(), "t") != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name.c_str());
    return 1;
}

void setFn(lua_State *L, const char *name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
}

void openLibs(lua_State *L) {
    static const luaL_Reg libs[] = {{"_G", luaopen_base},
                                    {LUA_TABLIBNAME, luaopen_table},
                                    {LUA_STRLIBNAME, luaopen_string},
                                    {LUA_MATHLIBNAME, luaopen_math},
                                    {LUA_UTF8LIBNAME, luaopen_utf8},
                                    {LUA_COLIBNAME, luaopen_coroutine},
                                    {LUA_OSLIBNAME, luaopen_os},
                                    {nullptr, nullptr}};
    for (const luaL_Reg *l = libs; l->func; ++l) {
        luaL_requiref(L, l->name, l->func, 1);
        lua_pop(L, 1);
    }
}

// Removes everything that could touch files, run programs or load bytecode.
void sandbox(lua_State *L) {
    static const char *removed[] = {"dofile", "loadfile", "load", "loadstring"};
    for (const char *name : removed) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    static const char *keepOs[] = {"time", "clock", "date", "difftime"};
    lua_getglobal(L, "os");
    lua_newtable(L);
    for (const char *name : keepOs) {
        lua_getfield(L, -2, name);
        lua_setfield(L, -2, name);
    }
    lua_setglobal(L, "os");
    lua_pop(L, 1);
}

void installApi(lua_State *L, LuaMod &m) {
    lua_newtable(L);
    setFn(L, "info", l_logInfo);
    setFn(L, "warn", l_logWarn);
    setFn(L, "error", l_logError);
    lua_setglobal(L, "log");
    lua_pushcfunction(L, l_logInfo);
    lua_setglobal(L, "print");

    lua_pushcfunction(L, l_on);
    lua_setglobal(L, "on");

    lua_newtable(L);
    setFn(L, "register", l_menuRegister);
    lua_setglobal(L, "menu");

    lua_newtable(L);
    setFn(L, "get", l_storageGet);
    setFn(L, "set", l_storageSet);
    lua_setglobal(L, "storage");

    lua_newtable(L);
    setFn(L, "after", l_timerAfter);
    setFn(L, "every", l_timerEvery);
    setFn(L, "cancel", l_timerCancel);
    lua_setglobal(L, "timer");

    lua_newtable(L);
    setFn(L, "ms", l_timeMs);
    lua_setglobal(L, "time");

    lua_newtable(L);
    lua_pushstring(L, m.id.c_str());
    lua_setfield(L, -2, "id");
    lua_pushstring(L, m.name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushstring(L, m.version.c_str());
    lua_setfield(L, -2, "version");
    lua_pushstring(L, m.author.c_str());
    lua_setfield(L, -2, "author");
    lua_pushstring(L, m.description.c_str());
    lua_setfield(L, -2, "description");
    lua_pushstring(L, m.dir.string().c_str());
    lua_setfield(L, -2, "dir");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "enabled");
    lua_setglobal(L, "mod");

    lua_newtable(L);
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "api");
    lua_pushstring(L, "0.1.0");
    lua_setfield(L, -2, "version");
    lua_setglobal(L, "host");

    lua_newtable(L);
    lua_setglobal(L, "settings");

    if (!m.singleFile) {
        lua_pushcfunction(L, l_require);
        lua_setglobal(L, "require");
    }
}

// ---------------------------------------------------------------- menu thunks

void toggleThunk(std::string_view moduleId, bool enabled) { LuaHost::instance().handleToggle(moduleId, enabled); }

void configThunk(std::string_view moduleId, std::string_view key, std::string_view value) {
    LuaHost::instance().handleConfigChanged(moduleId, key, value);
}

// Attach the change callback only if this SDK's builder accepts one with this shape.
template <class B> bool attachConfigCallback(B &builder) {
    if constexpr (requires { builder.onConfigChanged(&configThunk); }) {
        builder.onConfigChanged(&configThunk);
        return true;
    } else {
        return false;
    }
}

} // namespace

// ------------------------------------------------------------------ LuaHost

LuaHost &LuaHost::instance() {
    static LuaHost *host = new LuaHost(); // intentionally never destroyed
    return *host;
}

LuaHost::LuaHost() {
    for (auto &c : mHandlerCounts)
        c = 0;
}

int LuaHost::eventIndex(std::string_view name) {
    if (name == "attack")
        return EvAttack;
    if (name == "key")
        return EvKey;
    if (name == "mouse")
        return EvMouse;
    if (name == "touch")
        return EvTouch;
    if (name == "tick")
        return EvTick;
    return -1;
}

void LuaHost::noteHandler(const std::string &event) {
    const int i = eventIndex(event);
    if (i >= 0)
        ++mHandlerCounts[i];
}

bool LuaHost::guardTripped(const char *name) {
    std::ifstream in(mDataDir / name);
    if (!in)
        return false;
    std::string content;
    std::getline(in, content);
    return content == kGuardTag;
}

void LuaHost::guardSet(const char *name) {
    std::ofstream out(mDataDir / name, std::ios::trunc);
    if (out)
        out << kGuardTag << "\n";
}

void LuaHost::guardClear(const char *name) {
    std::error_code ec;
    fs::remove(mDataDir / name, ec);
}

void LuaHost::log(const char *level, const std::string &modId, const std::string &message) {
    std::lock_guard<std::mutex> lk(mLogMutex);
    const std::string line = std::string("[") + level + "][" + modId + "] " + message;
    if (mLogFile.is_open()) {
        mLogFile << line << '\n';
        mLogFile.flush();
    }
    if (mSelf) {
        auto &logger = mSelf->getLogger();
        const std::string_view lv = level;
        if (lv == "ERROR")
            logger.error("{}", line);
        else if (lv == "WARN")
            logger.warn("{}", line);
        else
            logger.info("{}", line);
    }
}

void LuaHost::resolveDirs() {
    std::string pkg;
    {
        std::ifstream f("/proc/self/cmdline");
        std::getline(f, pkg, '\0');
    }
    bool pkgOk = !pkg.empty();
    for (char c : pkg)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_'))
            pkgOk = false;

    std::error_code ec;
    fs::path ext;
    if (pkgOk) {
        // App media folder: visible in file managers without special permissions.
        ext = fs::path("/storage/emulated/0/Android/media") / pkg / "mod_host";
        fs::create_directories(ext / "mods", ec);
        if (ec || !fs::is_directory(ext / "mods", ec))
            ext.clear();
    }
    if (ext.empty()) {
        ext = mDataDir / "mod_host";
        fs::create_directories(ext / "mods", ec);
    }
    mExtDir = ext;
    mModsDir = ext / "mods";
    fs::create_directories(mDataDir / "lua_storage", ec);

    const fs::path readme = mModsDir / "README.txt";
    if (!fs::exists(readme, ec)) {
        std::ofstream out(readme, std::ios::trunc);
        if (out)
            out << "Put Lua mods in this folder.\n"
                   "  One file:  mods/hello.lua\n"
                   "  A folder:  mods/my_mod/main.lua  (optional mod.json next to it)\n"
                   "Errors and messages are written to ../mod_host.log\n"
                   "Restart the game after adding or changing a mod.\n";
    }
}

void LuaHost::openLog() {
    std::lock_guard<std::mutex> lk(mLogMutex);
    if (mLogFile.is_open())
        mLogFile.close();
    mLogFile.open(mExtDir / "mod_host.log", std::ios::trunc);
}

void LuaHost::loadStorage(LuaMod &m) {
    std::ifstream in(m.storageFile);
    if (!in)
        return;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_object())
        m.storage = std::move(j);
}

void LuaHost::saveStorage(LuaMod &m) {
    std::error_code ec;
    fs::create_directories(m.storageFile.parent_path(), ec);
    std::ofstream out(m.storageFile, std::ios::trunc);
    if (out)
        out << m.storage.dump();
}

void LuaHost::prepareSettings(LuaMod &m) {
    nlohmann::json &saved = m.storage["settings"];
    if (!saved.is_object())
        saved = nlohmann::json::object();
    lua_State *L = m.L;
    lua_newtable(L);
    for (const SettingDef &s : m.settings) {
        std::string v = s.defValue;
        if (saved.contains(s.key) && saved[s.key].is_string())
            v = saved[s.key].get<std::string>();
        m.values[s.key] = v;
        pushSettingValue(L, s, v);
        lua_setfield(L, -2, s.key.c_str());
    }
    lua_setglobal(L, "settings");
}

void LuaHost::recountHandlers() {
    for (auto &c : mHandlerCounts)
        c = 0;
    for (auto &mp : mMods)
        for (auto &kv : mp->handlers) {
            const int i = eventIndex(kv.first);
            if (i >= 0)
                mHandlerCounts[i] += static_cast<int>(kv.second.size());
        }
}

void LuaHost::loadMod(const fs::path &mainFile, const fs::path &dir, bool single, const std::string &defaultId) {
    auto mod = std::make_unique<LuaMod>();
    LuaMod &m = *mod;
    m.id = sanitizeId(defaultId);
    m.name = defaultId;
    m.dir = dir;
    m.mainFile = mainFile;
    m.singleFile = single;
    m.memLimit = static_cast<size_t>(std::max(1, mConfig.maxMemoryMb)) * 1024u * 1024u;
    m.maxMs = std::max(5, mConfig.maxScriptMs);

    if (!single) {
        std::ifstream in(dir / "mod.json");
        if (in) {
            nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_object()) {
                if (j.contains("enabled") && j["enabled"].is_boolean() && !j["enabled"].get<bool>()) {
                    log("INFO", m.id, "skipped (disabled in mod.json)");
                    return;
                }
                auto str = [&](const char *key, std::string &target) {
                    if (j.contains(key) && j[key].is_string())
                        target = j[key].get<std::string>();
                };
                std::string idText;
                str("id", idText);
                if (!idText.empty())
                    m.id = sanitizeId(idText);
                str("name", m.name);
                str("version", m.version);
                str("author", m.author);
                str("description", m.description);
            }
        }
    }

    for (auto &other : mMods)
        if (other->id == m.id) {
            log("WARN", m.id, "skipped: another mod already uses this id");
            return;
        }

    m.storageFile = mDataDir / "lua_storage" / (m.id + ".json");
    loadStorage(m);

    m.L = lua_newstate(luaAlloc, &m);
    if (!m.L) {
        log("ERROR", m.id, "could not create a Lua state");
        return;
    }
    openLibs(m.L);
    sandbox(m.L);
    installApi(m.L, m);
    lua_sethook(m.L, timeHook, LUA_MASKCOUNT, 10000);

    if (luaL_loadfilex(m.L, mainFile.string().c_str(), "t") != LUA_OK) {
        const char *msg = lua_tostring(m.L, -1);
        log("ERROR", m.id, std::string("could not load: ") + (msg ? msg : "?"));
        lua_close(m.L);
        return;
    }

    const int normalMs = m.maxMs;
    m.maxMs = std::max(normalMs, 1000); // setting up may take longer than a single event
    const bool ok = callTop(m, 0, 0);
    m.maxMs = normalMs;
    if (!ok) {
        lua_close(m.L);
        return;
    }

    log("INFO", m.id, "loaded " + m.name + " v" + m.version);
    mMods.push_back(std::move(mod));
}

void LuaHost::scanAndLoad() {
    const std::vector<fs::path> roots{mModsDir, mDataDir / "lua_mods"};
    for (const fs::path &root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec))
            continue;
        std::vector<fs::path> entries;
        for (const auto &e : fs::directory_iterator(root, ec))
            entries.push_back(e.path());
        std::sort(entries.begin(), entries.end());
        for (const fs::path &p : entries) {
            const std::string name = p.filename().string();
            if (name.empty() || name[0] == '.')
                continue;
            if (fs::is_directory(p, ec)) {
                const fs::path mainFile = p / "main.lua";
                if (fs::exists(mainFile, ec))
                    loadMod(mainFile, p, false, name);
            } else if (p.extension() == ".lua") {
                loadMod(p, root, true, p.stem().string());
            }
        }
    }
    recountHandlers();
    log("INFO", "host", std::to_string(mMods.size()) + " Lua mod(s) loaded from " + mModsDir.string());
}

void LuaHost::registerMenus() {
    bool any = false;
    for (auto &mp : mMods)
        if (mp->hasMenu)
            any = true;
    if (!any)
        return;
    if (guardTripped("menu.pending")) {
        log("WARN", "host", "The menu crashed the game last time; skipping it. Delete menu.pending in the mod's data folder to retry.");
        return;
    }
    guardSet("menu.pending");

    for (auto &mp : mMods) {
        LuaMod &m = *mp;
        if (!m.hasMenu)
            continue;
        m.moduleId = m.id + ".menu";

        pl::modmenu::ModuleBuilder builder(m.moduleId.c_str(), m.menuName.c_str());
        builder.modId(mSelf->getId());
        builder.description(m.menuDesc.c_str());
        builder.defaultEnabled(m.menuDefaultEnabled);
        builder.onToggle(&toggleThunk);
        for (const SettingDef &s : m.settings) {
            const std::string &value = m.values[s.key];
            if (s.type == "toggle") {
                builder.config(s.key.c_str(), s.label.c_str(), pl::modmenu::ConfigType::Toggle, value.c_str());
            } else {
                builder.config(s.key.c_str(), s.label.c_str(),
                               s.type == "slider" ? pl::modmenu::ConfigType::SliderInt
                                                  : pl::modmenu::ConfigType::SliderFloat,
                               value.c_str(), s.minValue.c_str(), s.maxValue.c_str());
            }
        }
        if (!attachConfigCallback(builder))
            log("WARN", m.id, "this SDK has no menu change callback; menu edits will not reach the Lua mod");
        if (builder.registerModule())
            m.menuRegistered = true;
        else
            log("WARN", m.id, "could not register the in-game menu module");
    }

    guardClear("menu.pending");
}

LuaMod *LuaHost::findByModuleId(std::string_view moduleId) {
    for (auto &mp : mMods)
        if (mp->moduleId == moduleId)
            return mp.get();
    return nullptr;
}

void LuaHost::handleToggle(std::string_view moduleId, bool enabled) {
    std::lock_guard<std::recursive_mutex> lk(mMutex);
    LuaMod *m = findByModuleId(moduleId);
    if (!m || m->failed || !m->L)
        return;
    m->moduleEnabled = enabled;
    lua_State *L = m->L;
    lua_getglobal(L, "mod");
    if (lua_istable(L, -1)) {
        lua_pushboolean(L, enabled ? 1 : 0);
        lua_setfield(L, -2, "enabled");
    }
    lua_pop(L, 1);
    if (m->onToggleRef != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, m->onToggleRef);
        lua_pushboolean(L, enabled ? 1 : 0);
        callTop(*m, 1, 0);
    }
}

void LuaHost::handleConfigChanged(std::string_view moduleId, std::string_view key, std::string_view value) {
    std::lock_guard<std::recursive_mutex> lk(mMutex);
    LuaMod *m = findByModuleId(moduleId);
    if (!m || m->failed || !m->L)
        return;
    const std::string k(key);
    auto it = std::find_if(m->settings.begin(), m->settings.end(), [&](const SettingDef &s) { return s.key == k; });
    if (it == m->settings.end())
        return;

    m->values[k] = std::string(value);
    nlohmann::json &saved = m->storage["settings"];
    if (!saved.is_object())
        saved = nlohmann::json::object();
    saved[k] = std::string(value);
    saveStorage(*m);

    lua_State *L = m->L;
    lua_getglobal(L, "settings");
    if (lua_istable(L, -1)) {
        pushSettingValue(L, *it, m->values[k]);
        lua_setfield(L, -2, k.c_str());
    }
    lua_pop(L, 1);

    if (m->onChangeRef != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, m->onChangeRef);
        lua_pushstring(L, k.c_str());
        pushSettingValue(L, *it, m->values[k]);
        callTop(*m, 2, 0);
    }
}

bool LuaHost::dispatch(const char *event, const std::function<int(lua_State *)> &pushArgs) {
    std::lock_guard<std::recursive_mutex> lk(mMutex);
    if (!mRunning.load())
        return false;
    bool consumed = false;
    for (auto &mp : mMods) {
        LuaMod &m = *mp;
        if (m.failed || !m.moduleEnabled || !m.L)
            continue;
        auto it = m.handlers.find(event);
        if (it == m.handlers.end())
            continue;
        const std::vector<int> refs = it->second;
        for (int ref : refs) {
            lua_State *L = m.L;
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            const int n = pushArgs ? pushArgs(L) : 0;
            if (callTop(m, n, 1)) {
                if (lua_toboolean(L, -1))
                    consumed = true;
                lua_pop(L, 1);
            }
            if (m.failed)
                break;
        }
    }
    return consumed;
}

bool LuaHost::onKey(int keyCode, bool down, unsigned int unicodeChar) {
    if (!mRunning.load() || mHandlerCounts[EvKey].load() == 0)
        return false;
    return dispatch("key", [&](lua_State *L) {
        lua_pushinteger(L, keyCode);
        lua_pushboolean(L, down ? 1 : 0);
        lua_pushinteger(L, static_cast<lua_Integer>(unicodeChar));
        return 3;
    });
}

bool LuaHost::onTouch(int action, int pointerId, float x, float y) {
    if (!mRunning.load() || mHandlerCounts[EvTouch].load() == 0)
        return false;
    return dispatch("touch", [&](lua_State *L) {
        lua_pushinteger(L, action);
        lua_pushinteger(L, pointerId);
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        return 4;
    });
}

bool LuaHost::onMouse(int button, bool down) {
    if (!mRunning.load() || mHandlerCounts[EvMouse].load() == 0)
        return false;
    return dispatch("mouse", [&](lua_State *L) {
        lua_pushinteger(L, button);
        lua_pushboolean(L, down ? 1 : 0);
        return 2;
    });
}

void LuaHost::onAttack() {
    if (!mRunning.load() || mHandlerCounts[EvAttack].load() == 0)
        return;
    dispatch("attack", [&](lua_State *L) {
        lua_createtable(L, 0, 1);
        lua_pushinteger(L, static_cast<lua_Integer>(nowMs()));
        lua_setfield(L, -2, "time_ms");
        return 1;
    });
}

void LuaHost::runTimers(int64_t now) {
    for (auto &mp : mMods) {
        LuaMod &m = *mp;
        if (m.failed || !m.moduleEnabled || m.timers.empty() || !m.L)
            continue;
        std::vector<int> due;
        for (const TimerEntry &t : m.timers)
            if (t.nextMs <= now)
                due.push_back(t.id);
        for (int id : due) {
            auto it = std::find_if(m.timers.begin(), m.timers.end(), [&](const TimerEntry &t) { return t.id == id; });
            if (it == m.timers.end())
                continue; // cancelled by an earlier callback
            const int ref = it->ref;
            const bool repeat = it->repeat;
            if (repeat)
                it->nextMs = now + it->intervalMs;
            else
                m.timers.erase(it);
            lua_rawgeti(m.L, LUA_REGISTRYINDEX, ref);
            callTop(m, 0, 0);
            if (!repeat)
                luaL_unref(m.L, LUA_REGISTRYINDEX, ref);
            if (m.failed)
                break;
        }
    }
}

void LuaHost::threadMain() {
    int64_t lastTick = nowMs();
    while (mRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const int64_t now = nowMs();
        {
            std::lock_guard<std::recursive_mutex> lk(mMutex);
            if (!mRunning.load())
                break;
            runTimers(now);
        }
        if (mConfig.tickEvent && now - lastTick >= 50 && mHandlerCounts[EvTick].load() > 0) {
            const int64_t dt = now - lastTick;
            lastTick = now;
            dispatch("tick", [&](lua_State *L) {
                lua_pushinteger(L, static_cast<lua_Integer>(dt));
                return 1;
            });
        } else if (now - lastTick >= 50) {
            lastTick = now;
        }
    }
}

void LuaHost::start(ll::mod::NativeMod &self, const ModConfig &config) {
    std::lock_guard<std::recursive_mutex> lk(mMutex);
    if (mRunning.load())
        return;
    mSelf = &self;
    mConfig = config;
    mDataDir = self.getDataDir();
    resolveDirs();
    openLog();
    log("INFO", "host", "Mod Host starting. Lua mods folder: " + mModsDir.string());

    mRunning = true;
    scanAndLoad();
    if (mConfig.menuEnabled)
        registerMenus();
    mThread = std::thread([this] { threadMain(); });
}

void LuaHost::stop() {
    const bool wasRunning = mRunning.exchange(false);
    if (mThread.joinable())
        mThread.join();
    if (!wasRunning)
        return;

    std::lock_guard<std::recursive_mutex> lk(mMutex);
    for (auto &mp : mMods) {
        if (mp->menuRegistered)
            pl::modmenu::unregisterModule(mp->moduleId.c_str());
        saveStorage(*mp);
        if (mp->L) {
            lua_close(mp->L);
            mp->L = nullptr;
        }
    }
    mMods.clear();
    for (auto &c : mHandlerCounts)
        c = 0;
    log("INFO", "host", "Mod Host stopped");
    std::lock_guard<std::mutex> logLock(mLogMutex);
    if (mLogFile.is_open())
        mLogFile.close();
}

} // namespace luahost
