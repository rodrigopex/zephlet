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

    # Only needed with CONFIG_ZEPHLETS_SHELL=y — see Dependencies. Drop
    # this entry if you are not enabling the shell frontend.
    - name: zephyr-nanopb-textformat
      url: https://codeberg.org/rodrigopex/zephyr-nanopb-textformat
      revision: v0.4.0
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
- **Coordinators** (optional, `CONFIG_ZEPHLETS_COORD=y`): multi-step flows with workqueue dispatch + bounded zbus-event awaits. Sits above the per-zephlet command/events surface — see [Coordinators](#coordinators) below.
- **Frontends** (optional): expose the same command surface over a transport, with no change to domain code — each hooks itself in from `ZEPHLET_NEW` via a per-type macro. Shell (`CONFIG_ZEPHLETS_SHELL=y`, see [Shell frontend](#shell-frontend)) and CoAP (`CONFIG_ZEPHLETS_COAP=y`, per-service proto opt-in). Rationale in [ADR-0001](docs/adr/0001-zephlet-frontends.md).

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

## Shell frontend

`CONFIG_ZEPHLETS_SHELL=y` puts every instance's RPCs under one `zlet` root
command. No app code: `ZEPHLET_NEW(...)` already expands the per-type hook that
registers them. Needs the library in your manifest — see [Dependencies](#dependencies).

An RPC takes its request as a protobuf **text-format** message:

```
uart:~$ zlet <TAB>                                    # one entry per instance
uart:~$ zlet tick_fast config duration_ms: 100, period_ms: 10
duration_ms: 100
period_ms: 10
uart:~$ zlet tick_fast get_config
duration_ms: 100
period_ms: 10
```

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
| Submessage | `origin {x: 1}` on one line | an indented block |

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

## West commands

| Command | Purpose |
|---|---|
| `west zephlet new [-o <dir>] [-n -d -a] [--prefix STR] [--no-module]` | Copier scaffold. Destination = `$PWD` unless `-o`. `--prefix` overrides the default `zlet_` (`""` drops it); `--no-module` skips the tests folder and `zephyr/module.yml`. |
| `west zephlet new-adapter` | Prints the v0.3 recipe. No codegen. |
| `west zephlet gen <zephlet_dir>` | Regenerate `<prefix>_interface.{h,c}` from its proto. |

## Dependencies

- Zephyr RTOS (with `zbus`, `nanopb` modules).
- Python packages: `proto-schema-parser`, `jinja2`, `copier`.
- [`zephyr-nanopb-textformat`](https://codeberg.org/rodrigopex/zephyr-nanopb-textformat)
  — **only with `CONFIG_ZEPHLETS_SHELL=y`**, which `select`s it. The shell
  frontend takes each RPC's request as a protobuf text-format message and
  hands parsing and printing to this library, so every field shape works
  (`optional`, `repeated`, submessage, nested) without codegen walking
  fields.

  It must be in *your* west manifest: if it is absent the `select` has no
  symbol to resolve and the frontend has no `nanopb_textformat.h` to
  include. Nothing else in the infra needs it, so a project with the shell
  frontend off can leave it out entirely.

  **Pin a tag.** The API is pre-1.0 and has already moved: v0.3.0 dropped
  the trailing `flags` argument from `pb_tf_parse()` / `pb_tf_print()` in
  favour of Kconfig options, and split `PB_TF_NOINIT` out as
  `pb_tf_merge()`. Tested here against **v0.4.0**.

  Its `docs/shell-integration.md` covers the application-side settings a
  console needs — chiefly `CONFIG_CBPRINTF_FULL_INTEGRAL=y` for fields
  wider than 32 bits, and larger `CONFIG_SHELL_CMD_BUFF_SIZE` /
  `CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE` for a wide message,
  since naming every field makes text format verbose.

## Example app

Reference implementation: [ports_adapter_zbus](https://github.com/rodrigopex/ports_adapter_zbus).

## License

Apache-2.0
