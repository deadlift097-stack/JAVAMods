#pragma once

#include <string>
#include <string_view>

#include <pl/Config.hpp>

namespace luahost {

struct ModConfig {
    int version = 1;
    bool enabled = true;
    // Register an in-game menu module for every Lua mod that calls menu.register.
    bool menuEnabled = true;
    // Load Java mods (.jar files with classes.dex) from the java_mods folder.
    bool javaMods = true;
    // Run on("tick") handlers about 20 times a second from a host thread.
    bool tickEvent = true;
    // Longest a single Lua call may run, in milliseconds.
    int maxScriptMs = 50;
    // Memory cap for each Lua mod, in megabytes.
    int maxMemoryMb = 32;
    // Install the attack hook so Lua mods get on("attack"). Off by default
    // because the hook target is not confirmed on every game build.
    bool attackEvent = false;
    // Optional override of the attack function offset (hex), e.g. "0x1009d290".
    std::string attackRva = "";
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace luahost

template <> struct pl::config::Schema<luahost::ModConfig> {
    static constexpr std::string_view title = "Mod Host Config";
    static constexpr std::string_view description = {};

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")
            return {.title = "Version", .readOnly = true};
        if (name == "enabled")
            return {.title = "Enabled", .description = "Turns Mod Host on or off."};
        if (name == "menuEnabled")
            return {.title = "In-game menu", .description = "Show a menu module for Lua mods that define settings."};
        if (name == "javaMods")
            return {.title = "Java mods", .description = "Load Java mods from the java_mods folder."};
        if (name == "tickEvent")
            return {.title = "Tick event", .description = "Run on(\"tick\") handlers about 20 times a second."};
        if (name == "maxScriptMs")
            return {.title = "Script time limit (ms)", .description = "A Lua call running longer than this is stopped."};
        if (name == "maxMemoryMb")
            return {.title = "Memory limit per mod (MB)", .description = "A Lua mod using more memory than this is stopped."};
        if (name == "attackEvent")
            return {.title = "Attack event", .description = "Hook the game's attack function so Lua mods get on(\"attack\")."};
        if (name == "attackRva")
            return {.title = "Attack RVA", .description = "Optional hex offset of the attack function, e.g. 0x1009d290."};
        return {};
    }
};
