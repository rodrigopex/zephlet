# Zephlet Infrastructure v0.3

Ports+Adapters on Zephyr/zbus. Zephlets=domain logic (no direct deps). Adapters=plain user C composed from a framework-level observer primitive. Instances are non-singleton; multiple per type.

## Model

Each zephlet instance owns **two** zbus channels:

| Channel | Payload | Observers | Purpose |
|---|---|---|---|
| `command` | `struct zephlet_call *` (pointer, caller-stack) | exactly one `lis_<type>` listener | Synchronous command via zbus sync-listener |
| `events` | `struct <type>_events` (value-typed) | any mix | Async fan-out, zbus copies per consumer |

**Synchronous command:** `zbus_chan_pub()` on the `command` channel runs the dispatcher in the caller's thread (sync listener). By the time pub returns, `call->return_code` and `*call->resp` are populated. Wrapper returns `call.return_code`. No reply republish.

**Async events:** `zbus_chan_pub()` on the `events` channel copies the value into every consumer's queue. Consumers read by value; no pointer lifetime concerns.

## Shared contract (`zephlet.h`)

```c
struct zephlet_call { uint16_t method_id; int32_t return_code;
                      const pb_msgdesc_t *req_desc; const void *req;
                      const pb_msgdesc_t *resp_desc; void *resp; };

struct zephlet_method { const pb_msgdesc_t *req_desc, *resp_desc;
                        int (*handler)(const struct zephlet *, struct zephlet_call *); };

struct zephlet_api { const struct zephlet_method *methods; size_t num_methods; };

struct zephlet { const char *name; const struct zephlet_api *api;
                 struct { const struct zbus_channel *command, *events; } channel;
                 void *config; void *data;
                 int (*init_fn)(const struct zephlet *); int init_priority; };

int zephlet_dispatch(const struct zephlet *z, struct zephlet_call *call);
const struct zephlet *zephlet_get_by_name(const char *name);
```

**`ZEPHLET_NEW(type, name, cfg, data, init)`** instantiates one zephlet: creates `chan_<name>_command` (pointer, observer = `lis_<type>`), `chan_<name>_events` (value-typed `struct <type>_events`), and a section-iterable `struct zephlet` record. `cfg` is a writable pointer (the `config` command mutates it in place).

**SYS_INIT walker** (APPLICATION / prio 0) sorts the section by `init_priority` and calls each instance's `init_fn`.

## Per-zephlet files

- **User-owned (source tree):** `<prefix>.proto`, `<prefix>.h` (declares `struct <type>_data` + `int <type>_init_fn(const struct zephlet *)`), `<prefix>.c` (strong handler overrides), `CMakeLists.txt` (single `zephyr_zephlet_generate(TYPE ... PREFIX ... SOURCES ...)` call), `Kconfig`, `zephyr/module.yml`.
- **Generated (build dir):** `<prefix>_interface.{h,c}`, `<prefix>.pb.{h,c}`.
- **Framework-standard fields** come first in `struct <type>_data`: `bool is_running, is_ready;` — custom fields follow, separated by a banner comment. The runtime config is `*z->config` (writable; user owns the per-instance struct).

User code never needs a `src/zephlets/` convention — zephlets can live anywhere in the app tree. The app's top-level `CMakeLists.txt` lists each directory in `EXTRA_ZEPHYR_MODULES`.

## Handlers (weak + strong)

Generator emits `__weak int <type>_<cmd>_impl(...)` returning `-ENOSYS` for every rpc method declared in the proto. User overrides in `<prefix>.c`. Signatures follow four shapes driven by `(req_is_empty, resp_is_empty)`:

```
(Empty, Empty) → int <type>_<cmd>_impl(const struct zephlet *z);
(Empty, Resp)  → int <type>_<cmd>_impl(const struct zephlet *z, struct <R> *resp);
(Req, Empty)   → int <type>_<cmd>_impl(const struct zephlet *z, const struct <Q> *req);
(Req, Resp)    → int <type>_<cmd>_impl(const struct zephlet *z, const struct <Q> *req, struct <R> *resp);
```

`resp == NULL` = caller discards; handler guards writes with `if (resp)`. `req` is never nullable. Dispatcher handles `method_id` bounds check + null-handler fallback (both → `-ENOSYS`).

## Wrappers + events helper + readiness

Generated per-type in `<prefix>_interface.c`:

- Four wrapper shapes, each: build stack-local `struct zephlet_call`, `zbus_chan_pub(z->channel.command, &p, timeout)`, return `call.return_code`.
- `int <type>_emit(const struct zephlet *z, const struct <type>_events *ev, k_timeout_t timeout)` — thin `zbus_chan_pub` on the events channel.
- `bool <type>_is_ready(const struct zephlet *z)` — sugar over `get_status`.

## Events-listener primitive (adapters)

Framework ships one macro; no codegen.

```c
static void on_tick(const struct zephlet *z,
                    const struct tick_events *ev) { /* ... */ }
ZEPHLET_EVENTS_LISTENER(tick_instance, tick, on_tick);
```

Wraps `ZBUS_ASYNC_LISTENER_DEFINE` — callback runs from the system workqueue (never ISR), so it can freely call other zephlets' commands with real timeouts. Requires `CONFIG_ZBUS_ASYNC_LISTENER=y` (selected by `CONFIG_ZEPHLETS`).

If the adapter references a channel whose owning zephlet is optional, guard the translation unit at CMake level: `if(CONFIG_ZEPHLET_X AND CONFIG_ZEPHLET_Y) zephyr_library_sources(adapter.c) endif()`.

## Protobuf

Source `.proto` per zephlet:

```proto
message Tick {
  message Config { uint32 period_ms = 1; uint32 max_period_ms = 2; }
  message Events { int32 timestamp = 1; }
}

service TickApi {
  rpc start       (Empty)       returns (Lifecycle.Status);
  rpc stop        (Empty)       returns (Lifecycle.Status);
  rpc get_status  (Empty)       returns (Lifecycle.Status);
  rpc config      (Tick.Config) returns (Tick.Config);
  rpc get_config  (Empty)       returns (Tick.Config);
  /* custom RPCs */
}
```

- Service name must differ from the outer message name (protoc rejects collision). Convention: `<Type>Api`.
- `method_id` allocated in declaration order, starting at 1; slot 0 reserved.
- `option (nanopb_fileopt).long_names = false;` — required; generator assumes `Parent.Child → parent_child` snake-cased names.
- No oneofs. No `ZephletResult`, `Invoke`, `Report`, `has_result`, `extends_base` — all gone in v0.3.
- Streaming methods rejected by the generator.
- Shared `zephlet.proto` supplies `Empty` and `Lifecycle.Status`.

## West commands

- `west zephlet new [-n -d -a] [-o <dir>]` — Copier scaffold. Destination = `$PWD` unless `-o` given.
- `west zephlet new-adapter` — prints the recipe (framework has no adapter codegen).
- `west zephlet gen <zephlet_dir>` — regenerate `<prefix>_interface.{h,c}` from an existing proto.

## Build wiring

User's per-zephlet `CMakeLists.txt` is a single call:

```cmake
if(CONFIG_ZEPHLET_<NAME>)
    zephyr_zephlet_generate(
        TYPE    <type>
        PREFIX  <prefix>
        SOURCES <prefix>.c)
endif()
```

`zephyr_zephlet_generate` (in `codegen/zephyr_zephlet_codegen.cmake`) registers the proto for nanopb, invokes `generate_zephlet.py` to emit `<prefix>_interface.{h,c}` into `${CMAKE_BINARY_DIR}/modules/<prefix>/`, exposes includes (`<zephlet_source_dir>`, `${CMAKE_BINARY_DIR}/modules/<prefix>`, `${CMAKE_BINARY_DIR}`), and wires a `zephyr_library` with the user sources + generated source.

Top-level app CMakeLists lists each zephlet dir in `EXTRA_ZEPHYR_MODULES` and registers proto files with nanopb:

```cmake
set_property(GLOBAL PROPERTY PROTO_FILES_LIST)
set(EXTRA_ZEPHYR_MODULES "${CMAKE_SOURCE_DIR}/path/to/tick" "${CMAKE_SOURCE_DIR}/path/to/ui" ...)
find_package(Zephyr REQUIRED ...)
# ... include(nanopb) ...
get_property(LOCAL_PROTO_FILES_LIST GLOBAL PROPERTY PROTO_FILES_LIST)
zephyr_nanopb_sources(app ${LOCAL_PROTO_FILES_LIST})
target_sources(app PRIVATE src/main.c src/policies.c)
```

## Code generation

Single script `codegen/generate_zephlet.py` parses the `.proto` service block, classifies each rpc method by `(req_is_empty, resp_is_empty)`, and renders Jinja templates `codegen/templates/zephlet_interface.{h,c}.jinja` into the build dir. Copier scaffold at `codegen/zephyr_zephlet_template/`.

**Adapters are not generated.** Users write C directly using `ZEPHLET_EVENTS_LISTENER`.

## Kconfig

- `CONFIG_ZEPHLETS=y` — framework (auto `select ZBUS`, `select ZBUS_ASYNC_LISTENER`).
- `CONFIG_ZEPHLET_MAX_INSTANCES=N` — bound for the SYS_INIT ordering buffer. Default 16.
- `CONFIG_ZEPHLET_<TYPE>=y` — per-zephlet switch (selects `NANOPB`).

## Naming

| Element | Pattern |
|---|---|
| User source | `<prefix>.{proto,h,c}` (e.g. `zlet_tick.{proto,h,c}`) |
| Generated | `<prefix>_interface.{h,c}`, `<prefix>.pb.{h,c}` |
| Channels | `chan_<instance>_{command,events}` |
| Listeners | `lis_<type>` (command channel) |
| Handlers | `<type>_<cmd>_impl` (weak), `<type>_<cmd>` (wrapper) |
| Events helper | `<type>_emit(z, &ev, timeout)` |
| Readiness | `<type>_is_ready(z)` |

## Principles

zbus-only coupling. Pointer-in-channel only where sync listener semantics guarantee lifetime. Value-in-channel everywhere else. No pool, no refcount, no correlation IDs. Auto-discover instances via `STRUCT_SECTION_ITERABLE`. Framework takes no position on where zephlets or adapters live in the app tree — layout is entirely user choice.
