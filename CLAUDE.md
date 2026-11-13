# Zephlet Infrastructure v0.3

**`v0.3` here is the architecture generation, not a release tag.** It names the
contract below — two channels per instance, `ZEPHLET_NEW`, weak handler
overrides — and has not changed since the v0.3.0 tag. Releases within it are
`v0.3.x`; current is **`v0.3.4`** (the `zlet` shell frontend; `v0.3.3` was CoAP
frontend maturation). Consumers pin a tag — see README's Releases section.

Ports+Adapters on Zephyr/zbus. Zephlets=domain logic; a zephlet depends only on provider types its proto declares (`(zephlet.requires)`), bound per instance by the app (see Composition). Adapters=plain user C composed from a framework-level observer primitive. Instances are non-singleton; multiple per type.

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

struct zephlet_dep_slot { const char *name, *type; const struct zephlet_api *api; size_t offset; };

struct zephlet_api { const struct zephlet_method *methods; size_t num_methods;
                     const struct zephlet_dep_slot *deps; size_t num_deps; };

struct zephlet { const char *name; const struct zephlet_api *api;
                 struct { const struct zbus_channel *command, *events; } channel;
                 void *config; void *data; const void *dependencies;
                 int (*init_fn)(const struct zephlet *); int init_priority; };

int zephlet_dispatch(const struct zephlet *z, struct zephlet_call *call);
const struct zephlet *zephlet_get_by_name(const char *name);
int zephlet_init_result(const struct zephlet *z); /* 0, init rc, -ENODEV, -EAGAIN, -ENOENT */
int zephlet_init_one(const struct zephlet *z);    /* what an init entry runs; once */
```

**`ZEPHLET_NEW(type, name, cfg, data, init)`** instantiates one zephlet: creates `chan_<name>_command` (pointer, observer = `lis_<type>`), `chan_<name>_events` (value-typed `struct <type>_events`), a section-iterable `struct zephlet` record whose `runtime` points at its `struct zephlet_runtime` (a plain static in `.bss`), an undefined `_zlet_tag_<name>` typed `struct <type>_zlet_tag *` (read by `ZEPHLET_DEP`), and its own SYS_INIT entry: `ZEPHLET_INIT_DEFINE(name, _ZLET_INIT_PRIO_<type>, init_priority)` = extern decl + `ZEPHLET_RUNTIME_DEFINE` + `ZEPHLET_INIT_ENTRY`. `cfg` is a writable pointer (the `config` command mutates it in place).

**Init.** Each instance's SYS_INIT entry (APPLICATION, priority `_ZLET_INIT_PRIO_<type>` = `CONFIG_ZEPHLET_INIT_PRIORITY` (default 10, BUILD_ASSERTed > `CONFIG_ZBUS_CHANNELS_SYS_INIT_PRIORITY` so an `init_fn` may call provider RPCs) + init rank; sub-priority = `init_priority`, a bare literal 0–9 (`STRINGIFY` length + `UTIL_INC` check: `(2)`, `1+1`, `10` fail to build), linker sorts it as text; ties unspecified) calls `zephlet_init_one()`: FINAL → its result (SERVING, i.e. re-entered from its own `init_fn` → `-EAGAIN`); any bad slot (no record, unbound, not an instance of the section with a runtime record, wrong api; `provider_of()` returns NULL, never followed) or a provider not FINAL / result ≠ 0 → `-ENODEV`, logged; else set SERVING, run `init_fn`, then FINAL|SERVING on 0, FINAL on error. Rank 0 = no providers, else 1 + max provider rank. Cycles are a build error (acyclic type graph; a slot only takes its own type); a hand-written one (or a consumer ranked before its provider) finds a provider not initialized yet → `-ENODEV`, no cyclic `init_fn` runs. Hand-written records: `ZEPHLET_INIT_DEFINE(name, prio, sub)` before a const record with `.runtime = ZEPHLET_RUNTIME(name)` (absolute priority literal; uses Zephyr's internal `Z_INIT_ENTRY_SECTION`, revisit on upgrades); mutable test records use `ZEPHLET_RUNTIME_DEFINE` + `ZEPHLET_INIT_ENTRY`. **One gate for every instance** in `zephlet_dispatch` (O(1) through `z->runtime`, no division): no runtime or not run → `-EAGAIN`; SERVING (init_fn running, or returned 0) → served; else `-ENODEV`. No heap, no recursion. `zephlet.c` BUILD_ASSERTs the pointer-sized struct layout `codegen/check_deps_elf.py` reads (`struct zephlet` = 10 pointers, `runtime` at 8).

## Composition

- **Declare:** `option (zephlet.requires) = { type: "fan" name: "intake" };` (repeatable, `ZephletRequires` in `zephlet_options.proto`, ext 50005) or `<Svc> requires:[<name>=]<type>` in `.zephlet.options`; both add up. `type` = provider type_snake (not prefix); `name` defaults to `type`.
- **Codegen checks:** C identifier, name not a keyword, unique names, no self type. `--check-graph t=d,...` checks a graph (missing type, cycles any length, prints path); `--check-build TYPE PROTO OPTIONS` (repeated) reads it from every zephlet's proto, `--check-requires TYPE FILE` (what CMake uses) from the `<prefix>.requires` each zephlet's own generation writes with `--requires-out` (sorted, only when changed, a restat BYPRODUCT), `--stamp` on success, `--init-prio-header PATH --init-base N` writes `_ZLET_INIT_PRIO_<type>` = N + rank (only when changed; error above 999), included by every interface header. CMake: `zephyr_zephlet_generate` appends `--check-requires <type> <prefix>.requires` to GLOBAL `_ZEPHLET_GRAPH_ARGS` and the file to `_DEPENDS`; a DEFERred `_zephlet_check_dependency_graph` adds build step `zephlet_dependency_graph` (stamp `zephlet_dependency_graph.txt`) hooked into `zephyr_generated_headers` (header `zephlet_init_prio.h` as BYPRODUCT, restat), so it runs before every compile and a proto edit never reconfigures.
- **Generated:** `struct <t>_dependencies { const struct zephlet *<name>; ... }` (handlers read `z->dependencies`, like `config`/`data`), `_ZLET_HAS_DEPS_<t>` (0/1), `<t>_dep_slots[]` wired into `<t>_api.deps`.
- **Bind:** slots with `ZEPHLET_DEP(type, inst)` (`_Generic` on `_zlet_tag_<inst>`: another type fails to compile; needs `ZEPHLET_NEW*`/`ZEPHLET_DECLARE(type, inst)` in the TU; plain `&inst` still compiles). `ZEPHLET_NEW_WITH_DEPS(type, name, cfg, data, &deps, init)` / `ZEPHLET_NEW_PRIO_WITH_DEPS(..., prio)`, auto-storage `ZEPHLET_[PRIO_]INST_DEFINE_WITH_DEPS(type, id, &deps, init[, prio])`; `_Generic` rejects a record of another type; `ZEPHLET_NEW*` BUILD_ASSERTs the type has no requires (and vice versa). Providers sharable; no ownership transfer.
- **Link check:** `CONFIG_ZEPHLET_DEPS_LINK_CHECK` (default y) appends `codegen/check_deps_elf.py <zephyr.elf>` to `extra_post_build_commands`: reads the instance section, apis, slots and records (pyelftools, pointer-sized fields; NOBITS/`.bss` reads as zeros, so a zeroed record is all unbound), fails on missing record, unbound / non-instance / wrong-type slot. Instance cycles need a type cycle (a slot only accepts its own type), which the graph step rejects. Off in `deps_functional`/`deps_init`, which bind badly on purpose.
- **Call:** provider's public wrappers only, sync, caller's thread, never from an ISR (real timeouts). zbus holds the consumer's command-channel lock for the whole provider call: no deadlock (consumer → provider order, acyclic), but a slow provider blocks every caller and timeouts add up. Every wrapper returns `-EINVAL` unless `z->api == &<t>_api`.
- **Tests:** `tests/codegen` (requires/graph), `tests/codegen/test_init_prio.py` (ranks, header), `.requires` round-trip in `test_codegen.py`, `tests/deps_config` (pytest: graph check at build, consumer object alone, macro misuse, proto edit w/o reconfigure, link check accepts/rejects incl. a `.bss` record, mistyped `ZEPHLET_DEP` fails to compile, sub-priority `10`/`x`/`1+1`/`(2)` fail, header follows ranks w/o reconfigure), `tests/deps_functional` (climate fixture; an `init_fn` calling its providers and its own RPC), `tests/shared/deps_init` (hand-built records with `ZEPHLET_INIT_DEFINE`, cycles → `-ENODEV`), `tests/shared/init_unit` (FFF, `unit_testing`: real `zephlet.c`, a test linker script collects the instance section and the init entries (kernel's three-pattern sort); each test rebinds n0..n8 and runs `zephlet_init_one()` in a chosen order or boots the entries; covers the gate for every state).

## Per-zephlet files

- **User-owned (source tree):** `<prefix>.proto`, optional `<prefix>.zephlet.options` (frontend opt-ins `coap`, `coap_discoverable`, `lua`, set here in nanopb `.options` syntax, e.g. `TickApi coap:true lua:true`, and/or in the proto as `option (zephlet.<key>) = true;` from `zephlet_options.proto`; proto overrides the file, as in nanopb; per-rpc key `shell` (default true) set in the rpc body `{ option (zephlet.shell) = false; }` or as `TickApi.config shell:false` (rpc half fnmatch), hides that rpc from the shell tables only — method ids untouched, paste prefix skips hidden rpcs, all hidden → empty shell hook), `<prefix>.h` (declares `struct <type>_data` + `int <type>_init_fn(const struct zephlet *)`), `<prefix>.c` (strong handler overrides), `CMakeLists.txt` (single `zephyr_zephlet_generate(TYPE ... PREFIX ... SOURCES ...)` call), `Kconfig`, `zephyr/module.yml`.
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
  message Config { uint32 duration_ms = 1; uint32 period_ms = 2; }
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
  The shell frontend's descriptor emission does handle a oneof-only message (codegen counts oneof
  members, which the proto AST nests one level down), so lifting this would not silently break the
  link — but no oneof is exercised end to end, so treat the constraint as standing until it is.
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
- `CONFIG_ZEPHLET_MAX_INSTANCES=N` — expected instance count, default size of frontend per-instance pools (CoAP Observe). Default 16. Init has no limit.
- `CONFIG_ZEPHLET_DEPS_LINK_CHECK=y` — post-link dependency check (see Composition).
- `CONFIG_ZEPHLET_<TYPE>=y` — per-zephlet switch (selects `NANOPB`).
- `CONFIG_ZEPHLETS_SHELL=y` — shell frontend. Registers every instance's
  RPCs (minus `shell:false` ones) under one `zlet` root command, via the per-type
  `_ZLET_SHELL_HOOK_<type>` hook that `ZEPHLET_NEW` expands; no app code.
  **`select`s `NANOPB_TEXTFORMAT`, so the app's west manifest must carry
  [`zephyr-nanopb-textformat`](https://github.com/rodrigopex/zephyr-nanopb-textformat)**
  (pin a tag — pre-1.0, API has already moved twice: v0.3.0 replaced the
  per-call `flags` with Kconfig options, v0.7.0 replaced those with named
  wrappers and made every function return `int`, and v0.8.0 changed what
  the printer emits -- repeated fields fold into `[a, b]`, and multi-line
  is fit-or-expand rather than one field per line. Tested against
  v0.8.1.)
  Selects both `NANOPB_TEXTFORMAT_PARSE` and `NANOPB_TEXTFORMAT_PRINT`:
  requests are parsed, responses printed, and since v0.5.0 either half can
  be configured out. A response prints twice — an indented block via
  `pb_tf_print_multiline()`, then the compact form prefixed with the
  command that writes it back (`zlet <inst> <rpc> <msg>`), so a nested
  response is readable *and* still pasteable. The prefix comes from
  codegen matching a response's type against every RPC's request type;
  `NULL`, hence no prefix, when nothing accepts that type
  (`Lifecycle.Status`). `ZEPHLETS_SHELL_PRINT_{BOTH,PRETTY,COMPACT}`
  picks the default form and `zlet_fmt` switches it at run time when
  `ZEPHLETS_SHELL_PRINT_RUNTIME=y`. An RPC takes its request as a protobuf text-format message
  (`zlet tick_a config duration_ms: 100, period_ms: 10`), delivered as one
  `SHELL_OPT_ARG_RAW` argument and parsed by that library, so every field
  shape works without codegen walking fields. Codegen emits one
  `PB_TF_DEFINE` per message with at least one field into
  `<prefix>_interface.c`, and a `PB_TF_DECLARE` into the header. Needs no
  companion Kconfig beyond `CBPRINTF_FULL_INTEGRAL` for fields wider than
  32 bits; see `docs/adr/0001-zephlet-frontends.md` for why
  `SHELL_WILDCARD` is *not* one of them.

- `CONFIG_ZEPHLETS_LUA=y` — Lua frontend (`frontends/lua/`, guide in its
  README). `depends on LUA_ASYNC` (lua_zephyr module, pinned by SHA in the
  app manifest), `select`s `NANOPB_TEXTFORMAT` for descriptors. Per-type
  opt-in `lua` (proto option or `<prefix>.zephlet.options`); without it the hook is
  empty and the Lua outputs are stubs. Hook
  `_ZLET_LUA_HOOK_<type>` emits per instance a
  `zephlet_lua_instance` record (`__zlet_lua__<type>__<inst>`) plus a
  forwarding `ZEPHLET_EVENTS_LISTENER` that copies events into C staging.
  Codegen adds `<prefix>_lua_interface.{h,c}` (trampolines → typed wrappers,
  method/enum tables; compiled only `=y`) and `<prefix>_lua_meta.lua` (LuaLS).
  `select`s `LUA_PRECOMPILE`: the app embeds its script with
  `luaz_add_bytecode_file()` (the prelude too, via a `cmake_language(DEFER)`
  call, since lua_zephyr is processed after this module) and calls
  `zephlet_lua_coordinate(ZEPHLET_LUA_SCRIPT(app))` from `main()`; the
  main chunk adds policies (`inst:add_policy(fn)`), the seal freezes them,
  then the `zephyr.async` loop runs the pump + dispatcher tasks. The script's
  own tasks wait with `inst:await()` / `z.await_any()` (per-task queues,
  `ZEPHLETS_LUA_AWAIT_DEPTH`): queues/routing in C (`zephlet_lua_await.c`),
  only the parking loop in the prelude, on the same doorbell; lua_zephyr is
  never patched. Keep Lua code minimal: its protos live on the Lua heap.
  GC: `ZEPHLETS_LUA_GC_PAUSE` (100) + `GC_IDLE_STEP`. Tests: `tests/lua_functional`
  (console harness, native_sim).

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

zbus-only coupling; peer calls only through declared, externally bound dependencies. Pointer-in-channel only where sync listener semantics guarantee lifetime. Value-in-channel everywhere else. No pool, no refcount, no correlation IDs. Auto-discover instances via `STRUCT_SECTION_ITERABLE`. Framework takes no position on where zephlets or adapters live in the app tree — layout is entirely user choice.
