-- Lua frontend: a main chunk that raises. zephlet_lua_run() returns -EINVAL
-- and nothing is scheduled.

local z = require("zephlets")

z.probe_a.events:connect(function() end)
error("the script failed during startup")
