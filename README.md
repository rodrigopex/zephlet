# Zephlet Infrastructure Module (v0.3)

[![CI](https://github.com/rodrigopex/zephlet/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/rodrigopex/zephlet/actions/workflows/ci.yml)

Reusable framework for building domain-isolated components on Zephyr RTOS, communicating exclusively over zbus. Each **zephlet** is a non-singleton, instance-per-`ZEPHLET_DEFINE` module with two channels: a synchronous pointer-based RPC channel and a value-typed events channel.

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

The scaffold has no opinion on where zephlets must live — the Copier template drops a complete module under the destination directory. Users wire it into their app by adding its path to `EXTRA_ZEPHYR_MODULES` and enabling `CONFIG_ZEPHLET_<NAME>=y`.

### 4. App wiring

```cmake
set(EXTRA_ZEPHYR_MODULES "${CMAKE_SOURCE_DIR}/path/to/my_zephlet" ...)
```

Instantiate and use:

```c
#include "zlet_my_zephlet.h"

static struct my_zephlet_data my_data;
static const struct my_zephlet_config my_cfg = { /* ... */ };
ZEPHLET_DEFINE(my_zephlet, my_instance, &my_cfg, &my_data, my_zephlet_init_fn);

/* ... in main or elsewhere ... */
struct lifecycle_status st;
my_zephlet_start(&my_instance, &st, K_MSEC(500));
```

## Architecture at a glance

- **`rpc` channel** (pointer, listener-only): synchronous RPC via zbus sync-listener. Wrapper returns the handler's rc directly — no correlation IDs, no semaphores, no result struct.
- **`events` channel** (value-typed): async fan-out. Publishers call `<type>_emit(z, &ev, timeout)`; consumers observe with `ZEPHLET_EVENTS_LISTENER(instance, type, callback)` (wraps `ZBUS_ASYNC_LISTENER_DEFINE`).
- **Non-singleton**: multiple instances per type coexist; each has its own channel pair and data.
- **Weak handler overrides**: generator emits `__weak int <type>_on_<rpc>(...)` returning `-ENOSYS`; user provides strong overrides in `<prefix>.c`.

## Adapters

Not a framework concept in v0.3. An "adapter" is plain user code — usually a single `.c` file composed from the `ZEPHLET_EVENTS_LISTENER` primitive:

```c
static void on_tick(const struct tick_events *ev) {
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

## West commands

| Command | Purpose |
|---|---|
| `west zephlet new [-o <dir>] [-n -d -a]` | Copier scaffold. Destination = `$PWD` unless `-o`. |
| `west zephlet new-adapter` | Prints the v0.3 recipe. No codegen. |
| `west zephlet gen <zephlet_dir>` | Regenerate `<prefix>_interface.{h,c}` from its proto. |

## Dependencies

- Zephyr RTOS (with `zbus`, `nanopb` modules).
- Python packages: `proto-schema-parser`, `jinja2`, `copier`.

## Example app

Reference implementation: [ports_adapters_zbus](https://github.com/rodrigopex/ports_adapters_zbus).

## License

Apache-2.0
