# Zephlet Infrastructure Module

Reusable zephlet infrastructure for Zephyr RTOS implementing Ports & Adapters pattern via zbus.

## Quick Start

### 1. Create your west.yml

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

### 2. Initialize and update the workspace

```bash
west init -l .
west update --narrow --fetch-opt=--depth=1
```

### 3. Install Python dependencies

```bash
west packages pip --install
```

### 4. Create a zephlet

```bash
mkdir -p src/zephlets
west zephlet new -n <name> -d "<description>" -a "<author>"
```

If your source lives in a subdirectory (e.g. `app/`), configure the path once:

```bash
west config zephlet.zephlets-dir app/src/zephlets
mkdir -p app/src/zephlets
west zephlet new -n <name> -d "<description>" -a "<author>"
```

### 5. Wire it up

1. Edit `src/zephlets/<name>/zlet_<name>.proto` to define Settings/Events/RPCs
2. Add the zephlet directory to `CMakeLists.txt` `EXTRA_ZEPHYR_MODULES`
3. Enable `CONFIG_ZEPHLET_<NAME>=y` in `prj.conf`
4. Run `west build -b <board>` — the `.c` implementation file is generated once, then never overwritten

## Configuration

### Directory Structure

By default, zephlet commands use:

- Zephlets: `<project>/src/zephlets/`
- Adapters: `<project>/src/adapters/`

Where `<project>` is the directory containing `west.yml` (manifest repository).

**Backwards compatibility:** If `src/<dir>` doesn't exist, falls back to `<dir>` at project root.

### Custom Paths

Override defaults with west config:

```bash
west config zephlet.zephlets-dir <path>
west config zephlet.adapters-dir <path>
```

### Adapters Auto-Creation

The adapters directory is auto-created when running `west zephlet new-adapter` if it doesn't exist, including:

- `src/base_adapter.c` - Shared logging module
- `CMakeLists.txt` - Build configuration
- `Kconfig` - Configuration options
- `zephyr/module.yml` - Module declaration

**Note:** Add `<project>/src/adapters` to root `CMakeLists.txt` `EXTRA_ZEPHYR_MODULES`.

## Dependencies

- Zephyr RTOS (with zbus, nanopb modules)
- Python packages: proto-schema-parser, jinja2, copier

## Architecture

**Two-channel pattern** per zephlet:

- `chan_<zephlet>_invoke`: Receives commands (start/stop/update_settings/etc)
- `chan_<zephlet>_report`: Publishes status/settings/events

**Auto-discovery** via `STRUCT_SECTION_ITERABLE` for zero-config initialization.

**Protobuf-driven codegen** generates interface files from `.proto` definitions.

## Concurrency Model

Every zephlet owns a file-local `struct k_spinlock` that serializes
access to all three storage bags — `status`, `settings`, `events`.
All reads and writes of mutable state, from any context, go through it.

### 1. Spinlock-scoped critical sections

Reads and writes of `<z>_status_storage`, `<z>_settings_storage`, and
`<z>_events_storage` happen inside `K_SPINLOCK(&<z>_lock) { ... }`
blocks. On the single-CPU Cortex-M target this is equivalent to a
brief `irq_lock`/`irq_unlock` pair, so:

- Works identically from thread context and ISR context.
- Never pends, never sleeps, never fails.
- Cost: a few instructions + a few cycles of deferred interrupt
  latency per critical section.

Critical sections are strictly memcpy-scope — copy data in or out,
release immediately. Never hold the spinlock across a `zbus_chan_pub`,
a weak hook, or any call that might sleep.

### 2. Hooks always run unlocked

The generated dispatcher calls weak hooks (`pre_start`, `post_start`,
`validate_settings`, `post_update_settings`, ...) with the spinlock
**released**, so hook code may call any of the internal helpers
(`<pb>_settings_clone()`, `<pb>_events_update(...)`, etc.) — each of
which re-takes the lock briefly on its own.

### 3. Correlation slot (separate sem)

The per-zephlet blocking-helper shared state (`self.expected_cid`,
`self.sync_report`, `self.deadline`) is guarded by a distinct `k_sem`
held for the duration of a blocking call. This is orthogonal to the
storage spinlock — its job is to serialize correlation IDs across
competing blocking callers, not to protect storage.

Direct `chan_<z>_invoke` publishers that bypass the blocking helpers
(e.g., adapters firing a fire-and-forget invoke from their own thread)
**must** set `ivk->has_result = false`, so they never claim a
correlation slot they don't own.

### 4. Internal helpers are the only state API

User code in `zlet_<z>.c` (hooks, custom RPC handlers, init_fn) reaches
state through the generated internal helpers only:

```
struct zephlet_status <pb>_status_clone(void);
struct <pb>_settings  <pb>_settings_clone(void);
struct <pb>_events    <pb>_events_clone(void);
void                  <pb>_settings_init(const struct <pb>_settings *);  /* init_fn only */
int                   <pb>_events_update(struct <zlet>_context *ctx, const struct <pb>_events *delta);
```

Notice the `<pb>_*` prefix (e.g. `tick_*`, `ui_*`) — distinct from the
external `zlet_<z>_*` blocking helpers. The `<pb>_*` names are intra-
zephlet and operate directly on the zephlet's own storage; there is no
`self` parameter because the helpers are generated per-zephlet and
reference their specific storage by name.

### Assumption

This model assumes a single-CPU Zephyr build. On SMP, the per-CPU
spinlock semantics still hold but observers on other CPUs may see
stale cache lines; the sync-listener dispatch also needs a re-audit.
SMP support is future work.

### Why this works

- zbus serializes `api_handler` per channel → data-race-free
- `<z>_sem` serializes correlation slot updates → blocking-safe
- Single-CPU / sync-listener assumption → no SMP reordering to worry
  about

The model assumes a single-CPU Zephyr target with sync listeners. On
SMP, the sync-listener dispatch still happens on the publisher's CPU,
but observers on other CPUs may see stale snapshots through their own
caches; the current sync-listener path has not been audited for that.
SMP support is future work.

## West Commands

**Create zephlet:**

```bash
west zephlet new -n MyZephlet -d "Description" -a "Author"  # Non-interactive
west zephlet new                                            # Interactive (prompts for all fields)
```

**Create adapter:**

```bash
west zephlet new-adapter -o tick -d ui      # Non-interactive (all report fields)
west zephlet new-adapter -i                 # Interactive (select zephlets and fields)
```

**Regenerate interface files:**

```bash
west zephlet gen ZEPHLET  # Requires build/modules/<zephlet>_zephlet to exist first
```

## Integration Testing

New zephlets include `tests/integration/` via Copier template with 6 core lifecycle tests auto-generated:

- `test_start` - Zephlet start command
- `test_stop` - Zephlet stop command
- `test_get_status` - Status query
- `test_lifecycle` - Complete start/stop cycle
- `test_update_settings` - Partial settings update (commented until Settings has fields)
- `test_get_settings` - Full settings snapshot (commented until Settings has fields)

**Test structure:**

- `CMakeLists.txt` - Test target configuration
- `prj.conf` - Test project configuration
- `testcase.yaml` - Test metadata
- `test_*.c` - Test implementation

## Documentation

See [CLAUDE.md](CLAUDE.md) for detailed architecture and implementation guide.

## Example

Reference implementation: https://github.com/rodrigopex/ports_adapters_zbus

## License

Apache-2.0
