# Zephlet Infrastructure Module

Reusable zephlet infrastructure for Zephyr RTOS implementing Ports & Adapters pattern via zbus.

## Quick Start

### Add to west.yml

```yaml
projects:
  - name: zephlet
    url: https://github.com/<org>/zephlet
    revision: main
    path: modules/lib/zephlet
```

### Install dependencies

```bash
pip install -r modules/lib/zephlet/codegen/requirements.txt
```

### Create a zephlet

```bash
west zephlet new -n MyZephlet -d "Description" -a "Author"
```

## Configuration

Set `ZEPHLET_ADAPTERS_DIR` if not using workspace/adapters/ convention:

```bash
west config zephlet.adapters-dir <path>
```

Default: `workspace_root/adapters/`

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

- `west zephlet new [-n NAME] [-d DESC] [-a AUTHOR]` - Create zephlet (interactive if no args)
- `west zephlet new-adapter [-o ORIGIN] [-d DEST] [-i]` - Create adapter (-i for interactive)
- `west zephlet gen ZEPHLET` - Regenerate interface files

## Documentation

See [CLAUDE.md](CLAUDE.md) for detailed architecture and implementation guide.

## Example

Reference implementation: https://github.com/<org>/ports_adapters_zbus

## License

Apache-2.0
