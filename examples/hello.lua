-- hello.lua: a one-file Lua mod. Copy it to lua_host/mods/hello.lua
menu.register{
  id = "hello",
  name = "Hello Lua",
  description = "Example mod: logs key presses and greets you.",
  settings = {
    { key = "logKeys", label = "Log key presses", type = "toggle", default = false },
    { key = "greetDelay", label = "Greeting delay (s)", type = "slider", default = 3, min = 1, max = 30 },
  },
}

timer.after(settings.greetDelay * 1000, function()
  log.info("Hello from Lua! This is " .. mod.name)
end)

on("key", function(code, down)
  if settings.logKeys and down then
    log.info("key " .. code)
  end
  return false -- do not consume the key
end)
