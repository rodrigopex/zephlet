-- Lua frontend: a main chunk that returns neither nil nor a function.
-- zephlet_lua_coordinate() returns -EINVAL and nothing is scheduled.

local z = require("zephlets")

z.probe_a:add_policy(function() end)
return 42
