#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mod/Config.h"

#include <pl/Mod.hpp>

struct lua_State;

namespace luahost {

struct LuaMod;

// Loads Lua mods from a folder and runs them in sandboxed Lua states.
class LuaHost {
  public:
    static LuaHost &instance();

    void start(ll::mod::NativeMod &self, const ModConfig &config);
    void stop();
    [[nodiscard]] bool running() const { return mRunning.load(); }
    [[nodiscard]] const std::filesystem::path &extDir() const { return mExtDir; }
    [[nodiscard]] const std::filesystem::path &dataDir() const { return mDataDir; }

    // Events. Key, touch and mouse return true when a Lua handler consumed the event.
    bool onKey(int keyCode, bool down, unsigned int unicodeChar);
    bool onTouch(int action, int pointerId, float x, float y);
    bool onMouse(int button, bool down);
    void onAttack();

    // Called by the launcher's menu.
    void handleToggle(std::string_view moduleId, bool enabled);
    void handleConfigChanged(std::string_view moduleId, std::string_view key, std::string_view value);

    // Used by the Lua API functions.
    void log(const char *level, const std::string &modId, const std::string &message);
    void noteHandler(const std::string &event);
    void saveStorage(LuaMod &mod);
    void prepareSettings(LuaMod &mod);

    // Crash-loop breaker: a ".pending" file written before risky work and removed
    // afterwards. If it is still there next start, that work crashed the game.
    bool guardTripped(const char *name);
    void guardSet(const char *name);
    void guardClear(const char *name);

  private:
    LuaHost();

    enum EventId { EvAttack = 0, EvKey, EvMouse, EvTouch, EvTick, EvCount };
    static int eventIndex(std::string_view name);

    void resolveDirs();
    void openLog();
    void scanAndLoad();
    void loadMod(const std::filesystem::path &mainFile, const std::filesystem::path &dir, bool single,
                 const std::string &defaultId);
    void loadStorage(LuaMod &mod);
    void recountHandlers();
    void registerMenus();
    LuaMod *findByModuleId(std::string_view moduleId);
    bool dispatch(const char *event, const std::function<int(lua_State *)> &pushArgs);
    void runTimers(int64_t now);
    void threadMain();

    std::recursive_mutex mMutex;
    std::mutex mLogMutex;
    std::atomic<bool> mRunning{false};
    std::thread mThread;
    ll::mod::NativeMod *mSelf = nullptr;
    ModConfig mConfig;
    std::filesystem::path mDataDir, mExtDir, mModsDir;
    std::ofstream mLogFile;
    std::vector<std::unique_ptr<LuaMod>> mMods;
    std::array<std::atomic<int>, EvCount> mHandlerCounts;
};

} // namespace luahost
