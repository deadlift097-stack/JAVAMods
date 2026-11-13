#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <jni.h>

#include "mod/Config.h"

#include <pl/Mod.hpp>

namespace luahost {

// Connects the game process to the Java host (org.modhost.JavaHost) through JNI.
class JavaBridge {
  public:
    // Creates the class loader that can see the bridge classes.
    using LoaderFactory = std::function<jobject(JNIEnv *)>;

    static JavaBridge &instance();

    // Returns false (after logging why) when Java support is not available.
    bool start(ll::mod::NativeMod &self, const ModConfig &config, const std::filesystem::path &modsDir,
               const std::filesystem::path &dataDir, const std::filesystem::path &cacheDir,
               const LoaderFactory &loaderFactory);
    void stop();
    [[nodiscard]] bool running() const { return mRunning.load(); }

    bool onKey(int keyCode, bool down, unsigned int unicodeChar);
    bool onTouch(int action, int pointerId, float x, float y);
    bool onMouse(int button, bool down);
    void onAttack();

    // Called by the launcher's menu for modules registered by Java mods.
    void handleToggle(std::string_view moduleId, bool enabled);
    void handleConfigChanged(std::string_view moduleId, std::string_view key, std::string_view value);

    // Called from the Java side through Native.
    bool registerMenuFromJava(const std::string &moduleId, const std::string &name, const std::string &description,
                              bool enabled, const std::vector<std::string> &settings);
    void unregisterMenuFromJava(const std::string &moduleId);

    // Helper for the real game: builds an InMemoryDexClassLoader around embedded dex bytes.
    static jobject createInMemoryDexLoader(JNIEnv *env, const unsigned char *dex, size_t size);

  private:
    JavaBridge() = default;

    bool acquireVm();
    JNIEnv *env();
    bool checkException(JNIEnv *env, const char *where);
    jclass loadClass(JNIEnv *env, jobject loader, const char *dottedName);

    JavaVM *mVm = nullptr;
    ll::mod::NativeMod *mSelf = nullptr;
    std::atomic<bool> mRunning{false};
    jclass mHost = nullptr; // global refs
    jmethodID mOnKey = nullptr, mOnTouch = nullptr, mOnMouse = nullptr, mOnAttack = nullptr;
    jmethodID mOnToggle = nullptr, mOnConfig = nullptr, mStop = nullptr;
};

} // namespace luahost
