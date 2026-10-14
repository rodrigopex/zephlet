-- Lua frontend: the main suite.
--
-- The main chunk runs before the scheduler starts, so it covers discovery,
-- commands, argument checking and policies; the event checks run in the
-- async main this chunk returns, after the seal.

local coroutine = require("coroutine")
local math = require("math")
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

-- Discovery --------------------------------------------------------------

check(string.find(tostring(a), "probe_a (probe)", 1, true) ~= nil, "tostring names instance and type")
check(z.probe_a == a, "instance objects are cached")
raises(function() return z.no_such_instance end, "no instance named 'no_such_instance'", "missing instance")
raises(function() return a.no_such_field end, "has no field 'no_such_field'", "missing field")
raises(function() z.probe_a = 1 end, "read-only", "module is read-only")
raises(function() a.ping = 1 end, "read-only", "instance is read-only")
eq(z.FOREVER, -1, "FOREVER")

-- Enum constants ---------------------------------------------------------

eq(a.Mode.MODE_IDLE, 0, "enum MODE_IDLE")
eq(a.Mode.MODE_SLOW, 2, "enum MODE_SLOW")
check(a.Mode == b.Mode, "enum tables are shared per type")
raises(function() a.Mode.MODE_IDLE = 5 end, "read-only", "enum is read-only")

-- Commands ---------------------------------------------------------------

eq(a:ping(100), true, "Empty response returns true")

local st = a:get_status(z.FOREVER)
eq(st.is_ready, true, "get_status is_ready")
eq(st.is_running, false, "get_status is_running")
st = a:start(0)
eq(st.is_running, true, "start with K_NO_WAIT")

local cfg = {
	gain = -5,
	label = "hello",
	limit = 7,
	samples = {1, -2, 3},
	inner = {a = -1, b = 4000000},
	mode = a.Mode.MODE_FAST,
	blob = "\1\0\2",
	enabled = true,
	ratio = 0.5,
}
local resp = a:config(cfg, 100)
check(resp ~= nil, "config returns its response")
eq(resp.gain, -5, "int32")
eq(resp.label, "hello", "string")
eq(resp.limit, 7, "optional present")
eq(#resp.samples, 3, "repeated count")
eq(resp.samples[2], -2, "repeated element")
eq(resp.inner.a, -1, "nested int32")
eq(resp.inner.b, 4000000, "nested uint32")
eq(resp.mode, a.Mode.MODE_FAST, "enum")
eq(resp.blob, "\1\0\2", "bytes with an embedded NUL")
eq(resp.enabled, true, "bool")
eq(resp.ratio, 0.5, "float")

local got = a:get_config(100)
eq(got.label, "hello", "get_config reads back the config")
eq(b:get_config(100).label, "", "instances do not share config")

-- Every scalar shape, round-tripped through a command that echoes it back.
do
	local big = 2.0 ^ 40 -- beyond a 32-bit Lua integer: written and read as a float
	local w = a:echo({
		i64 = -big, u64 = big, s32 = -7, f32 = 3000000000.0, sf64 = -big,
		d = 0.5, i8 = -128, i16 = 32767, u8 = 255, u16 = 65535, tag4 = "a\0bc",
		r64 = {1, big, 3},
	}, 100)
	eq(w.i64, -big, "int64 beyond 32 bits")
	eq(w.u64, big, "uint64 beyond 32 bits")
	eq(math.type(w.u64), "float", "uint64 beyond 32 bits reads as a float")
	eq(w.s32, -7, "sint32")
	eq(w.f32, 3000000000.0, "fixed32 above 2^31 reads as a float")
	eq(w.sf64, -big, "sfixed64")
	eq(w.d, 0.5, "double")
	eq(w.i8, -128, "int8 at its minimum")
	eq(w.i16, 32767, "int16 at its maximum")
	eq(w.u8, 255, "uint8 at its maximum")
	eq(w.u16, 65535, "uint16 at its maximum")
	eq(w.tag4, "a\0bc", "fixed-length bytes, NUL included")
	eq(#w.r64, 3, "repeated uint64 count")
	eq(w.r64[2], big, "repeated uint64 beyond 32 bits")
	eq(w.p_int, nil, "unset oneof member is absent")
	eq(w.p_inner, nil, "other unset oneof member is absent")

	local o = a:echo({p_inner = {a = 5, b = 6}}, 100)
	eq(o.p_inner and o.p_inner.b, 6, "oneof submessage member")
	eq(o.p_int, nil, "only the member that is set")
	eq(a:echo({p_int = 9}, 100).p_int, 9, "oneof scalar member")
	eq(a:echo({}, 100).tag4, "\0\0\0\0", "fixed-length bytes left unset")

	-- The 64-bit bounds, exact as floats whether lua_Number is a float or a
	-- double: the largest float below 2^63 and 2^64 is 2^39 or 2^40 under it.
	local p63, p64 = 2.0 ^ 63, 2.0 ^ 64
	eq(a:echo({u64 = p63}, 100).u64, p63, "uint64 2^63")
	eq(a:echo({u64 = p64 - 2.0 ^ 40}, 100).u64, p64 - 2.0 ^ 40, "uint64 near 2^64")
	eq(a:echo({i64 = -p63}, 100).i64, -p63, "int64 minimum, -2^63")
	eq(a:echo({i64 = p63 - 2.0 ^ 39}, 100).i64, p63 - 2.0 ^ 39, "int64 near 2^63")
	eq(a:echo({sf64 = -p63}, 100).sf64, -p63, "sfixed64 minimum, -2^63")
	eq(a:echo({r64 = {p63, 0, 1}}, 100).r64[1], p63, "repeated uint64 2^63")
	eq(a:echo({u64 = -0.0}, 100).u64, 0, "uint64 negative zero")

	local cases = {
		{{u64 = p64}, "field 'u64': value out of range"},
		{{u64 = -1.0}, "field 'u64': value out of range"},
		{{u64 = math.huge}, "field 'u64': value out of range"},
		{{u64 = -0.5}, "field 'u64': expected integer, got non-integral number"},
		{{u64 = 2.5}, "field 'u64': expected integer, got non-integral number"},
		{{u64 = 0 / 0}, "field 'u64': expected integer, got non-integral number"},
		{{i64 = p63}, "field 'i64': value out of range"},
		{{i64 = -p64}, "field 'i64': value out of range"},
		{{i64 = -math.huge}, "field 'i64': value out of range"},
		{{i64 = 0 / 0}, "field 'i64': expected integer, got non-integral number"},
		{{i8 = 1e30}, "field 'i8': value out of range"},
		{{u8 = 255.5}, "field 'u8': expected integer, got non-integral number"},
		{{i8 = 128}, "field 'i8': value out of range"},
		{{i8 = -129}, "field 'i8': value out of range"},
		{{i16 = -32769}, "field 'i16': value out of range"},
		{{u8 = 256}, "field 'u8': value out of range"},
		{{u16 = -1}, "field 'u16': value out of range"},
		{{u64 = -1}, "field 'u64': value out of range"},
		{{i64 = 1.5}, "field 'i64': expected integer, got non-integral number"},
		{{i64 = "1"}, "field 'i64': expected integer, got string"},
		{{d = "x"}, "field 'd': expected number, got string"},
		{{tag4 = "abc"}, "field 'tag4': expected exactly 4 bytes"},
		{{r64 = {1, 2, 3, 4}}, "field 'r64': more than 3 elements"},
		{{r64 = 1}, "field 'r64': expected array, got number"},
		{{p_inner = 1}, "field 'p_inner': expected table, got number"},
		{{p_int = 1, p_inner = {}}, "another member of its oneof is already set"},
		{{[1] = 0}, "message field names must be strings"},
	}
	for _, c in ipairs(cases) do
		raises(function() a:echo(c[1], 100) end, c[2], c[2])
	end
end

-- A __gc metamethod that runs a command can fire inside another command,
-- while it builds its response: it must not overwrite that response.
do
	local nested_bad, guarded, corrupt = 0, 0, 0
	local gcmt = {__gc = function()
		local ok, r = pcall(b.get_config, b, 0)
		if not ok then
			if string.find(tostring(r), "cannot run while another", 1, true) then
				guarded = guarded + 1
			else
				nested_bad = nested_bad + 1
			end
		elseif r.label ~= "" then
			nested_bad = nested_bad + 1
		end
	end}
	-- A collector that never pauses and steps every few bytes, so finalizers
	-- run inside the commands' own allocations. Each step does plenty of
	-- work, so the collector keeps up and the heap does not fill.
	local pause = collectgarbage("param", "pause", 0)
	local stepsize = collectgarbage("param", "stepsize", 64)
	local stepmul = collectgarbage("param", "stepmul", 1000)
	for _ = 1, 200 do
		for _ = 1, 4 do
			setmetatable({}, gcmt)
		end
		if a:get_config(0).label ~= "hello" then
			corrupt = corrupt + 1
		end
	end
	collectgarbage("param", "pause", pause)
	collectgarbage("param", "stepsize", stepsize)
	collectgarbage("param", "stepmul", stepmul)
	collectgarbage()
	eq(corrupt, 0, "a command's response survives a __gc that runs a command")
	eq(nested_bad, 0, "a command run from __gc gets its own response, or the guard")
	check(guarded > 0, "some __gc ran mid-conversion and was refused")
end

resp = a:config({gain = 1}, 100)
eq(resp.limit, nil, "optional absent is nil")
eq(resp.inner, nil, "unset submessage is nil")
eq(#resp.samples, 0, "empty repeated is an empty array")
eq(resp.label, "", "unset string is empty")

-- Command errors ---------------------------------------------------------

local ok, err, rc = a:fail({code = -22}, 100)
eq(ok, nil, "error: first result")
eq(err, "EINVAL", "error: errno name")
eq(rc, -22, "error: rc")
ok, err, rc = a:stop(100)
eq(err, "ENOSYS", "unimplemented rpc")
check(ok == nil and rc < 0, "unimplemented rpc rc")

-- Invalid arguments ------------------------------------------------------

raises(function() a:ping() end, "expected (timeout_ms)", "missing timeout")
raises(function() a:ping(-2) end, "timeout_ms must be", "negative timeout")
raises(function() a:ping(1.5) end, "timeout_ms must be", "fractional timeout")
raises(function() a:ping("10") end, "timeout_ms must be", "string timeout")
raises(function() a:ping(10, 10) end, "expected (timeout_ms)", "extra argument")
raises(function() a:config(100) end, "expected (req, timeout_ms)", "missing request")
raises(function() a:config("x", 100) end, "table expected", "request not a table")
raises(function() a.ping(100) end, "call it as a method", "dot call")
raises(function() a:config({nope = 1}, 100) end, "unknown field 'nope'", "unknown field")
raises(function() a:config({gain = "x"}, 100) end, "field 'gain': expected integer", "wrong type")
raises(function() a:config({gain = 1.5}, 100) end, "non-integral", "fractional integer")
raises(function() a:config({limit = -1}, 100) end, "field 'limit': value out of range", "unsigned range")
raises(function() a:config({label = string.rep("x", 16)}, 100) end, "longer than 15", "string too long")
raises(function() a:config({samples = {1, 2, 3, 4, 5}}, 100) end, "more than 4", "too many elements")
raises(function() a:config({inner = 3}, 100) end, "field 'inner': expected table", "submessage type")
raises(function() a:config({inner = {c = 1}}, 100) end, "unknown field 'c'", "nested unknown field")
raises(function() a:config({blob = string.rep("x", 9)}, 100) end, "bytes longer than 8", "bytes too long")
raises(function() a:config({enabled = 1}, 100) end, "expected boolean", "bool type")
raises(function() a:add_policy(1) end, "function expected", "add_policy needs a function")
raises(function() a.add_policy(print) end, "call it as a method", "add_policy needs : syntax")

-- Policies ---------------------------------------------------------------

-- Emitted before the seal: staged, and delivered once dispatch starts.
a:burst({count = 1, value = 1}, 100)

local log = {}
local routes = {}
local names = {[a] = "probe_a", [b] = "probe_b"}
local first_ev
local threads = {}
local in_handler = false
local overlapped = false

local function record(tag)
	return function(ev, inst)
		if in_handler then
			overlapped = true
		end
		first_ev = first_ev or ev
		log[#log + 1] = string.format("%s:%s:%d", tag, names[inst], ev.seq)
		routes[#routes + 1] = inst
		threads[#threads + 1] = coroutine.running()
	end
end

a:add_policy(record("a1"))
a:add_policy(function(ev)
	in_handler = true
	if ev.value == 9 then
		-- Park the dispatcher; events keep arriving meanwhile.
		require("zephyr.async").sleep(30)
	end
	in_handler = false
	log[#log + 1] = string.format("a2:%d:%s", ev.seq, tostring(ev.alarm))
end)
b:add_policy(function(ev)
	error("boom " .. ev.seq)
end)
b:add_policy(record("b2"))

-- The async main ---------------------------------------------------------

return function()
	local async = require("zephyr.async")

	local function wait_for(n, what)
		for _ = 1, 100 do
			if #log >= n then
				return
			end
			async.sleep(10)
		end
		check(false, "timed out waiting for " .. what)
	end

	raises(function() a:add_policy(print) end, "policies are fixed", "add_policy after the seal")

	wait_for(2, "the event emitted before the seal")
	eq(log[1], "a1:probe_a:1", "pre-seal event, first handler")
	eq(log[2], "a2:1:false", "pre-seal event, second handler")

	-- Event objects: fields read from the C struct, read-only.
	eq(first_ev.seq, 1, "event scalar field")
	eq(first_ev.mode, a.Mode.MODE_IDLE, "event enum field")
	eq(first_ev.inner.a, 0, "event submessage field")
	eq(first_ev.inner.b, 0, "event submessage, second field")
	check(first_ev.inner ~= first_ev.inner, "a submessage is built on each read")
	raises(function() return first_ev.nope end, "probe event has no field 'nope'", "unknown event field")
	raises(function() first_ev.seq = 2 end, "events are read-only", "events are read-only")
	local fields = {}
	for k in pairs(first_ev) do
		fields[#fields + 1] = k
	end
	eq(table.concat(fields, ","), "seq,value,mode,alarm,inner", "pairs() over an event")
	check(string.find(tostring(first_ev), "probe", 1, true) ~= nil, "tostring names the type")

	-- Two sources, one coroutine, in emission order.
	a:burst({count = 2, value = 2}, 100)
	b:burst({count = 2, value = 3}, 100)
	wait_for(8, "two bursts")
	eq(table.concat(log, ",", 3, 8),
		"a1:probe_a:2,a2:2:false,a1:probe_a:3,a2:3:true,b2:probe_b:1,b2:probe_b:2",
		"routing and order")
	eq(routes[2], a, "a policy gets its instance")
	eq(routes[4], b, "the instance of the second source")
	check(threads[1] == threads[4] and threads[1] ~= coroutine.running(),
		"every handler runs in the one dispatcher coroutine")

	-- A handler error is counted, and the next handler still runs.
	eq(z.stats().probe_b.handler_err, 2, "handler errors counted")
	eq(z.stats().probe_b.delivered, 2, "probe_b delivered")

	-- A parked handler: events that arrive meanwhile wait their turn.
	a:burst({count = 3, value = 9}, 100)
	wait_for(14, "events behind a parked handler")
	check(not overlapped, "a handler is never re-entered while parked")
	eq(log[10], "a2:4:false", "first event after the park")
	eq(log[14], "a2:6:false", "last event behind the park")

	-- A source with no policy is filtered, not counted as dropped.
	idle:burst({count = 3, value = 0}, 100)
	async.sleep(20)
	local s = z.stats().probe_idle
	eq(s.emitted, 3, "source without policy still counts emits")
	eq(s.delivered, 0, "source without policy is not delivered")
	eq(s.staged_drop + s.fifo_drop, 0, "events without policy are not drops")

	print(string.format("lua suite basic: %d passed, %d failed", passed, failed))
end
