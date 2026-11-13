-- hit_counter.lua: counts your attacks. Needs "attackEvent": true in Lua Host's config.
menu.register{
  id = "hit_counter",
  name = "Hit Counter",
  description = "Counts your attacks (written to the log).",
  settings = {
    { key = "logEvery", label = "Log every N hits", type = "slider", default = 5, min = 1, max = 50 },
  },
}

local total = storage.get("total", 0)

on("attack", function(info)
  total = total + 1
  storage.set("total", total)
  if total % settings.logEvery == 0 then
    log.info("hits so far: " .. total)
  end
end)
