-- Lua frontend: a main chunk that raises. zephlet_lua_coordinate() returns
-- -EINVAL and nothing is scheduled; main() then checks that events are no
-- longer staged.

local z = require("zephlets")

z.probe_a.events:connect(function() end)
error("the script failed during startup")
