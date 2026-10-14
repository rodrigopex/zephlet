-- Internal tasks of the zephlets Lua frontend; started by
-- zephlet_lua_coordinate().
--
-- pump     moves staged events into the Lua-heap FIFO, so staging keeps
--          emptying while a handler is parked (e.g. in async.sleep).
-- dispatch the one persistent application coroutine: pops the FIFO and runs
--          the handlers of each event's source, in order. A handler that
--          parks parks this task; later events wait in the FIFO.
--
-- Both await the doorbell channel. It carries no data, so a coalesced or lost
-- doorbell loses no event; POLL_MS, when set, bounds how long one can go
-- unnoticed.

local core = ...
local POLL = core.POLL_MS

local traceback
do
	local ok, dbg = pcall(require, "debug")
	if ok and type(dbg) == "table" then
		traceback = dbg.traceback
	end
end

local function attach()
	require("zephyr.async")
	local bell = core.bell()
	-- Watch the doorbell before the first drain, so a wakeup between the two
	-- cannot be missed.
	bell:await(0)
	return bell
end

local function pump()
	local bell = attach()
	while true do
		core.drain()
		bell:await(POLL)
	end
end

local function call(handler, ev, meta)
	if traceback then
		return xpcall(handler, traceback, ev, meta)
	end
	return pcall(handler, ev, meta)
end

local function dispatch()
	local bell = attach()
	while true do
		core.drain()
		local ok, handlers, ev, meta = pcall(core.pop)
		if not ok then
			core.lost(handlers)
		elseif handlers == nil and ev == nil then
			bell:await(POLL)
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

return pump, dispatch
