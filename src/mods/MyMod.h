#pragma once

#include <cstdint>
#include <optional>

#include "mod/Config.h"

#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

namespace luahost {

class LuaHostMod {
  public:
    static LuaHostMod &instance();

    LuaHostMod();

    [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

  private:
    uintptr_t resolveAttackTarget();
    void startJava();
    void installInput();
    void installAttackHook();

    pl::memory::HookHandle mAttackHook;
    ll::mod::NativeMod &mSelf;
    ModConfig mConfig;
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile;
};

} // namespace luahost
