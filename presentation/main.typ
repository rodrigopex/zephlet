// NOTE: This presentation describes the pre-v0.3 zephlet architecture
// (Invoke/Report oneof + correlation IDs). It has not been updated for
// v0.3. See the current CLAUDE.md / README.md for the up-to-date model.

#import "@preview/typslides:1.2.5": *
#import "@preview/codly:1.2.0": *
#import "@preview/fletcher:0.5.8": diagram, node, edge

// Configure code highlighting
#show: codly-init.with()
#codly(
  languages: (
    c: (name: "C", icon: none, color: rgb("#555555")),
    proto: (name: "Protobuf", icon: none, color: rgb("#4285f4")),
  )
)

// Project configuration
#show: typslides.with(
  ratio: "16-9",
  theme: "dusky",
)

// The front slide is the first slide of your presentation
#front-slide(
  title: "Zephlet Framework",
  subtitle: [Ports & Adapters Architecture on Zephyr RTOS],
  authors: "Rodrigo Peixoto",
  //info: [#link("https://github.com/rodrigopex/zephlet")],
)

// Custom outline
#table-of-contents()

// ============================================================================
// 1. INTRODUCTION
// ============================================================================

#title-slide[Introduction]

#slide(title: "What is Zephlet?")[

A framework implementing *Ports & Adapters* architecture on Zephyr RTOS:

- *Zephlets:* Domain logic components with zero direct dependencies
- *Adapters:* Compose zephlets via channel bridging (zbus)
- *Two-channel pattern:* Clean separation of commands and events
- *Code generation:* Proto definitions → complete implementation scaffold

*Built on Zephyr's zbus* for loose coupling and composability.

]

#slide(title: "Key Benefits")[

- *Loose coupling:* Components only know about channels, not each other
- *Type safety:* Protobuf definitions enforce contracts
- *Composability:* Adapters wire zephlets together without modifying them
- *Auto-discovery:* `STRUCT_SECTION_ITERABLE` finds all zephlets at runtime
- *Thread safety:* Built-in spinlocks for state protection
- *Testability:* Pure domain logic, easy to mock channels

]

// ============================================================================
// 2. ARCHITECTURE
// ============================================================================

#title-slide[Architecture]

#slide(title: "Two-Channel Pattern")[

Each zephlet exposes exactly two zbus channels:

```c
ZBUS_CHAN_DEFINE(chan_tick_invoke, /* ... */);  // Commands IN
ZBUS_CHAN_DEFINE(chan_tick_report, /* ... */);  // Events OUT
```

*Invoke channel:* Receives commands (start, stop, config, custom RPCs)

*Report channel:* Publishes status, events, responses

#v(1em)

#align(center)[
  #diagram(
    spacing: (3cm, 2cm),
    node-stroke: 1pt,
    edge-stroke: 1pt,
    node((0,0), [*Caller*], shape: rect, width: 2cm, height: 1cm),
    node((2,0), [*Zephlet*], shape: rect, width: 2cm, height: 1cm),
    node((4,0), [*Observer*], shape: rect, width: 2cm, height: 1cm),

    edge((0,0), (2,0), "->", [invoke], label-pos: 0.5),
    edge((2,0), (0,0), "->", [status], label-pos: 0.5, bend: 20deg),
    edge((2,0), (4,0), "-->", [events (async)], label-pos: 0.5, stroke: (dash: "dashed")),
  )
]

#v(0.5em)

*No direct dependencies* - only zbus channels connect components.

]

#slide(title: "Request-Response with Context")[

Commands can request responses using correlation:

```c
/* Inline helper generates correlation_id */
tick_start(correlation_id, K_MSEC(100));

/* API implementation receives context */
static int start(const struct zephlet *zephlet,
                 const struct msg_api_context *context) {
    /* ... do work ... */
    return tick_report_status(context, ret, &status, K_NO_WAIT);
}
```

`MsgAPIContext {correlation_id, return_code, has_context}`

Observers check `has_context` to distinguish responses from async events.

]

#slide(title: "Async Events")[

Events published without correlation (fire-and-forget):

```c
void timer_handler(struct k_timer *timer) {
    struct msg_tick_events events = {
        .timestamp = k_uptime_get(),
        .has_tick = true
    };

    /* No context - async event */
    tick_report_events_async(&events, K_NO_WAIT);
}
```

Adapters see `has_context == false` for async events.

]

// ============================================================================
// 3. LIFECYCLE
// ============================================================================

#title-slide[Lifecycle]

#slide(title: "Zephlet Lifecycle States")[

Two orthogonal flags in `MsgZephletStatus`:

- *`is_ready`:* Set by init (SYS_INIT), never changes
  - Indicates zephlet resources initialized successfully

- *`is_running`:* Controlled by start/stop commands
  - Can be started/stopped multiple times
  - Check before processing commands

```c
static int start(const struct zephlet *zephlet,
                 const struct msg_api_context *context) {
    K_SPINLOCK(&data->lock) {
        if (!data->status.is_ready) return -ENODEV;
        if (data->status.is_running) return -EALREADY;
        data->status.is_running = true;
    }
    /* ... */
}
```

]

#slide(title: "Standard Lifecycle Commands")[

All zephlets support these commands (proto reserved fields 1-6):

```proto
message Invoke {
    optional MsgAPIContext context = 999;
    oneof invoke {
        Empty start = 1;
        Empty stop = 2;
        Empty get_status = 3;
        Config config = 4;
        Empty get_config = 5;
        Empty get_events = 6;
        // Custom commands at 7+
    }
}
```

*Reserved ranges enforced at build time* - validation fails on conflicts.

]

// ============================================================================
// 4. STRUCTURE
// ============================================================================

#title-slide[File Structure]

#slide(title: "Source vs Generated Files")[

*Version-controlled (write once):*
- `zlet_tick.proto` - Data contracts and RPC definitions
- `zlet_tick.c` - Business logic implementation
- `CMakeLists.txt`, `Kconfig`, `module.yml`

*Generated (auto-regen on proto change):*
- `zlet_tick_interface.h` - Data structs, API, inline helpers
- `zlet_tick_interface.c` - Channels, dispatcher, registration
- `zlet_tick.h` - Report helper functions
- `zlet_tick.pb.h/.pb.c` - nanopb serialization

*Bootstrap:* First build auto-generates `.c` via `--impl-only` if missing.

]

#slide(title: "Protobuf Structure")[

```proto
message MsgZletTick {
    message Config { uint32 delay_ms = 1; }
    message Events { int32 timestamp = 1; optional Empty tick = 2; }

    message Invoke {
        optional MsgAPIContext context = 999;
        oneof tick_invoke {
            Empty start = 1; Empty stop = 2;
            Empty get_status = 3; Config config = 4;
            Empty get_config = 5; Empty get_events = 6;
        }
    }

    message Report {
        optional MsgAPIContext context = 999;
        oneof tick_report {
            MsgZephletStatus status = 1;
            Config config = 2; Events events = 3;
        }
    }
}
```

]

#slide(title: "Service Definitions (RPCs)")[

RPC return types validate against Report fields:

```proto
service Tick {
    rpc start(Empty) returns (MsgZephletStatus);
    rpc stop(Empty) returns (MsgZephletStatus);
    rpc get_status(Empty) returns (MsgZephletStatus);
    rpc config(Config) returns (Config);
    rpc get_config(Empty) returns (Config);
    rpc get_events(Empty) returns (stream Events);
}
```

Generator validates: `returns Config` → `report_config()` helper exists.

]

// ============================================================================
// 5. CODE GENERATION
// ============================================================================

#title-slide[Code Generation]

#slide(title: "Creating a New Zephlet")[

```bash
# Interactive mode
$ west zephlet new
? Name: sensor
? Description: Temperature sensor interface
? Author: Your Name

# Non-interactive
$ west zephlet new -n sensor -d "Temp sensor" -a "Your Name"
```

Copier template creates:
- `zlet_sensor.proto` with standard lifecycle
- `CMakeLists.txt` with `zephyr_zephlet_generate()`
- `Kconfig` with CONFIG_ZEPHLET_SENSOR
- `module.yml` for west integration

*First build* auto-generates `zlet_sensor.c` template with TODOs.

]

#slide(title: "Development Workflow")[

1. Edit `.proto` - add Config fields, Events, custom RPCs
2. `just b` - auto-regenerates interface files
3. Edit `.c` - implement TODOs (API functions)
4. Add to `CMakeLists.txt` EXTRA_ZEPHYR_MODULES
5. Enable `CONFIG_ZEPHLET_SENSOR=y`
6. Rebuild and test

*Proto changes → auto-regen* (never overwrites `.c` after bootstrap)

*Manual regen:* `west zephlet gen sensor` (needs build dir first)

]

#slide(title: "Generated API Structure")[

Interface header provides:

#text(size: 10pt)[
```c
/* State with spinlock */
struct sensor_data {
    K_SPINLOCK_DEFINE(lock);
    struct msg_zephlet_status status;
    struct msg_sensor_config config;
    /* ... */
};

/* API function pointers (context-aware) */
struct sensor_api {
    int (*start)(const struct zephlet*,
                 const struct msg_api_context*);
    /* ... */
};

/* Inline helpers for invoking */
int sensor_start(uint32_t correlation_id, k_timeout_t timeout);
```
]

]

// ============================================================================
// 6. ADAPTERS
// ============================================================================

#title-slide[Adapters]

#slide(title: "Composition via Bridging")[

Adapters listen to one zephlet's reports, invoke another:

#text(size: 10pt)[
```c
#include "zlet_tick_interface.h"
#include "zlet_ui_interface.h"

static void tick_to_ui_adapter(const struct zbus_channel *chan,
                                const void *msg) {
    const struct msg_tick_report *report = zbus_chan_const_msg(chan);

    if (report->which_tick_report == MSG_TICK_REPORT_EVENTS_TAG) {
        if (report->events.has_tick) {
            /* Start new request chain */
            ui_blink(0, K_NO_WAIT);
        }
    }
}

ZBUS_ASYNC_LISTENER_DEFINE(lis_tick_to_ui_adapter, tick_to_ui_adapter);
ZBUS_CHAN_ADD_OBS(chan_tick_report, lis_tick_to_ui_adapter, 3);
```
]

]

#slide(title: "Creating Adapters")[

```bash
# Interactive - select fields to handle
$ west zephlet new-adapter -i
? Origin zephlet: tick
? Destination zephlet: ui
? Select tick report fields: [x] events [ ] status

# Non-interactive - generates all fields
$ west zephlet new-adapter -o tick -d ui
```

Auto-generates:
- `Tick+Ui_zlet_adapter.c` with TODOs
- Kconfig entry `CONFIG_TICK_TO_UI_ADAPTER`
- CMakeLists.txt entry
- Interface includes (no .pb.h)

]

#slide(title: "Context-Aware Adapters")[

Adapters check `has_context` to distinguish responses from events:

```c
switch (tick_report->which_tick_report) {
case MSG_TICK_REPORT_STATUS_TAG:
    if (tick_report->has_context) {
        /* Response to someone's request */
        uint32_t corr_id = tick_report->context.correlation_id;
        int ret_code = tick_report->context.return_code;
        if (ret_code < 0) {
            LOG_WRN("tick failed: %d", ret_code);
        }
    } else {
        /* Async status event */
        LOG_DBG("Async status update");
    }
    break;
}
```

]

// ============================================================================
// 7. DEMO EXAMPLE
// ============================================================================

#title-slide[Live Example]

#slide(title: "Tick Zephlet Implementation")[

#text(size: 10pt)[
```c
static int start(const struct zephlet *zephlet,
                 const struct msg_api_context *context) {
    struct tick_data *data = zephlet->data;
    struct msg_zephlet_status status;
    int ret = 0, delay;

    K_SPINLOCK(&data->lock) {
        if (!data->status.is_ready) ret = -ENODEV;
        else if (data->status.is_running) ret = -EALREADY;
        else {
            delay = data->config.delay_ms;
            data->status.is_running = true;
        }
        status = data->status;
    }

    if (ret == 0) {
        k_timer_start(&timer_tick, K_MSEC(delay), K_MSEC(delay));
    }

    return tick_report_status(context, ret, &status, K_MSEC(250));
}
```
]

]

#slide(title: "Timer Handler (Async Events)")[

```c
void timer_handler(struct k_timer *timer_id) {
    struct msg_tick_events events = {
        .timestamp = k_uptime_get(),
        .has_tick = true
    };

    /* Async event - no correlation */
    tick_report_events_async(&events, K_NO_WAIT);
}

K_TIMER_DEFINE(timer_tick, timer_handler, NULL);
```

No context → adapters see this as async event, not a response.

]

#slide(title: "Zephlet Registration")[

```c
static struct tick_api api = {
    .start = start,
    .stop = stop,
    .get_status = get_status,
    .config = config,
    .get_config = get_config,
    .get_events = get_events,
};

static struct tick_data data = {
    .config = MSG_TICK_CONFIG_INIT_ZERO,
    .status = MSG_ZEPHLET_STATUS_INIT_ZERO,
};

ZEPHLET_DEFINE(tick, tick_init_fn, &api, &data);
```

`STRUCT_SECTION_ITERABLE` enables runtime discovery.

]

// ============================================================================
// 8. CONCLUSION
// ============================================================================

#title-slide[Summary]

#slide(title: "Key Takeaways")[

- *Ports & Adapters* on Zephyr RTOS via zbus channels
- *Two-channel pattern* (invoke/report) ensures loose coupling
- *Context-aware* request-response with correlation tracking
- *Code generation* from proto → complete scaffold
- *West integration* (`zephlet new`, `new-adapter`, `gen`)
- *Type-safe* composition without direct dependencies
- *Thread-safe* by design (spinlocks + zbus async)

*Framework enforces best practices through structure.*

]

#slide(title: "When to Use Zephlet")[

*Good fit:*
- Component-based embedded systems
- Need for loose coupling and testability
- Multiple subsystems that must compose
- Want type-safe interfaces with protobuf

*Not ideal for:*
- Single-file applications
- Hard real-time with strict latency requirements (zbus overhead)
- Memory-constrained devices (less than 32KB RAM)

]

#slide(title: "Resources & Next Steps")[

*Documentation:*
- Framework docs: `modules/lib/zephlet/CLAUDE.md`
- Example app: `ports_adapters_zbus/`

*Try it:*
```bash
west zephlet new -n mysensor -d "My sensor" -a "Me"
# Edit zlet_mysensor.proto
just b  # First build auto-generates .c
# Implement TODOs in zlet_mysensor.c
```

*Questions?*

]

#slide(title: "Questions & Answers")[]
