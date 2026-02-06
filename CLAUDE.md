# Zephlet Infrastructure

## Commands

**West Extensions** (primary, works from any directory):
- `west zephlet new [-n NAME] [-d DESC] [-a AUTHOR]` - Create zephlet (interactive if no args)
- `west zephlet new-adapter [-o ORIGIN] [-d DEST] [-i]` - Create adapter (-i for interactive)
- `west zephlet gen ZEPHLET` - Regenerate interface files (needs `build/modules/<zephlet>_zephlet`)

**Configuration:**
- `west config zephlet.zephlets-dir <path>` - Custom zephlets location (default: `<project>/src/zephlets/`)
- `west config zephlet.adapters-dir <path>` - Custom adapters location (default: `<project>/src/adapters/`)

**Fallback:** If `src/` subdirectories don't exist, uses root-level directories for backwards compatibility.

**Implementation:** `west/zephlet_commands.py` (registered via `west-commands.yml`)
**Benefits:** Ecosystem integration, auto path detection, dependency validation, works from any dir, structured output with next steps
**Dependencies:** Checks for copier, proto-schema-parser, jinja2 before execution
**Path detection:** Auto-resolves workspace paths from manifest location (`_get_workspace_paths()`)
**Error handling:** Validates zephlet existence, build dirs, provides actionable error messages

## Architecture

**Ports & Adapters** on Zephyr RTOS via **zbus**.

**Components:**

1. Zephlets: Domain logic, no direct dependencies
2. Adapters: Compose zephlets via channel bridging
3. Main: Lifecycle orchestration

**Two-Channel Pattern** per zephlet:

- `chan_<zephlet>_invoke`: Receives commands (START/STOP/CONFIG/etc)
- `chan_<zephlet>_report`: Publishes status/events
  Zephlets listen (sync/async), publish to report channel.

### Zephlet Structure

**Source (VCS):** zlet_<name>.proto, zlet_<name>.c (only this after bootstrap), CMakeLists.txt, Kconfig, module.yml
**Generated (build):** zlet_<name>_interface.h (data/API/inline funcs), zlet_<name>_interface.c (channels/dispatcher/registration), zlet_<name>.h (report helpers), zlet_<name>.pb.h/.pb.c (nanopb)
**Bootstrap:** CMake auto-generates .c once via `--impl-only` if missing. After initial creation, only manually edit .c.

**\_interface.h:** `<zephlet>_data` (state+spinlock), `<zephlet>_api` (func ptrs with context param), inline `<zephlet>_start(correlation_id, timeout)` → pub invoke chan with optional context
**\_interface.c:** Dispatcher (switch/case oneof tags + default), extracts context from invoke, passes to API functions, `<zephlet>_set_implementation()`
**.c:** Init func (sets is_ready), API impls (add context param), K_SPINLOCK updates, pub reports with context+ret, ends with `ZEPHLET_DEFINE(<zephlet>, init_fn, &api, &data)`. Use `report_*_async()` for events without correlation.
**Shared:** `struct zephlet` + `ZEPHLET_DEFINE()` → `STRUCT_SECTION_ITERABLE` discovery

**API signatures:** All API functions: `int fn(const struct zephlet*, const struct msg_api_context*, [params...])`. Inline helpers: `int <zephlet>_cmd(uint32_t correlation_id, [params...], k_timeout_t)`. Report helpers: `int <zephlet>_report_*(const struct msg_api_context *context, int ret, [data...], k_timeout_t)`. Async variants: `int <zephlet>_report_*_async([data...], k_timeout_t)` for events/timers.

### Adapters

Listen to zephlet report → invoke another zephlet. No direct coupling.
Uses `ZBUS_ASYNC_LISTENER_DEFINE()` + `ZBUS_CHAN_ADD_OBS(priority=3)`. Kconfig toggleable.
**base_adapter.c:** Registers shared logging module `LOG_MODULE_REGISTER(adapter, CONFIG_ADAPTERS_LOG_LEVEL)`. All adapters use `LOG_MODULE_DECLARE(adapter, CONFIG_ADAPTERS_LOG_LEVEL)`.
**Include pattern:** Interface headers only (no .pb.h). Standardized: interface includes, blank line, zephyr includes, blank line.

### Protobuf (nanopb)

`MsgZlet<Zephlet> { Config{}, Events{}, Invoke{oneof}, Report{oneof} }`. Invoke: start/stop/get_status/config/get_config+custom. Report: status/config+events. Import "zephlet.proto" for Empty/MsgZephletStatus. Use `option (nanopb_fileopt).anonymous_oneof = true`. Query: get_status/get_config → reports. PROTO_FILES_LIST → nanopb. RPC return types strictly validated against Invoke/Report fields.

**Lifecycle Pattern:** Standard fields explicitly listed in all zephlet protos. Reserved numbers: Invoke 1-6 (start, stop, get_status, config, get_config, get_events), Report 1-3 (status, config, events). Custom commands/reports start at 7+/4+. Comments mark custom field ranges.

**Field validation:** `validate_field_numbers()` enforces reserved ranges at build time. Checks duplicates, standard names at reserved numbers (fails build), warns on gaps (non-fatal). Actionable error messages guide fixes.

**Request-response context:** `MsgAPIContext {correlation_id, return_code}` enables invoke-report correlation + error propagation. `optional MsgAPIContext context = 999` in Invoke/Report. correlation_id=0 means fire-and-forget. return_code follows POSIX errno (0=success, <0=error). `has_context` distinguishes responses from async events.

**Lifecycle state:** `MsgZephletStatus {is_running, is_ready}`. `is_ready` set by init (SYS_INIT), `is_running` controlled by start/stop. Init happens once, start/stop multiple times.

**nanopb types:** Use `struct msg_<zephlet>_{invoke|report|config}`. Naming: `MsgZephlet.Invoke` → `msg_zephlet_invoke`, tags: `MSG_ZEPHLET_INVOKE_START_TAG`, oneof: `which_<oneof_name>`.

## Build System

**CMake function:** `zephyr_zephlet_generate(<proto_path>)` - auto-generates interface files, handles bootstrap with `--impl-only` flag.
**Include strategy:** shared_zephlet exposes `${CMAKE_BINARY_DIR}/zephlets` globally (for .pb.h). Each zephlet exposes build dir for interface headers via `zephyr_include_directories("${CMAKE_CURRENT_BINARY_DIR}")` after `zephyr_zephlet_generate()`. Interface library pattern: `zephyr_interface_library_named()` for include propagation.
**Proto collection:** Each zephlet appends to `PROTO_FILES_LIST` global property, root retrieves and passes to `zephyr_nanopb_sources()`.
**Dependencies:** Requires proto-schema-parser, jinja2 Python packages.
Copier template auto-includes pattern. Self-managed visibility, no manual root updates.

## Creating Zephlets

**Workflow:** `west zephlet new` → Copier creates .proto/.c/CMakeLists/Kconfig/module.yml → Edit .proto (Config/Events/RPCs) → `just b` once (bootstrap .c if needed) → Edit .c (TODOs) → Add to root CMakeLists EXTRA_ZEPHYR_MODULES → Enable CONFIG_<ZEPHLET>_ZEPHLET=y → rebuild

**Non-interactive:** `west zephlet new -n NAME -d "Description" -a "Author"`
**Interactive:** `west zephlet new` (prompts for all fields)

**Build-time codegen:** .proto changes → auto-regen \_interface.h/\_interface.c/<zephlet>.h/.pb.h/.pb.c. Manual: `west zephlet gen <zephlet>` (requires `build/modules/<zephlet>_zephlet` exists first).
**Bootstrap behavior:** First build auto-generates .c via `--impl-only` if missing. After that, only manually edit .c.

**Files:** VCS: .proto, .c (after bootstrap), CMakeLists, Kconfig, module.yml. Build: \_interface.h, \_interface.c, <zephlet>.h, .pb.h, .pb.c

## Creating Adapters

**Workflow:**

1. `west zephlet new-adapter` → select origin/dest → select report fields
2. Implement TODO comments in generated `<Origin>+<Dest>_zlet_adapter.c`
3. Enable CONFIG_<ORIGIN>\_TO\_<DEST>\_ADAPTER=y

**Non-interactive:** `west zephlet new-adapter -o ORIGIN -d DEST` (generates all report fields)
**Interactive:** `west zephlet new-adapter -i` (prompts for zephlets and report fields)

Auto-generates: adapter.c (with interface includes only, no .pb.h), Kconfig entry, CMakeLists.txt entry. Parses protos for Report fields (origin) + Invoke fields (dest, adds API suggestions to TODOs). Duplicate detection, manual fallback on auto-update failure.

## Configuration

Via Kconfig in `prj.conf`:

- `CONFIG_ZEPHLET_<ZEPHLET>=y` / `CONFIG_ZEPHLET_<ZEPHLET>_LOG_LEVEL_DBG=y`
- `CONFIG_<ADAPTER>_ADAPTER=y`

## Naming

| Element | Pattern | Example |
|---------|---------|---------|
| **Source files** | `zlet_<name>.{proto,c}` | zlet_tick.proto, zlet_tick.c |
| **Generated files** | `zlet_<name>_interface.{h,c}`, `zlet_<name>.h` | zlet_tick_interface.h, zlet_tick.h |
| **Adapter files** | `<Origin>+<Dest>_zlet_adapter.c` | Tick+Ui_zlet_adapter.c |
| **Adapter function** | `<origin>_to_<dest>_adapter` | tick_to_ui_adapter |
| **Channels** | `chan_<zephlet>_{invoke\|report}` | chan_tick_invoke |
| **Listeners** | `lis_<zephlet>`, `lis_<origin>_to_<dest>_adapter` | lis_tick, lis_tick_to_ui_adapter |
| **Messages** | `msg_<zephlet>_{invoke\|report\|config}` | msg_tick_invoke |
| **Structs** | `<zephlet>_{data\|api}` | tick_data, tick_api |
| **Functions** | `<zephlet>_<cmd>()` (inline), `static int <cmd>(zephlet*)` | tick_start(), static int start() |
| **Config** | `CONFIG_ZEPHLET_<ZEPHLET>`, `CONFIG_<ORIGIN>_TO_<DEST>_ADAPTER` | CONFIG_ZEPHLET_TICK, CONFIG_TICK_TO_UI_ADAPTER |

## Data Flow

Init: init_fn calls init() (sets is_ready) → register impl. Request-response: inline func (with correlation_id) → invoke chan (with context) → dispatcher extracts context → api->cmd(context) → spinlock update → report(context, ret) → observers check has_context + return_code. Async events: timer → report_*_async() (no context) → observers see has_context=false. Query: get_config(context) → dispatcher → api → read → pub with context+ret.

## Principles

Loose coupling (zbus only). Separation (.h/.c/\_impl.c). Single return (`ret`, `goto end`), expect for guards, you can have several. No direct deps (except inline API). Composition via adapters. Type safety (proto/structs). Thread safety (K_SPINLOCK). Pluggable (registration). Async (ZBUS_ASYNC_LISTENER). Lifecycle (START/STOP/get_status/get_config). Auto discovery (STRUCT_SECTION_ITERABLE).

## Code Generation

### Protobuf (nanopb)

PROTO_FILES_LIST → zephyr_nanopb_sources() → .pb.h/.pb.c (build dir). Don't edit.

### Zephlet Generator

Script: `codegen/generate_zephlet.py`. Proto → \_interface.h/\_interface.c/<zephlet>.h/.c template. Never overwrites .c unless `--impl-only`.

**Generated:** \_interface.h (data/API/inline funcs), \_interface.c (channels/dispatcher), <zephlet>.h (report helpers), .c template (bootstrap only)
**Hand-written:** .proto, .c (business logic)

**Flags:** `--generate-impl`, `--no-generate-impl`, `--impl-only` (bootstrap mode - generates only .c if missing)

**Workflow:**

- New: Write .proto → first build auto-bootstraps .c → complete .c TODOs → rebuild
- Modify: Edit .proto → auto-regen interfaces (no .c overwrite) → update .c → build

**Process:** proto-schema-parser extracts service/invoke/report/config → Jinja2 templates → files. Proto oneof → API func ptrs + inline funcs + dispatcher switch/case.

**RPC validation:** Strict type checking - `returns MsgZephletStatus` → `report_status()`, `returns Config` → `report_config()`, `returns Events (stream)` → `report_events()`. Validates RPC return types match Invoke/Report fields.

**Helper header:** `<zephlet>.h` (not `_priv.h`) with `<zephlet>_report_*()` helpers. Wraps zbus_chan_pub. Type mapping: MsgZephletStatus→ptr, Config→ptr, Empty→no params, CamelCase→snake_case.

**Templates:** `codegen/templates/` - 6 Jinja2 templates (zephlet.h→_interface.h, zephlet.c→_interface.c, zephlet_priv.h→.h, zephlet_impl.c→.c, adapter.c, adapter_kconfig). Filters: `|camel_to_snake`, `|upper`, `|lower`. Copier template: `zephyr_zephlet_template/` (initial 5 files).

### Adapter Generator

Script: `codegen/generate_adapter.py`. Parses origin proto (Report fields) + dest proto (Invoke fields) → generates adapter.c + updates Kconfig/CMakeLists.txt.

**Process:** Scans for protos → proto-schema-parser extracts Report/Invoke oneofs → Jinja2 templates (adapter.c.jinja, adapter_kconfig.jinja) → writes files. Duplicate detection, graceful degradation.

**Interactive:** User selects report fields to handle. **Non-interactive:** All fields.
**API suggestions:** Dest Invoke fields → TODO comments (`/** Available ui commands: start, stop */`)

**Include pattern:** Interface headers only (standardized: interface includes, blank line, zephyr includes, blank line). No .pb.h includes.

**Auto-updates:** Kconfig (before `module =` with proper blank line spacing), CMakeLists.txt (after last zephyr_library_sources). Manual fallback on failure.

**Naming:** File=`<Origin>+<Dest>_zlet_adapter.c` (CamelCase), Config=`CONFIG_<ORIGIN>_TO_<DEST>_ADAPTER`, Listener=`lis_<origin>_to_<dest>_adapter`, Function=`<origin>_to_<dest>_adapter`
