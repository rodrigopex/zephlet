# Zephlet Infrastructure Module

Reusable zephlet infrastructure for Zephyr RTOS implementing Ports & Adapters pattern via zbus.

## Quick Start

### Add to west.yml

```yaml
projects:
  - name: zephlet
    url: https://github.com/rodrigopex/zephlet
    revision: main
    path: modules/lib/zephlet
```

### Install dependencies

Explicitly install required Python packages:

```bash
pip install -r modules/lib/zephlet/codegen/requirements.txt
```

### Create a zephlet

```bash
west zephlet new -n MyZephlet -d "Description" -a "Author"
```

Edit your `.proto` file to define Config/Events/RPCs, then run `just b` to bootstrap the `.c` file (auto-generated once if missing via `--impl-only`). After bootstrap, implement TODOs in the `.c` file manually (never overwritten). Add to root `CMakeLists.txt` `EXTRA_ZEPHYR_MODULES`, enable `CONFIG_ZEPHLET_<ZEPHLET>=y`, rebuild.

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

- `chan_<zephlet>_invoke`: Receives commands (START/STOP/CONFIG/etc)
- `chan_<zephlet>_report`: Publishes status/events

**Auto-discovery** via `STRUCT_SECTION_ITERABLE` for zero-config initialization.

**Protobuf-driven codegen** generates interface files from `.proto` definitions.

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
- `test_get_config` - Config query (commented if no config)
- `test_config` - Config update (commented if no config)

**Test structure:**

- `CMakeLists.txt` - Test target configuration
- `prj.conf` - Test project configuration
- `testcase.yaml` - Test metadata
- `test_*.c` - Test implementation

## Documentation

See [CLAUDE.md](CLAUDE.md) for detailed architecture and implementation guide.

## Example

Reference implementation: https://github.com/<org>/ports_adapters_zbus

## License

Apache-2.0
