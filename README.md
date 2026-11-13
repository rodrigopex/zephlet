# Zephlet Infrastructure Module (v0.3)

[![CI](https://github.com/rodrigopex/zephlet/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/rodrigopex/zephlet/actions/workflows/ci.yml)

Reusable framework for building domain-isolated components on Zephyr RTOS, communicating exclusively over zbus. Each **zephlet** is a non-singleton, instance-per-`ZEPHLET_NEW` module with two channels: a synchronous pointer-based command channel and a value-typed events channel.

See [CLAUDE.md](CLAUDE.md) for the full architecture reference.

## Quick start

### 1. `west.yml`

```yaml
manifest:
  projects:
    - name: zephlet
      url: https://github.com/rodrigopex/zephlet
      revision: v0.3.4
      path: modules/lib/zephlet
      west-commands: west/west-commands.yml

    # Needed with CONFIG_ZEPHLETS_SHELL=y or CONFIG_ZEPHLETS_LUA=y.
    # Omit this entry only when both frontends are disabled.
    - name: zephyr-nanopb-textformat
      url: https://github.com/rodrigopex/zephyr-nanopb-textformat
      revision: v0.8.1
      path: modules/lib/zephyr-nanopb-textformat
  self:
    path: app
```

### 2. Bootstrap

```bash
west init -l .
west update --narrow --fetch-opt=--depth=1
west packages pip --install
```

### 3. Create a zephlet

From anywhere you want the zephlet to land:

```bash
cd path/to/wherever
west zephlet new -n <Name> -d "<description>" -a "<author>"
```

or non-interactively with an explicit destination:

```bash
west zephlet new -o path/to/wherever -n <Name> -d "<description>" -a "<author>"
```

Two further options shape the layout:

- `--prefix STR` — file-name prefix for the generated sources. Default `zlet_` (so a zephlet `tick` produces `zlet_tick.{c,h,proto}`); pass `--prefix=` to drop the prefix entirely (`tick.{c,h,proto}`). The prefix also flows into the header guard, log module name, and the `PREFIX` argument of `zephyr_zephlet_generate` so the generated `<prefix>_interface.{h,c}` stays consistent.
- `--no-module` — produce a minimal scaffold with just `CMakeLists.txt`, `Kconfig`, and the source files. Skips the `tests/integration/` folder and `zephyr/module.yml`, leaving you free to wire the directory into your app however you like (e.g. as part of a larger module rather than as a standalone Zephyr module).

The scaffold has no opinion on where zephlets must live — the Copier template drops a complete module under the destination directory. Users wire it into their app by adding its path to `EXTRA_ZEPHYR_MODULES` and enabling `CONFIG_ZEPHLET_<NAME>=y`.

### 4. App wiring

```cmake
set(EXTRA_ZEPHYR_MODULES "${CMAKE_SOURCE_DIR}/path/to/my_zephlet" ...)
```

Instantiate and use:

```c
#include "zlet_my_zephlet.h"

static struct my_zephlet_data my_data;
static struct my_zephlet_config my_cfg = { /* ... */ };
ZEPHLET_NEW(my_zephlet, my_instance, &my_cfg, &my_data, my_zephlet_init_fn);

/* ... in main or elsewhere ... */
struct lifecycle_status st;
my_zephlet_start(&my_instance, &st, K_MSEC(500));
```

## Architecture at a glance

- **`command` channel** (pointer, listener-only): synchronous command via zbus sync-listener. Wrapper returns the handler's rc directly — no correlation IDs, no semaphores, no result struct.
- **`events` channel** (value-typed): async fan-out. Publishers call `<type>_emit(z, &ev, timeout)`; consumers observe with `ZEPHLET_EVENTS_LISTENER(instance, type, callback)` (wraps `ZBUS_ASYNC_LISTENER_DEFINE`).
- **Non-singleton**: multiple instances per type coexist; each has its own channel pair and data.
- **Weak handler overrides**: generator emits `__weak int <type>_<cmd>_impl(...)` returning `-ENOSYS`; user provides strong overrides in `<prefix>.c`.
- **Composition**: a zephlet may declare, in its proto, the provider types it calls (`(zephlet.requires)`); the app binds concrete provider instances per consumer instance, and the implementation calls them through their public RPC wrappers — see [Composition](#composition). A zephlet still never names another zephlet instance.
- **Coordinators** (optional, `CONFIG_ZEPHLETS_COORD=y`): multi-step flows with workqueue dispatch + bounded zbus-event awaits. Sits above the per-zephlet command/events surface — see [Coordinators](#coordinators) below.
- **Frontends** (optional): expose the same command surface over a transport, with no change to domain code — each hooks itself in from `ZEPHLET_NEW` via a per-type macro. Shell (`CONFIG_ZEPHLETS_SHELL=y`, see [Shell frontend](#shell-frontend)) and CoAP (`CONFIG_ZEPHLETS_COAP=y`, per-service opt-in, see [Frontend options](#frontend-options)). Rationale in [ADR-0001](docs/adr/0001-zephlet-frontends.md).

## Frontend options

CoAP and Lua expose only the zephlet types that opt in. As with nanopb's
own [generator options](https://jpa.kapsi.fi/nanopb/docs/reference.html#generator-options),
an opt-in can be set in either of two places.

In the `.proto`, as a service option declared by `zephlet_options.proto`:

```proto
import "zephlet_options.proto";

service TickApi {
  option (zephlet.coap) = true;
  option (zephlet.lua) = true;
  ...
}
```

Or in an options file beside the proto, `<prefix>.zephlet.options`, which
keeps the proto plain protobuf, portable to other generators. Its syntax is
that of nanopb's `.options` files: `#`, `//` and `/* */` comments, and
`<pattern> option:value ...` lines, the pattern an `fnmatch()` mask over
the service name, a later line overriding an earlier one.

```
# zlet_tick.zephlet.options
TickApi coap:true lua:true
```

Both can be used together. As in nanopb, an option set in the proto
overrides the options file. (nanopb's own `<prefix>.options` cannot hold
these: it rejects keys it does not know.)

| Option | Effect |
|---|---|
| `coap` | Expose the type over the CoAP frontend. |
| `coap_discoverable` | Also list its instances in `/.well-known/core`. Needs `coap`, and every `LifecycleApi` method. |
| `lua` | Expose the type to the Lua frontend. |

Every option is `false` unless set, and a missing file sets none. An unknown
option, a value other than `true` or `false`, or an options file line whose
pattern matches no service stops the build, with the file and line for the
options file.

### Per-RPC options

One option is set on an rpc rather than on the service: in the rpc's body, or
on an options file line whose pattern is `<service>.<rpc>`, the rpc half an
`fnmatch()` mask over the rpc names.

```proto
service TickApi {
  rpc config(Tick.Config) returns (Tick.Config) {
    option (zephlet.shell) = false;
  }
  ...
}
```

```
# zlet_tick.zephlet.options
TickApi.config shell:false
TickApi.get_* shell:false
```

| Option | Effect |
|---|---|
| `shell` | Register the rpc as a `zlet <instance> <rpc>` subcommand. Default `true`. |

`shell:false` hides the rpc from the shell only: it keeps its method id and
stays callable from C, CoAP and Lua. A response's pasteable command line names
the first shown rpc that takes its type as a request, and is left out when no
shown rpc does (hiding `config` leaves `get_config` without one); an instance
whose rpcs are all hidden (`TickApi.* shell:false`) gets no `zlet` subcommand.
The proto again overrides the file. A service option on a `<service>.<rpc>`
line, `shell` on a bare service line, or an rpc pattern that matches no rpc
stops the build.

## Composition

A zephlet may call another zephlet as part of its own behaviour: a climate
controller reads a temperature sensor and drives a fan. It declares the
provider *types* it needs; the application decides which provider
*instances* each controller uses: a zephlet depends only on what its proto
declares, bound from outside.

### Declare

One repeatable service option per dependency slot:

```proto
import "zephlet_options.proto";

service ClimateApi {
  option (zephlet.requires) = { type: "temperature" };
  option (zephlet.requires) = { type: "fan" name: "intake" };
  option (zephlet.requires) = { type: "fan" name: "exhaust" };

  rpc regulate(Empty) returns (Climate.Decision);
}
```

or, keeping the proto free of the import, in `zlet_climate.zephlet.options`:

```
ClimateApi requires:temperature requires:intake=fan requires:exhaust=fan
```

- `type` is the provider's zephlet type, as passed to `ZEPHLET_NEW` (the
  snake-cased owning message, `temperature` for `message Temperature`) —
  never its file prefix.
- `name` is the slot, and defaults to `type`. Requiring one type twice
  needs a name per slot.
- Both places add to one list. An `import` alone declares nothing.

Codegen rejects a type or name that is not a C identifier, a name that is a
C keyword, two slots of one name, and a zephlet requiring its own type.
The build then stops, before anything compiles, when a required type is not
in the build or the types form a cycle of any length (`a -> b -> c -> a`),
printing it. The check is a build step that depends on every zephlet's proto
and options file, so editing a proto re-runs it without reconfiguring.

### Generated

```c
struct climate_dependencies {
	const struct zephlet *temperature;
	const struct zephlet *intake;
	const struct zephlet *exhaust;
};
```

plus one slot row per dependency (name, provider type, its `<type>_api`,
field offset) in `climate_api`, which `zephlet_init_one()` checks.

### Bind

```c
static const struct climate_dependencies room_a_deps = {
	.temperature = ZEPHLET_DEP(temperature, room_a_sensor),
	.intake = ZEPHLET_DEP(fan, room_a_fan),
	.exhaust = ZEPHLET_DEP(fan, shared_exhaust),
};
ZEPHLET_NEW_WITH_DEPS(climate, room_a, &room_a_cfg, &room_a_data, &room_a_deps,
		      climate_init_fn);
```

`ZEPHLET_DEP(type, inst)` is `&inst`, checked at compile time: an instance
of another type fails to build. It needs `inst` defined with `ZEPHLET_NEW*`
or declared with `ZEPHLET_DECLARE(type, inst)` in the same file. A plain
`&inst` also compiles, whatever its type; the checks below still catch it.

`ZEPHLET_NEW_PRIO_WITH_DEPS` adds a priority. Instances of a type that
requires nothing keep `ZEPHLET_NEW`; each macro fails to build on the other
kind of type, and `_WITH_DEPS` rejects a record of another type.
`ZEPHLET_INST_DEFINE_WITH_DEPS(type, id, &deps, init)` and its `_PRIO_`
form allocate the config and data, like `ZEPHLET_INST_DEFINE`. Several
consumers may share a provider.

| `struct zephlet` field | Holds |
|---|---|
| `config` | Writable behavioural configuration |
| `data` | Mutable runtime state |
| `dependencies` | Immutable references to provider instances (NULL without `requires`) |

Binding transfers no ownership: a provider keeps its own config, data and
lifecycle, and serves every caller alike.

### Call

```c
int climate_regulate_impl(const struct zephlet *z, struct climate_decision *resp)
{
	const struct climate_dependencies *deps = z->dependencies;
	struct temperature_reading t;
	int err = temperature_read(deps->temperature, &t, K_MSEC(100));
	...
}
```

Only through the provider's public wrappers — never its `_impl` functions
or its `data`. Every wrapper returns `-EINVAL` for an instance of another
type (or NULL), so a mistaken pointer never reaches another type's method
table.

**Execution context.** A call is the ordinary synchronous RPC: it runs the
provider in the caller's thread, inside the consumer's handler. Composition
adds no thread, queue or asynchrony.

- **Locking:** zbus runs a command's listener while it holds the command
  channel's lock, so the consumer's channel stays locked for the whole
  provider call. Locks are always taken consumer before provider, and the
  graph is acyclic, so this cannot deadlock. But a slow provider blocks every
  caller of the consumer, and the caller's and the provider's timeouts add
  up: offload in the consumer if a provider is slow.
- **No ISR:** a consumer with dependencies cannot be called from an ISR. Its
  handler calls providers with a real timeout, and zbus asserts on anything
  but `K_NO_WAIT` from an ISR.

**Testing with a fake provider.** Handlers belong to the type, so a test
builds the provider type from its proto alone (its generated interface,
without its real `.c`), defines the handlers it needs, and binds the
consumer under test to an instance of it:

```c
/* test/main.c: a fan instance whose set_speed the test inspects. */
ZEPHLET_NEW(fan, fake_fan, &fake_fan_cfg, &fake_fan_data, NULL);
int fan_set_speed_impl(const struct zephlet *z, const struct fan_speed *req)
{
	last_speed = req->percent; /* strong override of the weak default */
	return 0;
}
static const struct climate_dependencies room_deps = {
	.temperature = ZEPHLET_DEP(temperature, fake_sensor),
	.intake = ZEPHLET_DEP(fan, fake_fan),
	.exhaust = ZEPHLET_DEP(fan, fake_fan),
};
ZEPHLET_NEW_WITH_DEPS(climate, room, &room_cfg, &room_data, &room_deps, climate_init_fn);
```

Handlers the test does not define keep their weak defaults. The fake is
an ordinary instance of the provider type, so the consumer cannot tell it
apart, and the link check still checks its binding.

### Initialization

With `CONFIG_ZEPHLET_DEPS_LINK_CHECK` (default `y`) the build fails after
the link on an unbound slot or a slot holding an instance of another type:

```
error: zephlet: 'room_a': dependency 'intake' (a fan) is not bound
```

It reads the instances and their records from the ELF (pyelftools) and adds
nothing to the image. A zeroed record in `.bss` reads as all slots unbound.

Every `ZEPHLET_NEW` instance gets its own Zephyr `SYS_INIT` entry at `APPLICATION` level, and the kernel's ordinary init
sequence runs them:

- **Priority:** `CONFIG_ZEPHLET_INIT_PRIORITY` (default 10) + the type's
  *init rank*. A type that requires nothing has rank 0; otherwise its rank is
  one above the highest rank among its providers. The dependency-graph build
  step computes the ranks and writes them to `zephlet_init_prio.h`, rewriting
  it only when a rank changes. A provider type therefore always runs before
  its consumer types.
- **Sub-priority:** `init_priority`, given with `ZEPHLET_NEW_PRIO*`, orders the
  instances of one rank. It is a decimal literal 0–9, because the linker sorts
  it as text; the build rejects anything else. Instances with equal priority
  and sub-priority run in an unspecified order.

| Types | Rank | Priority | Instances (sub-priority) |
|---|---|---|---|
| `temperature`, `fan` | 0 | 10 | `sensor_a`, `fan_a` (0), then `fan_b` (9) |
| `climate` (requires both) | 1 | 11 | `room_a`, `room_b` (0) |

So `fan_b` runs before `room_a` even though its sub-priority is higher. Every
instance of a rank runs before any instance of the next. Zephlets interleave
with the app's own `SYS_INIT`s by priority, and `west build -t initlevels`
lists them.

Each entry calls `zephlet_init_one()`. It skips the instance, so its
`init_fn` never runs and an error is logged, when:

- a slot is unbound, or holds something other than an instance of its type;
- a provider's `init_fn` failed, or the provider was itself skipped;
- a provider has not run yet (see below).

Every instance, with or without dependencies, is behind the same gate in
`zephlet_dispatch()`, for calls from C or any frontend:

| State | Calls answer | `zephlet_init_result(z)` |
|---|---|---|
| Its entry has not run yet | `-EAGAIN` | `-EAGAIN` |
| Its `init_fn` is running | served, so it may call its own RPCs | `-EAGAIN` |
| `init_fn` returned 0 (or there is none) | served | `0` |
| `init_fn` failed | `-ENODEV` | the `init_fn`'s error |
| Skipped (bad binding, provider down) | `-ENODEV` | `-ENODEV` |

The base priority sits above zbus's channel initialization
(`CONFIG_ZBUS_CHANNELS_SYS_INIT_PRIORITY`; the build checks it), so a
consumer's `init_fn` may call its providers' RPCs.

**Cycles are a build error.** Ranks need an acyclic type graph, and a slot
only accepts an instance of its own type, so instances of generated types
cannot form a cycle. A hand-written record (below) that still forms one, or
that ranks a consumer before its provider, finds that provider not
initialized yet: it is skipped with `-ENODEV`, and so is anything that needs
it. No cyclic `init_fn` runs.

A hand-written `const struct zephlet` record uses
`ZEPHLET_INIT_DEFINE(name, prio, sub)` before its definition, and sets
`.runtime = ZEPHLET_RUNTIME(name)`:

```c
ZEPHLET_INIT_DEFINE(my_rec, 20, 0);
const STRUCT_SECTION_ITERABLE(zephlet, my_rec) = {
	.name = "my_rec", .api = &my_api, .runtime = ZEPHLET_RUNTIME(my_rec),
};
```

- `prio` is an absolute `APPLICATION` priority literal, which must be above
  zbus's;
- `sub` is a bare decimal literal 0–9;
- the macro relies on Zephyr's internal `Z_INIT_ENTRY_SECTION` for the
  sub-priority, so revisit it on a Zephyr upgrade.

A record without a runtime record is never initialized: every call answers
`-EAGAIN`, and it cannot be a provider.

Each instance points at an 8-byte runtime record in `.bss`, which costs no
flash; there is no instance limit, and the gate is O(1), with no division.

**Migrating from v0.3:**

- Zephlets initialize at `APPLICATION` priority `CONFIG_ZEPHLET_INIT_PRIORITY`
  (10) + rank instead of 0, so an app `SYS_INIT` at `APPLICATION` priority 1–9 now
  runs **before** them.
- `init_priority` used to accept any `int`; it is now a bare literal 0–9.
- Instances with equal priority used to initialize in name order; they now
  run in an unspecified order.
- Calls before an instance's init answer `-EAGAIN`, and an instance whose
  `init_fn` failed refuses calls with `-ENODEV`. v0.3 served every call.

### Versus adapters and coordinators

| Mechanism | Use when |
|---|---|
| Composition | A zephlet needs a reply from another service to do its own job (request/reply inside one RPC) |
| [Adapters](#adapters) | An event from one zephlet should trigger a command on another; neither knows of the other |
| [Coordinators](#coordinators) | An application-level, multi-step flow waits for events and timeouts across zephlets |

Not provided: runtime rebinding or lookup by name, optional dependencies,
and interchangeable implementations of one service contract — a slot
accepts instances of exactly its declared type.

## Adapters

Not a framework concept in v0.3. An "adapter" is plain user code — usually a single `.c` file composed from the `ZEPHLET_EVENTS_LISTENER` primitive:

```c
static void on_tick(const struct zephlet *z,
                    const struct tick_events *ev) {
    ARG_UNUSED(z);
    ARG_UNUSED(ev);
    (void)ui_blink(&ui_instance, K_MSEC(100));
}
ZEPHLET_EVENTS_LISTENER(tick_instance, tick, on_tick);
```

Guard the translation unit at CMake level when it references channels from optional zephlets:

```cmake
if(CONFIG_ZEPHLET_TICK AND CONFIG_ZEPHLET_UI)
    target_sources(app PRIVATE adapters.c)
endif()
```

## Coordinators

Optional framework for multi-step flows that span several zephlets. A coordinator is a singleton state object whose flow is expressed as a chain of step callbacks dispatched on a shared workqueue (`zephlet_coord_workq`), with optional bounded zbus-event awaits. Enable with `CONFIG_ZEPHLETS_COORD=y`.

Reach for it when an application flow needs state across multiple events (provisioning, OTA, multi-stage tamper response). Stateless event routing stays as plain `ZEPHLET_EVENTS_LISTENER` adapters.

```c
static struct provisioning_ctx ctx;
ZEPHLET_COORD_ASYNC_DEFINE(provisioning, ctx, s_handshake);

static void s_handshake(struct zephlet_coord *c)
{
    struct provisioning_ctx *st = c->ctx;
    (void)zlet_radio_connect(&radio_instance, &st->cred, K_SECONDS(2));
    zephlet_coord_await(c, &chan_zlet_radio_events,
                        &st->reply, match_connected,
                        s_complete, K_SECONDS(5));
}

static void s_complete(struct zephlet_coord *c)
{
    struct provisioning_ctx *st = c->ctx;
    uint8_t result = zephlet_coord_await_result(c);

    if (result & ZEPHLET_COORD_AWAIT_FLAG_ARM_FAILED) {
        /* could not arm the await */
    } else if (result & ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT) {
        /* radio never reported connected; st->reply is untouched */
    } else if (result & ZEPHLET_COORD_AWAIT_FLAG_MATCHED) {
        /* st->reply holds the matching event */
    }
    zephlet_coord_done(c);
}

/* trigger source — typically a zbus listener on a flow-local channel */
int err = zephlet_coord_kick(provisioning);
/* err == -EBUSY: already running; the kick re-runs the flow once it
 * completes. Author's policy decides whether that is wanted. */
```

Public surface (see [`zephlet_coord.h`](zephlet_coord.h)):

| Operation | Role |
|---|---|
| `ZEPHLET_COORD_DEFINE` / `_ASYNC_DEFINE` | Allocate a sync or async coordinator at file scope. |
| `zephlet_coord_kick(c)` | Start the flow; returns `-EBUSY` if already running. |
| `zephlet_coord_next(c, fn)` | Queue the next step within an in-flight flow. |
| `zephlet_coord_await(c, chan, dst, match, next, timeout)` | Suspend until a matching publish arrives or the timeout fires. The framework-generated listener handles the memcpy; only the winning event is copied. If arming fails, `next` still runs and reads `_ZBUS_FAILED` or `_TIMER_FAILED`. Rule: call await, return from the step, and handle every outcome in `next`. |
| `zephlet_coord_resolve(c)` | End an armed await as a success, without a match or copy; no-op if it already ended. |
| `zephlet_coord_cancel(c)` | End an armed await as cancelled; `next` still runs. No-op if it already ended. |
| `zephlet_coord_await_result(c)` | From a continuation: how the await ended, as one `ZEPHLET_COORD_AWAIT_FLAG_*` bit (`_MATCHED`, `_TIMED_OUT`, `_RESOLVED`, `_CANCELLED`, `_ZBUS_FAILED`, `_TIMER_FAILED`; `_PENDING` while armed). The bits are mutually exclusive; `_ARM_FAILED` masks the two arm failures. Read it at the start of the continuation, before another transition: kick and plain `next` report `0`, and a new await replaces it with `_PENDING`. Domain reasons belong in `c->ctx`. |
| `zephlet_coord_done(c)` | Mark the flow idle. |

## Shell frontend

`CONFIG_ZEPHLETS_SHELL=y` puts every instance's RPCs under one `zlet` root
command, except those an rpc's [`shell`](#per-rpc-options) option hides. No app code: `ZEPHLET_NEW(...)` already expands the per-type hook that
registers them. Needs the library in your manifest — see [Dependencies](#dependencies).

An RPC takes its request as a protobuf **text-format** message:

```
uart:~$ zlet <TAB>                                    # one entry per instance
uart:~$ zlet_init                                     # each instance's init result and bindings
uart:~$ zlet tick_fast config duration_ms: 100, period_ms: 10
duration_ms: 100
period_ms: 10

zlet tick_fast config duration_ms: 100, period_ms: 10
```

A response prints **twice**: an indented block to read, then the compact form
prefixed with the command that writes it back. Copy that last line, change a
value, paste it.

Both forms are valid text format, but only one of them is useful for each job.
The block indents each submessage and puts one field per line, so where an
element starts and finishes stays visible however deep the message nests — yet
it cannot be pasted into the shell, which submits a command on every newline.
The compact form is the shortest valid standalone text format and pastes back
intact, but wraps mid-token once it outgrows the terminal. Printing both is
what makes a nested response readable *and* editable.

The command prefix is resolved by codegen, by matching a response's type against
every RPC's request type — so `get_config` names `config`, and a `get_shapes` /
`set_shapes` pair is matched on type rather than on any naming convention. A
response nothing accepts as a request prints with no prefix, which is the case
for the `Lifecycle.Status` returned by `start`, `stop` and `get_status`.

`CONFIG_ZEPHLETS_SHELL_PRINT_BOTH` is the default; `_PRETTY` and `_COMPACT`
select one form on its own. With `CONFIG_ZEPHLETS_SHELL_PRINT_RUNTIME=y` (also
the default) the `zlet_fmt` command switches between them live:

```
uart:~$ zlet_fmt                                      # report the current form
both
uart:~$ zlet_fmt compact                              # just the pasteable line
uart:~$ zlet tick_fast get_config
zlet tick_fast config duration_ms: 100, period_ms: 10
```

`zlet_fmt` is a root command rather than a `zlet` subcommand, because `zlet`'s
subcommands are the instance names and an instance could otherwise shadow it.

A nested message is where the block earns its place. The library keeps a
narrow submessage inline and breaks a wide or submessage-bearing one apart, so
structure shows without the output turning into one field per line:

```
uart:~$ zlet typelab_bench get_shapes
plain_scalar: 1
rep_scalar: [3, 4]
sing_msg {a: 5, b: -6}
rep_msg: [
  {a: 9, b: 0},
  {a: 10, b: 0}
]
deep {
  d2 {
    d3 {v: 11}
  }
}

zlet typelab_bench set_shapes plain_scalar: 1, rep_scalar: [3, 4], sing_msg {a: 5, b: -6}, rep_msg: [{a: 9, b: 0}, {a: 10, b: 0}], deep {d2 {d3 {v: 11}}}
```

`sing_msg` and `d3` stay inline because they are narrow leaves; `rep_msg` and
`deep` break because a repeated submessage and a submessage-bearing type always
do, however narrow the value. `CONFIG_NANOPB_TEXTFORMAT_PRINT_FIT_WIDTH` (64)
sets the width, and the frontend pins `PRINT_FIT` on wherever it prints a block.

Fields are named, so order is free and any subset works. Anything omitted reads
back as zero — the parser clears the message before writing into it.

Every field shape works, including ones a positional grammar cannot express:

```
zlet ui_a config opt_scalar: 0                        # optional: present, zero
zlet ui_a config tags: [1, 2, 3]                      # repeated
zlet ui_a config origin {x: 7, y: -3}                 # submessage; colon optional
zlet ui_a config path: [{x: 1}, {x: 2}]               # repeated submessage
zlet ui_a config deep {d2 {d3 {v: 42}}}               # nesting
zlet ui_a config name: "hi there"                     # a value with a space
zlet ui_a config blob: "\x0F\x0Ahello\x1E"             # mixed binary and text
```

### Absent is not zero

The one thing that surprises people. An `optional` field, an unset submessage
and an empty `repeated` field print **nothing** — they are absent, which text
format expresses by omission. An implicit-presence scalar always prints, zero
included. So a freshly zeroed message shows only its implicit-presence fields,
and `opt_scalar: 0` appears only once something has set it. That distinction is
the reason to declare a field `optional` at all, and there is no flag to
override it: emitting an absent field would produce text that re-parses into a
different struct.

### Input and output spellings differ, deliberately

Round-trip holds at the *value* level, not the text level:

| | Accepted | Printed |
|---|---|---|
| Repeated | `tags: [1, 2]` or `tags: 1 tags: 2` | `tags: 1` / `tags: 2`, one per element |
| Bytes | `"\x0F\x0A"`, `\NNN`, `\uXXXX` | three-digit octal: `"\017\012"` |
| Submessage | `origin {x: 1}` | `origin {x: 1}` — braces inline, since output is compact |

The printer avoids `\xHH` because a hex escape runs on: `\x0` followed by `a`
reads back as the single byte `0x0A`, while three octal digits cannot.

### Errors

Failures carry a byte offset and, when a field was in scope, its name:

```
uart:~$ zlet tick_fast config duration_ms: -1
config: at offset 14: value out of range in field 'duration_ms'
uart:~$ zlet tick_fast config nope: 1
config: at offset 0: no such field
```

A rejected message never reaches the zephlet. The parser writes decoded bytes
straight into the destination with no scratch buffer, so a failed parse can
leave partial bytes behind; the generated handler dispatches only on success.

### What to watch for

- **Don't quote the whole argument.** It arrives as one `SHELL_OPT_ARG_RAW`
  argument, so quotes are no longer stripped: `config '{a: 1}'` feeds the `'`
  to the parser. Quote *values*, not the argument.
- **`CONFIG_CBPRINTF_FULL_INTEGRAL=y`** if any exposed message has a field
  wider than 32 bits, or such a field reports `PB_TF_ERR_UNSUPPORTED`.
- **Two buffers default smaller than you would guess.** Naming every field
  makes text format verbose: `CONFIG_SHELL_CMD_BUFF_SIZE` (256) caps the whole
  line, and on the serial backend
  `CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE` (64) caps how much can
  arrive before the shell drains it — which truncates a *pasted* line
  mid-parse, silently, while typing the same line by hand works.
- **`CONFIG_SHELL_WILDCARD` needs no attention.** An RPC's raw argument is
  never tokenised, so `*` and `?` inside a value survive intact. See
  [`docs/adr/0001-zephlet-frontends.md`](docs/adr/0001-zephlet-frontends.md).
- Enum values are numeric in both directions, not by name.

### Cost

Measured on `mps2/an385` at `-Os`: ~5.1 KB for the library, plus
`12 x (fields + 1) + 12` bytes and the field-name strings per message
descriptor — 1068 B across 23 messages in the example app. No static RAM;
~400 B of stack per call. Descriptors are emitted for every message in a proto
that has at least one field, since one reachable only as a submessage still
needs one.

## Lua frontend

`CONFIG_ZEPHLETS_LUA=y` exposes the instances of each type whose options
file sets `lua:true` (see [Frontend options](#frontend-options)) to a Lua
script as the `zephlets` module, through lua_zephyr's `zephyr.async` scheduler. No app wiring beyond
`zephlet_lua_coordinate()` in `main()`:

```lua
local z = require("zephlets")
z.tick_fast:add_policy(function(ev) z.ui_main:blink(100) end)
```

Commands take a required `timeout_ms` and return `resp` or
`nil, "<ERRNO>", rc`. Policies are fixed once the script's main chunk
returns, and all of them run in one persistent coroutine. The script's own
`zephyr.async` tasks can also wait for events with `inst:await()` and
`z.await_any()`. No Lua runs in listener context. Full guide (event transport, limits, LuaLS):
[`frontends/lua/README.md`](frontends/lua/README.md).

## West commands

| Command | Purpose |
|---|---|
| `west zephlet new [-o <dir>] [-n -d -a] [--prefix STR] [--no-module]` | Copier scaffold. Destination = `$PWD` unless `-o`. `--prefix` overrides the default `zlet_` (`""` drops it); `--no-module` skips the tests folder and `zephyr/module.yml`. |
| `west zephlet new-adapter` | Prints the v0.3 recipe. No codegen. |
| `west zephlet gen <zephlet_dir>` | Regenerate `<prefix>_interface.{h,c}` from its proto. |

## Releases

Two version numbers appear in this repo and they mean different things.

**`v0.3` is the architecture generation** — the contract described in
[CLAUDE.md](CLAUDE.md): two channels per instance, `ZEPHLET_NEW`, weak handler
overrides, no `ZephletResult`. That is what "the v0.3 contract" refers to
throughout the docs, and it has not changed since v0.3.0.

**The third component is the release counter** within that generation. Pin a
tag rather than `main`, so a `west update` cannot move the framework under you.

| Tag | |
|---|---|
| **`v0.3.4`** | The `zlet` shell frontend — see [Shell frontend](#shell-frontend). Adds a *conditional* dependency: `CONFIG_ZEPHLETS_SHELL=y` needs `zephyr-nanopb-textformat` in your manifest. **Breaking** for anyone on the earlier frontend: RPC arguments were positional and are now named text format. |
| `v0.3.3` | CoAP frontend maturation — optional DTLS-PSK, service start on L4 connectivity rather than at boot, the Observe registration gate, a `CONFIG_ZEPHLETS_COAP=n` footprint gate. Also codegen build-ordering fixes and a coordinator lost-wakeup fix. |
| `v0.3.2` | Configurable codegen prefix, optional module scaffold. |

Each tag carries an annotated message with the detail; `git show v0.3.4` is the
authority over this table.

## Dependencies

- Zephyr RTOS (with `zbus`, `nanopb` modules).
- Python packages: `proto-schema-parser`, `jinja2`, `copier`.
- [`zephyr-nanopb-textformat`](https://github.com/rodrigopex/zephyr-nanopb-textformat)
  — **only with `CONFIG_ZEPHLETS_SHELL=y` or `CONFIG_ZEPHLETS_LUA=y`**, each
  of which `select`s it. The shell frontend takes each RPC's request as a
  protobuf text-format message and hands parsing and printing to this
  library, so every field shape works (`optional`, `repeated`, submessage,
  nested) without codegen walking fields. The Lua frontend uses only its
  message descriptors, for the conversion between Lua tables and nanopb
  structs.

  It must be in *your* west manifest: if it is absent the `select` has no
  symbol to resolve and the frontend has no `nanopb_textformat.h` to
  include. Nothing else in the infra needs it, so a project with both the
  shell and the Lua frontend off can leave it out entirely.

  `CONFIG_ZEPHLETS_SHELL` selects both halves of it —
  `NANOPB_TEXTFORMAT_PARSE` and `NANOPB_TEXTFORMAT_PRINT` — since an RPC's
  request is parsed and its response printed. Since v0.5.0 either can be
  configured out, and the library guards its declarations on them, so
  selecting both keeps a deliberate `=n` an unmet Kconfig dependency
  rather than a compile error inside a generated handler.

  **Pin a tag.** The API is pre-1.0 and has already moved twice. v0.3.0
  dropped the trailing `flags` argument from `pb_tf_parse()` /
  `pb_tf_print()` in favour of Kconfig options, and split `PB_TF_NOINIT`
  out as `pb_tf_merge()`. v0.7.0 then replaced those Kconfig options with
  named wrappers — `pb_tf_print_compact()`, `pb_tf_print_buf_multiline()`
  and so on — dropped the `pb_tf_*_t` typedefs, and made every function
  return `int` Zephyr-style: 0, or a negated `enum pb_tf_err`. v0.8.0
  changed what the printer *emits* without touching the API: a repeated
  field folds into `tags: [1, 2]`, and the multi-line styles keep a
  narrow submessage on one line instead of breaking every one apart.
  Tested here against **v0.8.1**.

  Its `docs/shell-integration.md` covers the application-side settings a
  console needs — chiefly `CONFIG_CBPRINTF_FULL_INTEGRAL=y` for fields
  wider than 32 bits, and larger `CONFIG_SHELL_CMD_BUFF_SIZE` /
  `CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE` for a wide message,
  since naming every field makes text format verbose.

- [`lua_zephyr`](https://github.com/zano-systems/lua_zephyr): **only with
  `CONFIG_ZEPHLETS_LUA=y`**, which `depends on` its `LUA_ASYNC`. Pin it by
  SHA with `submodules: true`. It was tested against `58c47e5`
  (`feat/async-coroutine-scheduler`). The Lua frontend also `select`s
  `NANOPB_TEXTFORMAT`, for its descriptors only, so the textformat library
  above must be in the manifest too.

## Example app

Reference implementation: [ports_adapter_zbus](https://github.com/rodrigopex/ports_adapter_zbus).

## License

Apache-2.0
