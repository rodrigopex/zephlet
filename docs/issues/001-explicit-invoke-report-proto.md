# Issue 001: Make Invoke/Report explicit in zephlet protos

> **Superseded by v0.3** (see app repo `docs/REFACTOR_V3_PLAN.md`).
> The Invoke/Report oneof model is gone entirely. Each RPC has its own
> request/response message; the service block in the source `.proto`
> is authoritative.

## Problem

The developer writes a minimal `.proto` with only `Config`, `Events`, and a `service` block with `extends_base = true`. The build system then generates a completely different proto file that includes `Invoke` and `Report` messages with all base lifecycle fields merged in.

This means the full dispatch contract — what messages actually flow on zbus channels — is invisible in the source proto. To understand the API, you must read generated files in the build directory.

**What the developer writes:**
```protobuf
message Tick {
  message Config { uint32 delay_ms = 1; }
  message Events { int32 timestamp = 1; optional Empty tick = 2; }
}
service Tick { option (zephlet.extends_base) = true; }
```

**What the build produces (hidden from source):**
```protobuf
message Tick {
  message Config { ... }
  message Events { ... }
  message Invoke {
    optional ZephletContext context = 999;
    oneof tick_invoke_tag {
      Empty start = 1; Empty stop = 2; Empty get_status = 3;
      Tick.Config config = 4; Empty get_config = 5; Empty get_events = 6;
    }
  }
  message Report {
    optional ZephletContext context = 999;
    oneof tick_report_tag {
      Empty empty = 1; ZephletStatus status = 2;
      Tick.Config config = 3; Tick.Events events = 4;
    }
  }
}
```

## Proposal

Stop generating Invoke/Report from the base. Instead, the developer writes Invoke and Report explicitly in their `.proto` file. Base lifecycle fields become an importable pattern (helper macros or documented copy-paste), not implicit generation.

## Trade-offs

- **Pro:** What you see is what you get. The source proto is the single source of truth for the zbus contract.
- **Pro:** Easier to reason about custom invoke/report combinations (e.g., a zephlet that doesn't need `get_events`).
- **Con:** More boilerplate per proto file — every zephlet must list the standard lifecycle fields.
- **Con:** Risk of inconsistency if developers forget or mistype standard fields (mitigated by build-time validation).

## Questions

- Should the base fields still be validated at build time even if written explicitly?
- Could a proto `import` + nanopb extensions reduce the boilerplate while keeping things explicit?
