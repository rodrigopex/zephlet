-- Lua frontend: policies whose errors cannot be formatted. An error object
-- whose __tostring raises, or that is not a string at all, is still counted,
-- and the other policies and later events keep running.

local string = require("string")
local z = require("zephlets")

local passed, failed = 0, 0

local function eq(actual, expected, what)
	if actual == expected then
		passed = passed + 1
	else
		failed = failed + 1
		print(string.format("FAIL %s: expected %s, got %s", what, tostring(expected),
			tostring(actual)))
	end
end

local a = z.probe_a
local seen = 0

a:add_policy(function()
	error(setmetatable({}, {__tostring = function()
		error("error object formatting failed")
	end}))
end)
a:add_policy(function()
	error(setmetatable({}, {__tostring = function()
		return 42 -- not a string: tostring() itself raises
	end}))
end)
a:add_policy(function()
	seen = seen + 1
end)

return function()
	local async = require("zephyr.async")

	a:burst({count = 2, value = 0}, 100)
	async.sleep(50)
	eq(seen, 2, "the next policy ran for both events")
	eq(z.stats().probe_a.handler_err, 4, "every failed policy is counted")

	a:burst({count = 1, value = 0}, 100)
	async.sleep(50)
	eq(seen, 3, "later events are still dispatched")

	print(string.format("lua suite policy_error: %d passed, %d failed", passed, failed))
end
