# Issue 004: Make the report channel optional

## Problem

Every zephlet currently defines two zbus channels: invoke and report. This is the right model for zephlets that both receive commands and publish state/events. But some zephlets don't fit neatly:

- **Command sink** (e.g., a GPIO driver zephlet): Only needs invoke. Never publishes reports or events. The report channel is dead weight — unused memory, an observer slot, generated report helpers that are never called.
- **Event source** (e.g., a sensor zephlet): Only needs report. Publishes readings periodically but doesn't accept commands beyond start/stop. The invoke channel could be simplified or removed.

Currently, both channels are always generated with their full protobuf structs, dispatcher code, and inline helpers regardless of whether they're used.

## Proposal

Allow the proto definition to declare which channels a zephlet needs:

```protobuf
service Tick {
  option (zephlet.channels) = INVOKE_AND_REPORT;  /* default */
}

service GpioOut {
  option (zephlet.channels) = INVOKE_ONLY;
}

service Sensor {
  option (zephlet.channels) = REPORT_ONLY;
}
```

The code generator would then:
- **INVOKE_ONLY:** Skip report channel, report helpers, report oneof, and context echo.
- **REPORT_ONLY:** Skip invoke channel, dispatcher, inline invoke helpers. Events are published directly.
- **INVOKE_AND_REPORT:** Current behavior (default).

## Trade-offs

- **Pro:** Lighter footprint for simple zephlets — fewer channels, less generated code, smaller structs.
- **Pro:** Communicates intent — reading the proto tells you if a zephlet is a command sink, event source, or both.
- **Con:** Three code paths in the generator instead of one.
- **Con:** Adapters need to handle the case where a zephlet has no report channel (can't observe it).

## Questions

- Is REPORT_ONLY practical? A pure event source still needs start/stop — does that imply an invoke channel?
- Could INVOKE_ONLY zephlets still return errors via the invoke channel (synchronous return code) instead of needing a report?
- Should this be a proto annotation or a Kconfig option?
