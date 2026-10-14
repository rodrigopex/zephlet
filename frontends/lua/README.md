# Lua frontend

`CONFIG_ZEPHLETS_LUA=y` exposes every zephlet instance to a Lua script as the
`zephlets` module. It covers commands, events and enum constants. Zephlet code
and the core C API stay unchanged. The scheduler is lua_zephyr's
`zephyr.async`; this frontend adds no scheduler of its own.

```lua
local z = require("zephlets")

local tick      = z.tick_timer_based_impl
local tampering = z.tampering_emul_impl
local ui        = z.ui_fake_impl

tick:add_policy(function(ev, meta)
    ui:blink(100)
end)

tampering:add_policy(function(ev, meta)
    if ev.proximity_tamper_detected then
        ui:blink(100)
    end
end)
```

```cmake
# CMakeLists.txt, after find_package(Zephyr): precompile src/app.lua into
# app_lua_bytecode.h (lua_zephyr's luaz.cmake).
luaz_add_bytecode_file(src/app.lua)
```

```c
#include <zephlet_lua.h>

#include "app_lua_bytecode.h"

int main(void)
{
	/* C setup first: this call does not return while the script runs. */
	return zephlet_lua_coordinate(ZEPHLET_LUA_SCRIPT(app));
}
```

## Setup

1. Put [lua_zephyr](https://github.com/zano-systems/lua_zephyr) in your west
   manifest. Pin it by SHA, and set `submodules: true` because the Lua core is
   a submodule. This frontend was developed and tested against
   `58c47e5bdb40f7cdaea3a05db2b5fcc81226b5a6`, the head of
   `feat/async-coroutine-scheduler`. If the module is missing,
   `CONFIG_ZEPHLETS_LUA` cannot be enabled (`depends on LUA_ASYNC`) and
   nothing Lua is built.
2. Kconfig:

   ```
   CONFIG_LUA=y
   CONFIG_LUA_ASYNC=y
   CONFIG_ZEPHLETS_LUA=y
   # Libraries the script uses, e.g. CONFIG_LUA_LIB_STRING=y,
   # or all of them with CONFIG_LUA_LIBS_ALL=y.
   ```

3. Embed the script with `luaz_add_bytecode_file(<path>.lua)` and call
   `zephlet_lua_coordinate(ZEPHLET_LUA_SCRIPT(<name>))` from `main()`.
   `CONFIG_ZEPHLETS_LUA` selects `CONFIG_LUA_PRECOMPILE`, so a host `luac` is
   built and every script is compiled to stripped bytecode at build time,
   the frontend's own tasks included. Add `CONFIG_LUA_PRECOMPILE_ONLY=y` to
   leave the Lua parser out of the image (about 20 KB of flash on Cortex-M3).
   `zephlet_lua_coordinate(buf, len, chunkname)` also takes source, for
   example a script read from a filesystem, when the parser is kept.

No other wiring is needed. The per-type hook that `ZEPHLET_NEW(...)` already
expands registers every instance with the frontend. Instances are found by
name in an iterable section; the application declares no list of them.

## The `zephlets` module

| Expression | Meaning |
|---|---|
| `z.<instance>` | The instance object. An unknown name raises an error. |
| `inst:<rpc>(timeout_ms)` | A command whose request is `Empty`. |
| `inst:<rpc>(req, timeout_ms)` | A command with a request table. |
| `inst:add_policy(fn)` | Adds a policy: `fn(ev, meta)` runs for every event of `inst`. |
| `inst.<Enum>.<VALUE>` | Proto enum constants, read-only. Proto names are kept verbatim. |
| `z.FOREVER` | `-1`, the `timeout_ms` that means `K_FOREVER`. |
| `z.stats()` | Per-instance counters, keyed by instance name. |

The module, the instances and the enum tables are all read-only.

### Commands

```lua
local cfg, err, rc = tick:config({duration_ms = 0, period_ms = 1000}, 500)
if not cfg then
    print("config failed", err, rc)    -- e.g. nil, "EINVAL", -22
end
```

- **`timeout_ms` is required**: a plain integer, where `0` means `K_NO_WAIT`
  and `z.FOREVER` means `K_FOREVER`. There is no default. It bounds the wait
  for the command channel, exactly as in the C wrappers.
- A command goes through the generated typed wrapper (`<type>_<rpc>()`), the
  same path a C caller takes. The handler runs on the caller's thread, which
  here is the thread that called `zephlet_lua_coordinate()`.
- **Success** returns the response table, or `true` for `Empty`. **Failure**
  (`rc != 0`) returns `nil, "<ERRNO>", rc`.
- **A command cannot start while another converts its request or response**,
  which only a `__gc` metamethod that calls a command can attempt: it raises
  instead of overwriting the first command's data.
- **Invalid arguments raise an error** instead of returning one, because they
  are bugs in the script, not runtime conditions. That covers a wrong
  argument count, a missing or invalid timeout, an unknown field, a value of
  the wrong type, an integer out of range, a string or bytes value too long,
  too many repeated elements, two members of one oneof, and calling with `.`
  instead of `:`.

### Messages as tables

Request, response and event tables follow the proto field names:

| Proto | Lua |
|---|---|
| scalar (proto3 implicit presence) | always present; `0`/`false`/`""` when unset |
| `optional` scalar, submessage | `nil` when absent |
| `repeated` | array (`{}` when empty) |
| enum | integer; compare with `inst.<Enum>.<VALUE>` |
| `string`, `bytes` | Lua string; `bytes` may contain NUL |
| `oneof` | only the member that is set |
| `float`, `double` | number |

The conversion walks the nanopb-textformat descriptors that codegen already
emits for every message. No second schema parser is involved. Those exist only
for the messages of the zephlet's own proto (and `Lifecycle.Status`), so an rpc
whose request or response is imported from another proto fails codegen for a
type with the `lua` option, rather than converting as `{}`.

**Integers are 32-bit.** lua_zephyr builds Lua with `LUA_32BITS`, so a Lua
integer is 32-bit and a Lua number is a `float`. Reading a `uint32` above
`2^31 - 1`, or a 64-bit value outside the `int32` range, gives a number, which
may round. Writing one of those values only works as an integral number, with
the same caveat. The field's own range is checked exactly: `-2^63` fits an
`int64` and `2^63` a `uint64`, while `2^64`, infinities and NaN are rejected.
`double` fields carry float precision.

### Events and policies

```lua
tick:add_policy(function(ev, meta)
    -- ev:   the event table, e.g. ev.timestamp
    -- meta: {source = <instance>, name = "tick_timer_based_impl",
    --        seq = <per-instance number>, dropped = <events lost before this one>}
end)
```

- **Policies are static.** `add_policy` works only while the script's main
  chunk runs. Once the chunk returns the policies are sealed, and a later
  `add_policy` raises an error. An instance may have any number of policies.
  They run in the order they were added, and the lists live on the Lua heap.
  `add_policy` is a member of every instance, so codegen rejects an rpc or
  enum of that name.
- **One persistent coroutine** runs every policy of every instance. No
  coroutine is created per event. Policies run one at a time, in the order the
  events were emitted.
- **A policy may park**, for example in `require("zephyr.async").sleep(ms)`.
  It is never resumed with anything other than its own reply; that is
  `zephyr.async`'s tagged-yield guarantee. While it is parked, later events
  wait in the FIFO and are handled in order afterwards. Do not call
  `coroutine.yield()` directly in a policy: an untagged yield ends the
  dispatcher task.
- **A policy that raises** has its error and traceback logged, and its
  instance's `handler_err` counter goes up. The remaining policies, and later
  events, still run.
- **Events emitted before the seal**, for example during boot or from the
  main chunk itself, are kept in staging, as far as its depth allows. After
  the seal they go to the instances that have policies. Staged events from
  instances with none are discarded without being counted as drops.
  After the seal, those instances' events are filtered out in the listener.

### The main chunk and the async main

The main chunk runs **synchronously, before the scheduler starts**. It can
`require`, call commands and `add_policy`, but `zephyr.async` is not available
yet. To run background work, return a function from the chunk. It becomes one
more task when the loop starts:

```lua
return function()
    local async = require("zephyr.async")
    while true do
        async.sleep(10000)
        print(z.stats().tampering_emul_impl.staged_drop)
    end
end
```

`zephlet_lua_coordinate()` returns only on failure:

| Return | Cause |
|---|---|
| `-EALREADY` | The frontend was already started, by any thread. |
| `-ENOMEM` | The Lua state could not be created. |
| `-ENOEXEC` | The script did not load (a syntax error, which is logged). |
| `-EINVAL` | The main chunk raised, or returned something other than nil or a function. |
| `-EFAULT` | The scheduler stopped on a fault. |

## Architecture

```
zephlet emit ─► chan_<inst>_events
                    │  (zbus async listener, system workqueue)
                    ▼
        forwarding listener ── copy ──► C staging slots ── doorbell ──► zlet_lua_bell
                                            (fixed, RAM)                (zbus channel)
                                                                             │
                ┌───────────────── zephyr.async loop, owner thread ──────────┘
                ▼
     pump task ── drain ──► Lua-heap FIFO ──► dispatcher task ──► policy(ev, meta)
```

- **No Lua runs in zbus listener context.** The forwarding listener, emitted
  per instance by the generated hook, copies the event struct into a staging
  slot under a spinlock and returns. It never blocks the workqueue. The Lua
  state, its `sys_heap` allocator and the scheduler state are owned by one
  thread and are not thread-safe, so this copy is the only cross-thread step.
- **The doorbell carries no data.** It is the one channel `zephyr.async`
  watches for the frontend. Its user data is a `lua_msg_descr`, as
  `chan:await` requires; zephlet channels carry a `struct zephlet *` there and
  cannot be awaited directly. A burst rings it once. A lost or coalesced
  doorbell loses no event, because each wakeup drains everything staged.
- **Two internal tasks.** Both the pump and the dispatcher drain staging into
  the Lua-heap FIFO, converting each event to a table with its source's
  descriptor. The pump exists so that staging keeps emptying while the
  dispatcher is parked inside a policy. The dispatcher is the one persistent
  application coroutine.
- **The scheduler never holds pointers into event buffers.** The listener
  copies, and the drain copies again into a table. The zbus buffer is released
  as soon as the listener returns.

### Initialisation order

1. The zephlet SYS_INIT walker initialises the instances (APPLICATION level).
   From then on their events are staged.
2. `zephlet_lua_coordinate()`, called from `main()`, creates the Lua state and
   registers `zephlets`.
3. The main chunk runs: policies, plus any setup commands.
4. The seal freezes the policies and marks the instances that have one.
5. The `zephyr.async` loop starts the pump, the dispatcher and the optional
   async main.

## Limits and memory

| Kconfig | Default | Cost and effect |
|---|---|---|
| `ZEPHLETS_LUA_HEAP_SIZE` | 32768 | Static RAM for the Lua heap: the script, the queued event tables and policy lists. |
| `ZEPHLETS_LUA_SLOT_SIZE` | 64 | Payload of one staging slot. Every events struct must fit, which is checked at build time. |
| `ZEPHLETS_LUA_STAGING_DEPTH` | 8 | Staging slots. Static RAM is `(SLOT_SIZE + 8) × DEPTH`. |
| `ZEPHLETS_LUA_FIFO_DEPTH` | 32 | Events queued as Lua tables. Costs 6 B of static RAM each, plus each queued table on the Lua heap. |
| `ZEPHLETS_LUA_SCRATCH_SIZE` | 256 | Two static buffers for a command's request and response. Every RPC must fit, which is checked at build time. |
| `ZEPHLETS_LUA_POLL_MS` | 1000 | How long an event can wait unnoticed after a refused doorbell. `0` turns the poll off. |
| `MAIN_STACK_SIZE` | 8192 (overridden) | The Lua VM and every command's handler run on this stack. |

Each instance also has one record in RAM (flags, counters, last sequence
number) and one async listener.

The frontend uses 2 of `CONFIG_LUA_ASYNC_MAX_TASKS` tasks, 3 with an async
main; this is checked at build time. It uses one of
`CONFIG_LUA_ASYNC_MAX_CHANNELS` and one runtime zbus observer.

**When a queue is full, the newest event is dropped and the drop is
reported, never silently.**

| Where | Counter | Also |
|---|---|---|
| Staging full | `staged_drop` | `LOG_WRN` on the 1st, 2nd, 4th, 8th, … drop |
| FIFO full, or out of memory while converting | `fifo_drop` | Same |
| Doorbell refused (zbus message pool exhausted) | `doorbell_fail` | Recovered by the next event or by `POLL_MS` |

The next event delivered from that source reports what was lost before it, as
`meta.dropped`. That value is the gap in the source's sequence numbers, so it
is exact. `z.stats()` also reports `emitted`, `delivered` and `handler_err`.

**Allocations on the event path.** The listener allocates nothing on the Lua
heap. Per delivered event, the drain allocates the event table (and its nested
tables) and `pop` allocates the `meta` table. Both are bounded by
`FIFO_DEPTH`, and both run under protected calls: an out-of-memory error drops
the event and counts it, and the scheduler does not fault. A zbus net_buf
carries each event to the async listener, and another carries the doorbell.
Both come from `ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE`, which you may need to
raise for bursts. lua_zephyr's allocator prints "Something went wrong with
allocation" whenever an allocation fails, including ones Lua then recovers
from with an emergency GC. Frequent messages mean the heap is too small.

## LuaLS

Each build writes `build/zephlets_lua/`:

- `zephlets_base.lua`: shared classes (`zephlets.EventMeta`,
  `zephlets.Stats`, `pb.Lifecycle.Status`).
- `<prefix>_lua_meta.lua`, one per zephlet type, generated from its proto:
  message classes with optional/repeated markers, `---@enum`, the instance
  class with every command's signature, and a typed `add_policy`.
- `zephlets_instances.lua`: the `zephlets` module with one field per instance.
  Codegen never sees instance names, which exist only in `ZEPHLET_NEW(...)`
  calls, so this file is written *after the link*. The per-instance record's
  symbol is `__zlet_lua__<type>__<instance>`, and a post-build step reads
  those symbols from the ELF. The file therefore lists exactly the instances
  that were built.

Point LuaLS at it in the script's directory:

```json
{
  "runtime.version": "Lua 5.5",
  "workspace.library": ["../build/zephlets_lua"]
}
```

After that, `require("zephlets")` is typed. You get completion of instance
names, command names, request and event fields and enum values, and a
warning on a misspelled name.

## Known limitations

- The script must be started from one thread, which then owns the Lua state.
  Starting it a second time returns `-EALREADY`.
- Commands block that thread for their duration, and with it every Lua task.
  They are synchronous zbus calls, like the C wrappers.
- Scripts are stripped bytecode (`luac -s`), so Lua error messages carry no
  line numbers.
- 32-bit Lua numbers; see [Messages as tables](#messages-as-tables).
- Pointer- and callback-allocated nanopb fields, and `FIXARRAY` fields, are
  not converted. They are the same shapes nanopb-textformat skips.
- `coroutine.yield()` called directly in a policy ends the dispatcher. Park
  with `zephyr.async` calls instead.
- lua_zephyr pins Lua 5.5.0 (MIT), the `v5.5.0` release tag. The lua_zephyr
  repository has no LICENSE file yet, although its README states Apache-2.0.
