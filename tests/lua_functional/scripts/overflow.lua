-- Lua frontend: queue limits. Built with STAGING_DEPTH=4 and FIFO_DEPTH=2.
--
-- A burst of 8 is emitted while the Lua thread is busy in the command, so
-- staging keeps the first 4 and drops 4. The first drain moves those 4 into a
-- FIFO of 2 and drops 2 more. Every drop is counted in z.stats().
--
-- Before that, in the main chunk: staging overfills before the seal with
-- events of probe_idle, which nothing consumes; the newest are kept, so
-- probe_b's event, emitted last, still reaches its policy.

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

local seen = {}

local b_seen = 0
z.probe_b:add_policy(function()
	b_seen = b_seen + 1
end)
z.probe_idle:burst({count = 6, value = 0}, 100)
z.probe_b:burst({count = 1, value = 0}, 100)
require("zephyr").msleep(50) -- let the listeners stage them all

z.probe_a:add_policy(function(ev)
	seen[#seen + 1] = ev.seq
end)

return function()
	local async = require("zephyr.async")

	async.sleep(20)
	eq(b_seen, 1, "an event staged last before the seal is kept")
	eq(z.stats().probe_b.staged_drop, 0, "nothing of probe_b dropped")
	eq(z.stats().probe_idle.staged_drop, 0, "overwritten unconsumed events not counted")

	z.probe_a:burst({count = 8, value = 0}, 100)
	async.sleep(50)

	local s = z.stats().probe_a
	eq(s.emitted, 8, "emitted")
	eq(s.staged_drop, 4, "staging drops")
	eq(s.fifo_drop, 2, "fifo drops")
	eq(s.delivered, 2, "delivered")
	eq(#seen, 2, "handler calls")
	eq(seen[1], 1, "first delivered")
	eq(seen[2], 2, "second delivered")

	z.probe_a:burst({count = 1, value = 0}, 100)
	async.sleep(50)
	eq(#seen, 3, "delivery resumes after overflow")
	eq(seen[3], 9, "next event")
	s = z.stats().probe_a
	eq(s.staged_drop + s.fifo_drop, 6, "no new drops")

	print(string.format("lua suite overflow: %d passed, %d failed", passed, failed))
end
