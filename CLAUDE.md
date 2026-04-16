# Zephlet Infrastructure v0.3.0

Ports+Adapters on Zephyr/zbus. Zephlets=domain logic (no direct deps), adapters=bridge zephlet reports→invokes, main=lifecycle.

## Commands (west)

- `west zephlet new [-n -d -a]` — interactive w/o args
- `west zephlet new-adapter` — always interactive
- `west zephlet gen ZEPHLET` — regen interfaces (needs `build/modules/<z>_zephlet`)
- `west config zephlet.{zephlets-dir,adapters-dir} <path>` — default `<project>/src/{zephlets,adapters}/`, falls back to root dirs
- impl: `west/zephlet_commands.py` via `west-commands.yml`. Deps checked: copier, proto-schema-parser, jinja2. Workspace paths auto-resolved from manifest.

## Concurrency model

No spinlocks. Two invariants:

1. **Data invariant.** `struct zephlet_data` mutates only from inside the zephlet's `api_handler`, which zbus serializes per-channel (sync listener). Plain reads/writes are race-free.
2. **Correlation invariant.** Blocking-helper `self.*` state is touched only while `<z>_sem` is held. Direct invoke publishers must set `ivk->has_result = false`.

Assumes single-CPU + sync-listener. SMP is future work.

## Channels

Per zephlet: `chan_<z>_invoke` (cmds), `chan_<z>_report` (status/settings/events). Zephlets listen sync, publish to report.

## Zephlet files

- **VCS:** `zlet_<n>.proto`, `zlet_<n>.c` (after bootstrap), CMakeLists.txt, Kconfig, module.yml
- **Generated:** `zlet_<n>_interface.{h,c}` (types/api/dispatcher/channels/weak hooks/lifecycle shims/blocking helpers/`<z>_set_implementation`), `zlet_<n>.h` (async report helpers), `.pb.{h,c}`
- **Bootstrap:** CMake auto-gens `.c` via `--impl-only` if missing; hand-edit only.
- `_interface.h` exports: `extern <z>_api`, `extern <z>_data` (base `zephlet_data`), `extern <z>_config` (base `zephlet_config`, const), `<z>_context` (self+response+deadline), weak hook decls, typed `<z>_settings_mut(self)` / `<z>_events_mut(self)` accessors, blocking call decls.
- `.c` (user-owned): non-lifecycle RPC bodies (external linkage: `int <z>_<method>(ctx, …)`), strong hook overrides, `init_fn` (seeds Settings defaults; framework auto-marks ready on return 0). Ends `ZEPHLET_DEFINE(<z>, init_fn, &<z>_api, &<z>_config, &<z>_data)`.
- Shared base: `struct zephlet{name,channel,init_fn,api,config,data}`, `struct zephlet_data{status}`, `struct zephlet_config{settings_storage,settings_size,events_storage,events_size}`, `ZEPHLET_DEFINE(name,init,api,config,data)`, `ZEPHLET_CALL_OK()`, `<z>_is_ready()` (per-zephlet, zero-arg), `zephlet_{start,stop,get_status,get_settings,update_settings,get_events}_core()` (pure state machine).

## Lifecycle: shims + weak hooks

Generated shims in `<z>_interface.c` wrap the base core with weak-symbol hooks. Users opt into side effects by providing strong definitions of any of:

```
int <z>_pre_start(const struct zephlet *self);
int <z>_post_start(const struct zephlet *self);   /* failure rolls back via stop_core */
int <z>_pre_stop(const struct zephlet *self);
int <z>_post_stop(const struct zephlet *self);
int <z>_validate_settings(const struct <z>_settings *merged);
int <z>_post_update_settings(const struct zephlet *self);
```

Defaults are `__weak` no-ops. `update_settings` merges the partial delta using nanopb `has_<field>` flags (generated merge helper), validates the merged state (not the delta), then commits.

## Blocking API (gRPC-style)

All unary RPCs blocking. `<report> <z>_<cmd>([typed ptr], k_timeout_t)` returns by value. `ZEPHLET_CALL_OK(r)` = `r.has_result && r.result.return_code==0`. Errors: -ETIMEDOUT / -EBUSY / -EALREADY / -EINVAL or app errno in `return_code`. `result.invoke_tag` = which RPC.

Async events (impl-side): `int <z>_report_*_async([data], k_timeout_t)` — has_result=false.

## Adapters

Listen zephlet report → invoke another zephlet. Zero direct coupling. `ZBUS_ASYNC_LISTENER_DEFINE` + `ZBUS_CHAN_ADD_OBS(prio=3)`. Kconfig toggleable.
`base_adapter.c`: `LOG_MODULE_REGISTER(adapter, CONFIG_ADAPTERS_LOG_LEVEL)`. Others: `LOG_MODULE_DECLARE(...)`.
Includes: interface headers only (no .pb.h). Order: interface, blank, zephyr, blank.

## Protobuf (nanopb)

`<Z> { Settings, Events, Invoke{oneof}, Report{oneof} }`. Import `zephlet.proto` for Empty/ZephletStatus. Options: `anonymous_oneof=true`, `long_names=false`. Query RPCs (`get_status`/`get_settings`/`get_events`) return reports. Proto collected via `PROTO_FILES_LIST`→`zephyr_nanopb_sources()`. Don't edit generated.

**Proto3 required.** Every `Settings` scalar field MUST be declared `optional` so nanopb emits `has_<field>` companions that drive the partial-merge helper. Codegen rejects non-optional Settings fields.

**Lifecycle reserved:** Invoke 1-6 = start, stop, get_status, update_settings, get_settings, get_events. Report 1-4 = empty, status, settings, events. Custom: Invoke 7+, Report 5+. `validate_field_numbers()` enforces at build time.

**Result:** `ZephletResult {correlation_id, return_code, invoke_tag}` at `optional result = 999` in Invoke/Report. Blocking calls auto-fill correlation_id + invoke_tag. `return_code` = POSIX errno (0=ok). `has_result` separates responses from async events.

**Status:** `ZephletStatus {is_running, is_ready}`. is_ready set by `init_fn` on success; is_running toggled by start_core/stop_core.

**Generated C types:** `struct <z>_{invoke,report,settings,events}`, tags `<Z>_INVOKE_<CMD>_TAG`, oneof selector `which_<name>`.

## Build system

- `zephyr_zephlet_define(<name> [INCLUDE_DIRS ...] [SRCS ...])` — single-line `CMakeLists.txt` per zephlet. Wraps `CONFIG_ZEPHLET_<N>` guard. Does proto gen + codegen + interface lib + zephyr lib.
- `zephyr_zephlet_generate(<proto>)` — generates interfaces, bootstraps `.c` via `--impl-only` if missing.
- `shared_zephlet` exposes `${CMAKE_BINARY_DIR}/zephlets` globally for .pb.h.
- Proto collection: append to `PROTO_FILES_LIST` global, root→`zephyr_nanopb_sources()`.
- Py deps: proto-schema-parser, jinja2.

## Workflows

**New zephlet:** `west zephlet new` → copier writes 5 files → edit `.proto` (Settings with `optional` fields + Events + custom RPCs) → `just b` (bootstraps `.c`) → fill TODOs (non-lifecycle RPC bodies + optional hook overrides + init_fn defaults) → add to root `EXTRA_ZEPHYR_MODULES` → `CONFIG_ZEPHLET_<Z>=y` → rebuild.

**Modify:** edit `.proto` → auto-regen interfaces (never overwrites `.c`) → update `.c` → build.

**New adapter:** `west zephlet new-adapter` → prompts origin/dest + report fields → fill TODOs → `CONFIG_<O>_TO_<D>_ADAPTER=y`. Auto-writes `.c` + Kconfig + CMakeLists entries.

## Kconfig

`CONFIG_ZEPHLET_<Z>=y`, `CONFIG_ZEPHLET_<Z>_LOG_LEVEL_DBG=y`, `CONFIG_<O>_TO_<D>_ADAPTER=y`.

## Naming

| Elem | Pattern |
|---|---|
| Source | `zlet_<n>.{proto,c}` |
| Generated | `zlet_<n>_interface.{h,c}`, `zlet_<n>.h` |
| Adapter file | `<Origin>+<Dest>_zlet_adapter.c` (CamelCase) |
| Adapter fn | `<origin>_to_<dest>_adapter` |
| Channels | `chan_<z>_{invoke,report}` |
| Listeners | `lis_<z>`, `lis_<o>_to_<d>_adapter` |
| Messages | `<z>_{invoke,report,settings,events}` |
| Api funcs | shim: `<z>_shim_<cmd>()` (static); user: `<z>_<method>()` (extern) |
| Weak hooks | `<z>_{pre,post}_{start,stop}`, `<z>_{validate,post_update}_settings` |
| Storage | `<z>_settings_storage`, `<z>_events_storage` (static in interface.c) |
| Config | `CONFIG_ZEPHLET_<Z>`, `CONFIG_<O>_TO_<D>_ADAPTER` |

## Data flow

Init: `init_fn` → seed settings defaults → `set_implementation()` → return 0 → framework auto-marks `is_ready`. Blocking: `<z>_<cmd>(ptr, t)` → sem_take → correlation_id → pub invoke (result+invoke_tag) → sync listener filters → sem_give → return report. Async: timer/ISR → `report_*_async()` (no result) → observers see `has_result=false`. Errors: -EBUSY (sem), -ETIMEDOUT (wait), app errno in return_code.

## Principles

zbus-only coupling. No spinlocks (zbus serializes). No direct deps except inline API. Auto-discover via `STRUCT_SECTION_ITERABLE`. Settings must be `optional` for partial merge.

## Code generation

Scripts: `codegen/generate_proto.py` (schema+base→full proto, validates Settings-optional), `codegen/generate_zephlet.py` (parses full proto, validates Settings-optional + field-number conventions, renders templates), `codegen/generate_adapter.py`. Templates: `codegen/templates/` — `zephlet.h.jinja→_interface.h`, `zephlet.c.jinja→_interface.c`, `zephlet_priv.h.jinja→.h`, `zephlet_impl.c.jinja→.c` (bootstrap only), adapter templates. Filters: `camel_to_snake`, `proto_type_to_snake`, `upper`, `lower`. Copier: `zephyr_zephlet_template/`.

**Flags:** `--generate-impl` / `--no-generate-impl` / `--impl-only` (bootstrap).
**Parser:** proto-schema-parser extracts service/invoke/report/settings oneofs → api func ptrs + dispatcher switch/case + merge helper.
**RPC validation:** return type must match Report oneof field (`ZephletStatus`→status, `Settings`→settings, `Events`→events).
**Helper header `<z>.h`:** `report_*_async()` only. Interface layer owns correlated publishing.
