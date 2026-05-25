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
      revision: main
      path: modules/lib/zephlet
      west-commands: west/west-commands.yml
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
- **Coordinators** (optional, `CONFIG_ZEPHLETS_COORD=y`): multi-step flows with workqueue dispatch + bounded zbus-event awaits. Sits above the per-zephlet command/events surface — see [Coordinators](#coordinators) below.

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

/* trigger source — typically a zbus listener on a flow-local channel */
if(!zephlet_coord_is_running(provisioning)) {
    int err = zephlet_coord_kick(provisioning);
    /* err == -EBUSY: author's policy (drop / queue / reject) */
}
```

Public surface (see [`zephlet_coord.h`](zephlet_coord.h)):

| Operation | Role |
|---|---|
| `ZEPHLET_COORD_DEFINE` / `_ASYNC_DEFINE` | Allocate a sync or async coordinator at file scope. |
| `zephlet_coord_kick(c)` | Start the flow; returns `-EBUSY` if already running. |
| `zephlet_coord_next(c, fn)` | Queue the next step within an in-flight flow. |
| `zephlet_coord_await(c, chan, dst, match, next, timeout)` | Suspend until a matching publish arrives or the timeout fires. The framework-generated listener handles the memcpy. |
| `zephlet_coord_resolve(c)` | Finalise an await; idempotent against the timeout path. |
| `zephlet_coord_done(c)` | Mark the flow idle. |

## West commands

| Command | Purpose |
|---|---|
| `west zephlet new [-o <dir>] [-n -d -a] [--prefix STR] [--no-module]` | Copier scaffold. Destination = `$PWD` unless `-o`. `--prefix` overrides the default `zlet_` (`""` drops it); `--no-module` skips the tests folder and `zephyr/module.yml`. |
| `west zephlet new-adapter` | Prints the v0.3 recipe. No codegen. |
| `west zephlet gen <zephlet_dir>` | Regenerate `<prefix>_interface.{h,c}` from its proto. |

## Dependencies

- Zephyr RTOS (with `zbus`, `nanopb` modules).
- Python packages: `proto-schema-parser`, `jinja2`, `copier`.

## Example app

Reference implementation: [ports_adapter_zbus](https://github.com/rodrigopex/ports_adapter_zbus).

## License

Apache-2.0
