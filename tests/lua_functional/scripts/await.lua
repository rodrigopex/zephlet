-- Lua frontend: tasks that wait for events. Built with AWAIT_DEPTH=4.
--
-- probe_b also has a policy, so a policy and awaiting tasks share its events;
-- probe_a and probe_idle are only ever awaited.

local string = require("string")
local table = require("table")
local z = require("zephlets")

local passed, failed = 0, 0

local function check(cond, what)
	if cond then
		passed = passed + 1
	else
		failed = failed + 1
		print("FAIL " .. what)
	end
end

local function eq(actual, expected, what)
	check(actual == expected, string.format("%s: expected %s, got %s", what,
		tostring(expected), tostring(actual)))
end

local function raises(fn, pattern, what)
	local ok, err = pcall(fn)
	check(not ok and string.find(tostring(err), pattern, 1, true) ~= nil,
		string.format("%s: expected an error with '%s', got %s", what, pattern,
			ok and "no error" or tostring(err)))
end

local a, b, idle = z.probe_a, z.probe_b, z.probe_idle

-- Before the scheduler -----------------------------------------------------

raises(function() a:await(0) end, "only from a zephyr.async task", "await in the main chunk")
raises(function() z.await_any({a}, 0) end, "only from a zephyr.async task",
	"await_any in the main chunk")
raises(function() a.await(5) end, "expected a zephlet instance", "await needs : syntax")

local policy_seen = {}
b:add_policy(function(ev)
	policy_seen[#policy_seen + 1] = ev.seq
end)

-- The async main -------------------------------------------------------------

return function()
	local async = require("zephyr.async")

	-- Arguments.
	raises(function() a:await(-5) end, "timeout_ms must be", "negative timeout")
	raises(function() a:await(1.5) end, "timeout_ms must be", "fractional timeout")
	raises(function() a:await("x") end, "timeout_ms must be", "timeout type")
	raises(function() z.await_any({}, 0) end, "non-empty array", "await_any of nothing")
	raises(function() z.await_any({a, 3}, 0) end, "expected a zephlet instance",
		"await_any of a non-instance")

	-- Timeouts: nothing emitted.
	local ev, err = a:await(0)
	eq(ev, nil, "no wait: no event")
	eq(err, "timeout", "no wait: timeout")
	ev, err = a:await(30)
	eq(err, "timeout", "timed wait expires")

	-- Every event since the first await is kept, also while not waiting.
	a:burst({count = 3, value = 7}, 100)
	async.sleep(20)
	local got = {}
	for i = 1, 3 do
		local e, extra = a:await(0)
		got[i] = e and (tostring(e.value) .. (extra == nil and "" or "+")) or "nil"
	end
	eq(table.concat(got, ","), "7,7,7", "queued while away; await returns the event alone")
	eq(select(2, a:await(0)), "timeout", "queue empty again")

	-- A spawned task wakes on each event; two tasks both get every event.
	local function collector(inst, n)
		return async.spawn(function()
			local seqs = {}
			for i = 1, n do
				local e, err = inst:await(1000)
				if e == nil then
					return nil, err
				end
				seqs[i] = e.seq
			end
			return table.concat(seqs, ",")
		end)
	end
	local t1, t2 = collector(a, 4), collector(a, 4)
	async.yield() -- both park in their first await
	for _ = 1, 4 do
		a:burst({count = 1, value = 1}, 100)
		async.sleep(5)
	end
	local ok1, s1 = t1:join(500)
	local ok2, s2 = t2:join(500)
	eq(ok1 and s1, "4,5,6,7", "first task got every event")
	eq(ok2 and s2, "4,5,6,7", "second task got every event")
	-- The main task's own queue on probe_a filled meanwhile, in full.
	for _ = 1, 4 do
		a:await(0)
	end

	-- A policy and an awaiting task on one instance.
	local t3 = collector(b, 2)
	async.yield()
	b:burst({count = 2, value = 2}, 100)
	local ok3, s3 = t3:join(500)
	eq(ok3 and s3, "1,2", "awaiting task beside a policy")
	async.sleep(10)
	eq(table.concat(policy_seen, ","), "1,2", "policy still gets every event")

	-- The first await drains after it subscribes: an event of a connected
	-- instance still in staging reaches the new queue, with no yield between
	-- the emit and the await, and the policy gets it once.
	local first = async.spawn(function()
		b:burst({count = 1, value = 3}, 100)
		local e = b:await(100)
		return e and e.value
	end)
	local ok5, v5 = first:join(500)
	eq(ok5 and v5, 3, "first await gets the event still in staging")
	async.sleep(10)
	eq(table.concat(policy_seen, ","), "1,2,3", "the policy gets it once")

	-- A staged event of the wrong size is dropped and counted, not read.
	local fifo_drop = z.stats().probe_b.fifo_drop
	b:stage_short(100)
	async.sleep(10)
	eq(z.stats().probe_b.fifo_drop - fifo_drop, 1, "a short staged event is a counted drop")
	eq(#policy_seen, 3, "and never reaches the policy")

	-- await_any: several sources, oldest first.
	local any = async.spawn(function()
		local names = {}
		for i = 1, 3 do
			local e, inst = z.await_any({a, b}, 1000)
			names[i] = e and (inst == a and "probe_a" or inst == b and "probe_b" or "?")
				or tostring(inst)
		end
		return table.concat(names, ",")
	end)
	async.yield()
	a:burst({count = 1, value = 0}, 100)
	b:burst({count = 1, value = 0}, 100)
	a:burst({count = 1, value = 0}, 100)
	local ok4, s4 = any:join(500)
	eq(ok4 and s4, "probe_a,probe_b,probe_a", "await_any in arrival order")
	for _ = 1, 2 do
		a:await(0)
	end

	-- No timeout, or nil, waits forever.
	local woke = {}
	local function waiter(name, fn)
		return async.spawn(function()
			local e, inst = fn()
			woke[name] = true
			return e ~= nil and (inst == nil or inst == a)
		end)
	end
	local w1 = waiter("await()", function() return a:await() end)
	local w2 = waiter("await(nil)", function() return a:await(nil) end)
	local w3 = waiter("await_any()", function() return z.await_any({a, b}) end)
	local w4 = waiter("await_any(nil)", function() return z.await_any({a, b}, nil) end)
	async.sleep(200)
	check(next(woke) == nil, "no timeout: still parked after 200 ms")
	a:burst({count = 1, value = 0}, 100)
	for name, w in pairs({["await()"] = w1, ["await(nil)"] = w2,
			["await_any()"] = w3, ["await_any(nil)"] = w4}) do
		local ok, got = w:join(500)
		check(ok and got == true, name .. " without a timeout gets the event")
	end
	a:await(0)

	-- A finished task's queue does not keep collecting events: with the
	-- collector stopped, nothing frees it, so only the drain can tell.
	local once = async.spawn(function()
		return idle:await(0)
	end)
	once:join(500)
	collectgarbage("stop")
	local emitted = z.stats().probe_idle.emitted
	idle:burst({count = 6, value = 0}, 100)
	async.sleep(20)
	eq(z.stats().probe_idle.emitted - emitted, 6, "events emitted after the task finished")
	eq(z.stats().probe_idle.await_drop, 0, "no queue left to fill")
	collectgarbage("restart")

	-- A __gc that runs while a task's first await of probe_idle allocates its
	-- queue: it emits on probe_idle, then awaits probe_a, whose drain sees
	-- that event. The new queue must count as live by then, or the drain
	-- turns probe_idle's staging off under the parked task. Nothing makes the
	-- collector run there on demand, so finalizers keep re-arming and each try
	-- is a fresh task, until one fires inside that await.
	local inside, fired = false, false
	local gcmt = {}
	gcmt.__gc = function(o)
		if inside and not fired then
			fired = true
			idle:burst({count = 1, value = 0}, 0)
			a:await(0)
		end
		setmetatable(o, gcmt)
	end
	local pause = collectgarbage("param", "pause", 0)
	local stepsize = collectgarbage("param", "stepsize", 64)
	local tries, got = 0, nil
	while not fired and tries < 200 do
		tries = tries + 1
		for _ = 1, 8 do
			setmetatable({}, gcmt)
		end
		local t = async.spawn(function()
			inside = true
			idle:await(0)
			inside = false
			if not fired then
				return nil
			end
			while idle:await(0) ~= nil do
			end
			idle:burst({count = 1, value = 0}, 100)
			return idle:await(100) ~= nil
		end)
		local ok, r = t:join(500)
		got = ok and r
	end
	collectgarbage("param", "pause", pause)
	collectgarbage("param", "stepsize", stepsize)
	gcmt.__gc = nil
	collectgarbage()
	check(fired, "a __gc ran inside a first await")
	check(got == true, "a first await survives a __gc that drains")
	a:await(0)

	-- A full queue drops the newest, counted in z.stats(). Its source has no
	-- policy: awaiting alone turns its staging on.
	eq(select(2, idle:await(0)), "timeout", "idle queue starts empty")
	idle:burst({count = 6, value = 0}, 100)
	async.sleep(20)
	local n = 0
	while idle:await(0) ~= nil do
		n = n + 1
	end
	eq(n, 4, "a full queue keeps AWAIT_DEPTH events")
	eq(z.stats().probe_idle.await_drop, 2, "the rest are counted")
	idle:burst({count = 1, value = 0}, 100)
	check(idle:await(100) ~= nil, "delivery resumes after a full queue")

	print(string.format("lua suite await: %d passed, %d failed", passed, failed))
end
