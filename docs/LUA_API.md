# Mod Host API (version 1)

Mod Host loads small Lua mods inside LeviLauncher's Minecraft Bedrock. Each mod runs in its
own sandboxed Lua 5.4 state.

## Where mods go

The first time the game starts with Mod Host enabled it creates this folder:

    /storage/emulated/0/Android/media/<launcher package>/mod_host/mods/

(for the normal launcher: `.../Android/media/org.levimc.launcher/mod_host/mods/`)

A mod is either

* one file: `mods/hello.lua`
* or a folder: `mods/my_mod/main.lua`, with an optional `mod.json` next to it

Restart the game after adding or changing a mod. Errors and messages go to
`mod_host/mod_host.log` (next to the `mods` folder), so you can read them in a file manager.

`mod.json` (all fields optional):

    { "id": "my_mod", "name": "My Mod", "version": "1.0.0",
      "author": "you", "description": "What it does", "enabled": true }

## Globals

| Name | What it is |
|------|------------|
| `mod` | `id`, `name`, `version`, `author`, `description`, `dir`, `enabled` (false while its menu toggle is off) |
| `host` | `host.api` (1) and `host.version` |
| `settings` | Current values of the settings you declared in `menu.register` (`settings.myKey`) |
| `log.info(...)` `log.warn(...)` `log.error(...)` | Write to the host log. `print` does the same as `log.info`. |
| `time.ms()` | Milliseconds from a steady clock |
| `os.time` `os.clock` `os.date` `os.difftime` | The only parts of `os` that exist |
| `require("name")` | Folder mods only: loads `name.lua` from the mod's own folder |

Standard Lua `string`, `table`, `math`, `utf8` and `coroutine` libraries are available.
Not available (removed for safety): `io`, `package`, `debug`, `dofile`, `loadfile`, `load`,
and the rest of `os`.

## Events: `on(name, function)`

| Event | Arguments | Notes |
|-------|-----------|-------|
| `"key"` | `keyCode, isDown, unicodeChar` | Android key codes (111 = Escape). Return `true` to consume the key. |
| `"touch"` | `action, pointerId, x, y` | Return `true` to consume. |
| `"mouse"` | `button, isDown` | Return `true` to consume. |
| `"attack"` | `info` table with `time_ms` | Only if Mod Host's config has `"attackEvent": true` (see below). |
| `"tick"` | `dt_ms` | About 20 times a second from a host thread. This is NOT the game tick. |

Keep handlers short. Touch events can arrive many times a second.

## Menu: `menu.register{...}`

Call once per mod, at the top level of your script.

    menu.register{
      id = "my_mod",                  -- optional
      name = "My Mod",
      description = "Shown in the menu",
      enabled = true,                 -- starting state of the module toggle
      settings = {
        { key = "loud",   label = "Loud mode", type = "toggle", default = false },
        { key = "volume", label = "Volume",    type = "slider", default = 7,   min = 0, max = 10 },
        { key = "scale",  label = "Scale",     type = "float",  default = 0.5, min = 0, max = 1 },
      },
      on_toggle = function(enabled) end,        -- optional
      on_change = function(key, value) end,     -- optional
    }

* `toggle` values are booleans, `slider` values are integers, `float` values are numbers.
* Keys may only contain letters, digits and `_`. At most 64 settings.
* Current values are always in the `settings` table, and are remembered between runs.
* While the module's toggle is off, the mod gets no events and its timers pause.
* If the launcher's SDK has no change callback, menu edits cannot reach `on_change`
  (the host logs a warning at start).

## Storage

    storage.set("best", 12)       -- booleans, numbers and strings; nil deletes
    local best = storage.get("best", 0)

Saved automatically, per mod, in the mod's private data folder.

## Timers

    local id = timer.after(1000, function() ... end)    -- once, after 1000 ms
    local id = timer.every(500,  function() ... end)    -- repeating
    timer.cancel(id)

Minimum 10 ms, at most 200 timers per mod.

## Limits

* A single Lua call may run 50 ms (script load: 1 s). Longer calls are stopped with an error.
* 32 MB of memory per mod.
* After 5 errors a mod is stopped. See the log for the reason.
* These limits are set in Mod Host's config (`maxScriptMs`, `maxMemoryMb`).

## The attack event

`on("attack")` needs a hook into the game. It is off by default. To turn it on set
`"attackEvent": true` in Mod Host's `config.json` (the launcher's mod config editor works).
It is built for Minecraft Bedrock 1.26.62.1 only and checks the game's code bytes first, so
on any other build it stays off instead of crashing. It has not been confirmed in play yet.

## Crash guards

Mod Host writes a small `*.pending` file in its data folder before registering menus and
before the first calls of the attack hook, and deletes it afterwards. If the game crashed
there, the file is still present next start and that step is skipped. Delete the file to retry.

## What is NOT possible yet

The host only exposes what has been located in the game so far: input events, an
(unconfirmed) attack event, menus, timers, storage and logging. There is no HUD drawing, no
sound, no access to the world, entities, chat or commands. Those need more game functions
found first; they will be added to this API as they are.
