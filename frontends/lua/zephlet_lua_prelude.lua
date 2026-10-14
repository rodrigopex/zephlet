-- Lua side of the zephlets frontend, run by zephlet_lua_coordinate() before
-- the script's main chunk. It returns:
--
-- pump      task: moves staged events to their consumers, so staging keeps
--           emptying while a handler is parked (e.g. in async.sleep).
-- dispatch  task: the one persistent policy coroutine. Pops the FIFO and runs
--           the policies of each event's source, in order. A policy that parks
--           parks this task; later events wait in the FIFO.
-- await     inst:await([timeout_ms]), for the script's own tasks.
-- await_any zephlets.await_any(insts[, timeout_ms]).
--
-- Every waiter awaits the doorbell channel. It carries no data, so a
-- coalesced or lost doorbell loses no event; POLL_MS, when set, bounds how
-- long one can go unnoticed.
--
-- An awaiting task gets its own queue per instance, created by its first
-- await on it: every event from then on is queued, up to AWAIT_DEPTH, and
-- the newest is dropped when the queue is full. meta.dropped reports the gap.

local core = ...
local POLL = core.POLL_MS
local DEPTH = core.AWAIT_DEPTH
local FOREVER = core.FOREVER

local traceback
do
	local ok, dbg = pcall(require, "debug")
	if ok and type(dbg) == "table" then
		traceback = dbg.traceback
	end
end

-- [instance index] = {[task thread] = queue}. Weak keys: a finished task's
-- queues go with it.
local subs = {}
local WEAK = {__mode = "k"}
-- Arrival order across instances, for await_any.
local serial = 0

local function route(idx, ev, seq)
	local s = subs[idx]
	if s == nil then
		return
	end
	serial = serial + 1
	for _, q in pairs(s) do
		if q.n == DEPTH then
			core.await_drop(idx)
		else
			local tail = (q.head + q.n - 1) % DEPTH + 1
			q.ev[tail], q.seq[tail], q.ser[tail] = ev, seq, serial
			q.n = q.n + 1
		end
	end
end

local function drain()
	core.drain(route)
end

local bell
local function attach()
	if bell == nil then
		require("zephyr.async")
		bell = core.bell()
	end
	-- Watch the doorbell before the drain that follows, so a wakeup between
	-- the two cannot be missed.
	bell:await(0)
	return bell
end

local function pump()
	local b = attach()
	while true do
		drain()
		b:await(POLL)
	end
end

local function call(handler, ev, meta)
	if traceback then
		return xpcall(handler, traceback, ev, meta)
	end
	return pcall(handler, ev, meta)
end

local function dispatch()
	local b = attach()
	while true do
		drain()
		local ok, handlers, ev, meta = pcall(core.pop)
		if not ok then
			core.lost(handlers)
		elseif handlers == nil and ev == nil then
			b:await(POLL)
		elseif handlers ~= nil then
			for i = 1, #handlers do
				local hok, err = call(handlers[i], ev, meta)
				if not hok then
					core.handler_error(meta.name, tostring(err))
				end
			end
		end
	end
end

-- ----- await -------------------------------------------------------------

local function check_timeout(ms, what)
	if ms == nil or ms == FOREVER then
		return FOREVER
	end
	if type(ms) ~= "number" or ms // 1 ~= ms or ms < 0 or ms > 0x7FFFFFFF then
		error(what .. ": timeout_ms must be nil, an integer >= 0, or zephlets.FOREVER", 3)
	end
	return ms // 1
end

-- The calling task's queue for inst, created on its first await.
local function queue(inst, task, what)
	local idx, why = core.index(inst)
	if why == "instance" then
		error(what .. ": expected a zephlet instance, got " .. type(inst), 3)
	elseif why == "sealed" then
		error(what .. ": only from a zephyr.async task, once the script's main chunk"
			.. " has returned", 3)
	end
	local s = subs[idx]
	local q = s and s[task]
	if q ~= nil then
		return q
	end

	-- Hand the events staged so far to their current consumers first: the
	-- new queue starts with the next one.
	drain()
	local name, seq = core.subscribe(idx)
	if s == nil then
		s = setmetatable({}, WEAK)
		subs[idx] = s
	end
	q = {source = inst, name = name, last = seq, head = 1, n = 0, ev = {}, seq = {}, ser = {}}
	s[task] = q
	return q
end

local function take(qs)
	local q
	for i = 1, #qs do
		local c = qs[i]
		if c.n > 0 and (q == nil or c.ser[c.head] - q.ser[q.head] < 0) then
			q = c
		end
	end
	if q == nil then
		return nil
	end

	local h = q.head
	local ev, seq = q.ev[h], q.seq[h]
	q.ev[h] = nil
	q.head = h % DEPTH + 1
	q.n = q.n - 1
	local meta = {source = q.source, name = q.name, seq = seq, dropped = core.gap(seq, q.last)}
	q.last = seq
	return ev, meta
end

local function wait(qs, ms)
	local b = attach()
	local deadline = ms > 0 and core.now() + ms or nil
	while true do
		drain()
		local ev, meta = take(qs)
		if ev ~= nil then
			return ev, meta
		end
		if ms == 0 then
			return nil, "timeout"
		end
		local park = POLL
		if deadline ~= nil then
			local left = deadline - core.now()
			if left <= 0 then
				return nil, "timeout"
			end
			if park == nil or left < park then
				park = left
			end
		end
		b:await(park)
	end
end

local function await(inst, ms)
	ms = check_timeout(ms, "await")
	return wait({queue(inst, core.self(), "await")}, ms)
end

local function await_any(insts, ms)
	if type(insts) ~= "table" or #insts == 0 then
		error("await_any: expected a non-empty array of instances", 2)
	end
	ms = check_timeout(ms, "await_any")
	local task = core.self()
	local qs = {}
	for i = 1, #insts do
		qs[i] = queue(insts[i], task, "await_any")
	end
	return wait(qs, ms)
end

return {pump = pump, dispatch = dispatch, await = await, await_any = await_any}
