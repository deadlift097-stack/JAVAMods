#include "mod/JavaBridge.h"

#include <dlfcn.h>

#include <string>
#include <vector>

#include <pl/ModMenu.hpp>

#include "mod/LuaHost.h"

namespace luahost {

namespace {

JavaVM *gCachedVm = nullptr;

std::string toStd(JNIEnv *env, jstring s) {
    if (!s)
        return {};
    const char *c = env->GetStringUTFChars(s, nullptr);
    if (!c)
        return {};
    std::string out(c);
    env->ReleaseStringUTFChars(s, c);
    return out;
}

// ---- native methods bound to org.modhost.Native

jboolean JNICALL nativeRegisterMenu(JNIEnv *env, jclass, jstring id, jstring name, jstring desc, jboolean enabled,
                                    jobjectArray settings) {
    std::vector<std::string> list;
    if (settings) {
        const jsize n = env->GetArrayLength(settings);
        for (jsize i = 0; i < n; ++i) {
            jstring s = static_cast<jstring>(env->GetObjectArrayElement(settings, i));
            list.push_back(toStd(env, s));
            if (s)
                env->DeleteLocalRef(s);
        }
    }
    const bool ok = JavaBridge::instance().registerMenuFromJava(toStd(env, id), toStd(env, name), toStd(env, desc),
                                                                enabled != JNI_FALSE, list);
    return ok ? JNI_TRUE : JNI_FALSE;
}

void JNICALL nativeUnregisterMenu(JNIEnv *env, jclass, jstring id) {
    JavaBridge::instance().unregisterMenuFromJava(toStd(env, id));
}

void JNICALL nativeLog(JNIEnv *env, jclass, jstring level, jstring mod, jstring message) {
    const std::string lv = toStd(env, level);
    LuaHost::instance().log(lv.c_str(), toStd(env, mod), toStd(env, message));
}

// ---- menu callbacks for Java-registered modules

void javaToggleThunk(std::string_view moduleId, bool enabled) {
    JavaBridge::instance().handleToggle(moduleId, enabled);
}

void javaConfigThunk(std::string_view moduleId, std::string_view key, std::string_view value) {
    JavaBridge::instance().handleConfigChanged(moduleId, key, value);
}

// Attach the change callback only if this SDK's builder accepts one with this shape.
template <class B> bool attachConfigCallback(B &builder) {
    if constexpr (requires { builder.onConfigChanged(&javaConfigThunk); }) {
        builder.onConfigChanged(&javaConfigThunk);
        return true;
    } else {
        return false;
    }
}

struct MenuEntry {
    std::string key, label, type, value, min, max;
};

std::vector<std::string> splitUnit(const std::string &s) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '\x1f') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    gCachedVm = vm;
    return JNI_VERSION_1_6;
}

JavaBridge &JavaBridge::instance() {
    static JavaBridge *bridge = new JavaBridge(); // intentionally never destroyed
    return *bridge;
}

bool JavaBridge::acquireVm() {
    if (mVm)
        return true;
    if (gCachedVm) {
        mVm = gCachedVm;
        return true;
    }
    using GetVmsFn = jint (*)(JavaVM **, jsize, jsize *);
    auto fn = reinterpret_cast<GetVmsFn>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!fn)
        return false;
    JavaVM *vm = nullptr;
    jsize count = 0;
    if (fn(&vm, 1, &count) != JNI_OK || count < 1 || !vm)
        return false;
    mVm = vm;
    return true;
}

JNIEnv *JavaBridge::env() {
    if (!mVm)
        return nullptr;
    JNIEnv *e = nullptr;
    const jint r = mVm->GetEnv(reinterpret_cast<void **>(&e), JNI_VERSION_1_6);
    if (r == JNI_OK)
        return e;
    if (r == JNI_EDETACHED) {
#ifdef __ANDROID__
        if (mVm->AttachCurrentThread(&e, nullptr) == JNI_OK)
            return e;
#else
        if (mVm->AttachCurrentThread(reinterpret_cast<void **>(&e), nullptr) == JNI_OK)
            return e;
#endif
    }
    return nullptr;
}

bool JavaBridge::checkException(JNIEnv *e, const char *where) {
    if (!e->ExceptionCheck())
        return false;
    jthrowable t = e->ExceptionOccurred();
    e->ExceptionClear();
    std::string text = "(unknown Java exception)";
    if (t) {
        jclass thr = e->FindClass("java/lang/Throwable");
        if (thr) {
            jmethodID toString = e->GetMethodID(thr, "toString", "()Ljava/lang/String;");
            if (toString) {
                jstring js = static_cast<jstring>(e->CallObjectMethod(t, toString));
                if (!e->ExceptionCheck()) {
                    text = toStd(e, js);
                    if (js)
                        e->DeleteLocalRef(js);
                } else {
                    e->ExceptionClear();
                }
            }
            e->DeleteLocalRef(thr);
        }
        e->DeleteLocalRef(t);
    }
    LuaHost::instance().log("ERROR", "java", std::string(where) + ": " + text);
    return true;
}

jclass JavaBridge::loadClass(JNIEnv *e, jobject loader, const char *dottedName) {
    jclass clClass = e->FindClass("java/lang/ClassLoader");
    if (!clClass) {
        checkException(e, "ClassLoader lookup");
        return nullptr;
    }
    jmethodID load = e->GetMethodID(clClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = e->NewStringUTF(dottedName);
    jobject cls = load ? e->CallObjectMethod(loader, load, name) : nullptr;
    e->DeleteLocalRef(name);
    e->DeleteLocalRef(clClass);
    if (checkException(e, dottedName))
        return nullptr;
    return static_cast<jclass>(cls);
}

jobject JavaBridge::createInMemoryDexLoader(JNIEnv *e, const unsigned char *dex, size_t size) {
    if (!dex || size == 0)
        return nullptr;
    jclass clClass = e->FindClass("java/lang/ClassLoader");
    jmethodID getSystem = clClass ? e->GetStaticMethodID(clClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;")
                                  : nullptr;
    jobject parent = getSystem ? e->CallStaticObjectMethod(clClass, getSystem) : nullptr;
    jobject buffer = e->NewDirectByteBuffer(const_cast<unsigned char *>(dex), static_cast<jlong>(size));
    jclass loaderClass = e->FindClass("dalvik/system/InMemoryDexClassLoader");
    jobject loader = nullptr;
    if (loaderClass && buffer) {
        jmethodID ctor = e->GetMethodID(loaderClass, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        if (ctor)
            loader = e->NewObject(loaderClass, ctor, buffer, parent);
    }
    if (e->ExceptionCheck())
        e->ExceptionClear();
    if (loaderClass)
        e->DeleteLocalRef(loaderClass);
    if (buffer)
        e->DeleteLocalRef(buffer);
    if (parent)
        e->DeleteLocalRef(parent);
    if (clClass)
        e->DeleteLocalRef(clClass);
    return loader;
}

bool JavaBridge::start(ll::mod::NativeMod &self, const ModConfig &config, const std::filesystem::path &modsDir,
                       const std::filesystem::path &dataDir, const std::filesystem::path &cacheDir,
                       const LoaderFactory &loaderFactory) {
    LuaHost &host = LuaHost::instance();
    if (mRunning.load())
        return true;
    mSelf = &self;
    if (!acquireVm()) {
        host.log("WARN", "java", "Java mods unavailable: could not find the Java VM");
        return false;
    }
    JNIEnv *e = env();
    if (!e) {
        host.log("WARN", "java", "Java mods unavailable: could not attach to the Java VM");
        return false;
    }
    jobject loader = loaderFactory ? loaderFactory(e) : nullptr;
    if (checkException(e, "creating the class loader") || !loader) {
        host.log("ERROR", "java", "Java mods unavailable: could not create the class loader (needs Android 8+ and the bridge dex)");
        return false;
    }
    jclass hostLocal = loadClass(e, loader, "org.modhost.JavaHost");
    jclass nativeCls = loadClass(e, loader, "org.modhost.Native");
    if (!hostLocal || !nativeCls) {
        host.log("ERROR", "java", "Java mods unavailable: bridge classes not found");
        return false;
    }

    JNINativeMethod methods[3];
    methods[0] = {const_cast<char *>("registerMenu0"),
                  const_cast<char *>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z[Ljava/lang/String;)Z"),
                  reinterpret_cast<void *>(&nativeRegisterMenu)};
    methods[1] = {const_cast<char *>("unregisterMenu0"), const_cast<char *>("(Ljava/lang/String;)V"),
                  reinterpret_cast<void *>(&nativeUnregisterMenu)};
    methods[2] = {const_cast<char *>("log0"),
                  const_cast<char *>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"),
                  reinterpret_cast<void *>(&nativeLog)};
    if (e->RegisterNatives(nativeCls, methods, 3) != JNI_OK || checkException(e, "RegisterNatives")) {
        host.log("ERROR", "java", "Java mods unavailable: could not bind native methods");
        return false;
    }

    mHost = static_cast<jclass>(e->NewGlobalRef(hostLocal));
    mOnKey = e->GetStaticMethodID(mHost, "onKey", "(IZI)Z");
    mOnTouch = e->GetStaticMethodID(mHost, "onTouch", "(IIFF)Z");
    mOnMouse = e->GetStaticMethodID(mHost, "onMouse", "(IZ)Z");
    mOnAttack = e->GetStaticMethodID(mHost, "onAttack", "()V");
    mOnToggle = e->GetStaticMethodID(mHost, "onToggle", "(Ljava/lang/String;Z)V");
    mOnConfig = e->GetStaticMethodID(mHost, "onConfigChanged",
                                     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    mStop = e->GetStaticMethodID(mHost, "stop", "()V");
    jmethodID setApi = e->GetStaticMethodID(mHost, "setNativeApi", "(Lorg/modhost/NativeApi;)V");
    jmethodID startId = e->GetStaticMethodID(mHost, "start",
                                             "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V");
    jmethodID nativeCtor = e->GetMethodID(nativeCls, "<init>", "()V");
    if (checkException(e, "looking up Java methods") || !mOnKey || !mOnTouch || !mOnMouse || !mOnAttack ||
        !mOnToggle || !mOnConfig || !mStop || !setApi || !startId || !nativeCtor) {
        host.log("ERROR", "java", "Java mods unavailable: bridge classes do not match this build");
        e->DeleteGlobalRef(mHost);
        mHost = nullptr;
        return false;
    }

    jobject nativeObj = e->NewObject(nativeCls, nativeCtor);
    e->CallStaticVoidMethod(mHost, setApi, nativeObj);
    e->DeleteLocalRef(nativeObj);

    jstring jMods = e->NewStringUTF(modsDir.string().c_str());
    jstring jData = e->NewStringUTF(dataDir.string().c_str());
    jstring jCache = e->NewStringUTF(cacheDir.string().c_str());
    e->CallStaticVoidMethod(mHost, startId, jMods, jData, jCache, config.tickEvent ? JNI_TRUE : JNI_FALSE);
    e->DeleteLocalRef(jMods);
    e->DeleteLocalRef(jData);
    e->DeleteLocalRef(jCache);
    e->DeleteLocalRef(hostLocal);
    e->DeleteLocalRef(nativeCls);
    e->DeleteLocalRef(loader);
    if (checkException(e, "starting the Java host")) {
        mRunning = true; // let stop() clean up whatever did load
        stop();
        return false;
    }
    mRunning = true;
    host.log("INFO", "java", "Java host running");
    return true;
}

void JavaBridge::stop() {
    if (!mRunning.exchange(false))
        return;
    JNIEnv *e = env();
    if (e && mHost && mStop) {
        e->CallStaticVoidMethod(mHost, mStop);
        checkException(e, "stopping the Java host");
        e->DeleteGlobalRef(mHost);
    }
    mHost = nullptr;
}

bool JavaBridge::onKey(int keyCode, bool down, unsigned int unicodeChar) {
    if (!mRunning.load())
        return false;
    JNIEnv *e = env();
    if (!e)
        return false;
    const jboolean r = e->CallStaticBooleanMethod(mHost, mOnKey, static_cast<jint>(keyCode),
                                                  down ? JNI_TRUE : JNI_FALSE, static_cast<jint>(unicodeChar));
    if (checkException(e, "key event"))
        return false;
    return r != JNI_FALSE;
}

bool JavaBridge::onTouch(int action, int pointerId, float x, float y) {
    if (!mRunning.load())
        return false;
    JNIEnv *e = env();
    if (!e)
        return false;
    const jboolean r = e->CallStaticBooleanMethod(mHost, mOnTouch, static_cast<jint>(action),
                                                  static_cast<jint>(pointerId), static_cast<jdouble>(x),
                                                  static_cast<jdouble>(y));
    if (checkException(e, "touch event"))
        return false;
    return r != JNI_FALSE;
}

bool JavaBridge::onMouse(int button, bool down) {
    if (!mRunning.load())
        return false;
    JNIEnv *e = env();
    if (!e)
        return false;
    const jboolean r =
        e->CallStaticBooleanMethod(mHost, mOnMouse, static_cast<jint>(button), down ? JNI_TRUE : JNI_FALSE);
    if (checkException(e, "mouse event"))
        return false;
    return r != JNI_FALSE;
}

void JavaBridge::onAttack() {
    if (!mRunning.load())
        return;
    JNIEnv *e = env();
    if (!e)
        return;
    e->CallStaticVoidMethod(mHost, mOnAttack);
    checkException(e, "attack event");
}

void JavaBridge::handleToggle(std::string_view moduleId, bool enabled) {
    JNIEnv *e = env();
    if (!e || !mHost)
        return;
    jstring id = e->NewStringUTF(std::string(moduleId).c_str());
    e->CallStaticVoidMethod(mHost, mOnToggle, id, enabled ? JNI_TRUE : JNI_FALSE);
    e->DeleteLocalRef(id);
    checkException(e, "menu toggle");
}

void JavaBridge::handleConfigChanged(std::string_view moduleId, std::string_view key, std::string_view value) {
    JNIEnv *e = env();
    if (!e || !mHost)
        return;
    jstring id = e->NewStringUTF(std::string(moduleId).c_str());
    jstring k = e->NewStringUTF(std::string(key).c_str());
    jstring v = e->NewStringUTF(std::string(value).c_str());
    e->CallStaticVoidMethod(mHost, mOnConfig, id, k, v);
    e->DeleteLocalRef(id);
    e->DeleteLocalRef(k);
    e->DeleteLocalRef(v);
    checkException(e, "menu change");
}

bool JavaBridge::registerMenuFromJava(const std::string &moduleId, const std::string &name,
                                      const std::string &description, bool enabled,
                                      const std::vector<std::string> &settings) {
    LuaHost &host = LuaHost::instance();
    if (!mSelf)
        return false;
    if (host.guardTripped("java_menu.pending")) {
        host.log("WARN", "java", "The menu crashed the game last time; skipping it. Delete java_menu.pending in the mod's data folder to retry.");
        return false;
    }
    host.guardSet("java_menu.pending");

    std::vector<MenuEntry> entries;
    for (const std::string &line : settings) {
        const std::vector<std::string> p = splitUnit(line);
        if (p.size() < 6)
            continue;
        entries.push_back({p[0], p[1], p[2], p[3], p[4], p[5]});
    }

    pl::modmenu::ModuleBuilder builder(moduleId.c_str(), name.c_str());
    builder.modId(mSelf->getId());
    builder.description(description.c_str());
    builder.defaultEnabled(enabled);
    builder.onToggle(&javaToggleThunk);
    for (const MenuEntry &s : entries) {
        if (s.type == "toggle") {
            builder.config(s.key.c_str(), s.label.c_str(), pl::modmenu::ConfigType::Toggle, s.value.c_str());
        } else {
            builder.config(s.key.c_str(), s.label.c_str(),
                           s.type == "slider" ? pl::modmenu::ConfigType::SliderInt
                                              : pl::modmenu::ConfigType::SliderFloat,
                           s.value.c_str(), s.min.c_str(), s.max.c_str());
        }
    }
    if (!attachConfigCallback(builder))
        host.log("WARN", "java", "this SDK has no menu change callback; menu edits will not reach Java mods");
    const bool ok = builder.registerModule();
    host.guardClear("java_menu.pending");
    return ok;
}

void JavaBridge::unregisterMenuFromJava(const std::string &moduleId) {
    pl::modmenu::unregisterModule(moduleId.c_str());
}

} // namespace luahost
