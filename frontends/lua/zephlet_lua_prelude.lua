-- Lua side of the zephlets frontend, run by zephlet_lua_coordinate() before
-- the script's main chunk. Only what has to park a zephyr.async task is here;
-- the rest is C (core). It returns:
--
-- pump      task: moves staged events to their consumers, so staging keeps
--           emptying while a policy is parked (e.g. in async.sleep).
-- dispatch  task: the one persistent policy coroutine. Pops the FIFO and runs
--           the policies of each event's source, in order. A policy that parks
--           parks this task; later events wait in the FIFO.
-- await     inst:await([timeout_ms]), for the script's own tasks.
-- await_any zephlets.await_any(insts[, timeout_ms]).
--
-- Every waiter parks on the doorbell channel. It carries no data, so a
-- coalesced or lost doorbell loses no event; POLL_MS, when set, bounds how
-- long one can go unnoticed.

local core = ...
local POLL = core.POLL_MS

local traceback
do
	local ok, dbg = pcall(require, "debug")
	if ok and type(dbg) == "table" then
		traceback = dbg.traceback
	end
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
		core.drain()
		b:await(POLL)
	end
end

-- An error object as a string, whatever it is: its __tostring may raise too,
-- and nothing may escape the dispatcher's loop.
local function describe(err)
	local ok, msg = pcall(tostring, err)
	if ok and type(msg) == "string" then
		return msg
	end
	return "(error object could not be converted to a string)"
end

local function call(handler, ev, inst)
	if traceback then
		return xpcall(handler, traceback, ev, inst)
	end
	return pcall(handler, ev, inst)
end

local function dispatch()
	local b = attach()
	while true do
		core.drain()
		local ok, handlers, ev, inst = pcall(core.pop)
		if not ok then
			core.lost(describe(handlers))
		elseif handlers == nil and ev == nil then
			core.idle()
			b:await(POLL)
		elseif handlers ~= nil then
			for i = 1, #handlers do
				local hok, err = call(handlers[i], ev, inst)
				if not hok then
					core.handler_error(inst, describe(err))
				end
			end
		end
	end
end

-- core.prepare() returns the deadline and the task's queues; core.take()
-- returns an event and its instance, a timeout, or how long to park.
local function wait(deadline, ...)
	local b = attach()
	while true do
		local ev, inst, park = core.take(deadline, ...)
		if ev ~= nil or inst ~= nil then
			return ev, inst
		end
		b:await(park)
	end
end

local function await(inst, ms)
	local ev, err = wait(core.prepare(inst, ms, false))
	if ev ~= nil then
		return ev
	end
	return nil, err
end

local function await_any(insts, ms)
	return wait(core.prepare(insts, ms, true))
end

return {pump = pump, dispatch = dispatch, await = await, await_any = await_any}
