-- Lua frontend: queue limits. Built with STAGING_DEPTH=4 and FIFO_DEPTH=2.
--
-- A burst of 8 is emitted while the Lua thread is busy in the command, so
-- staging keeps the first 4 and drops 4. The first drain moves those 4 into a
-- FIFO of 2 and drops 2 more. Every drop is counted, and the next delivered
-- event reports the gap in meta.dropped.

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

z.probe_a.events:connect(function(ev, meta)
	seen[#seen + 1] = {seq = ev.seq, dropped = meta.dropped}
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
	eq(seen[1].seq, 1, "first delivered")
	eq(seen[1].dropped, 0, "nothing lost before the first")
	eq(seen[2].seq, 2, "second delivered")

	-- The next event carries everything lost since seq 2.
	z.probe_a:burst({count = 1, value = 0}, 100)
	async.sleep(50)
	eq(#seen, 3, "delivery resumes after overflow")
	eq(seen[3].seq, 9, "next event")
	eq(seen[3].dropped, 6, "meta.dropped reports the gap")

	print(string.format("lua suite overflow: %d passed, %d failed", passed, failed))
end
