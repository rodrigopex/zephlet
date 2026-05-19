# ADR-0001 — Zephlet frontends

Status: Proposed
Date: 2026-05-18
Supersedes: —

## Context

A zephlet exposes its domain logic through a single uniform envelope
(`struct zephlet_call`) on a pair of zbus channels:

- `chan_<name>_command` — pointer channel, sync listener, one observer.
- `chan_<name>_events`  — value channel, async fan-out.

Today the only caller is **local C code** (other zephlets, app `main`,
adapters). We want to expose the same operations over the network or over
other transports (CoAP first; gRPC / MQTT / JSON-over-UART later) **without
touching any zephlet's domain logic, handler signatures, envelope shape, or
core codegen output for the local path**.

## Decision

Introduce **frontends** as a first-class concept in the zephlet infra.

A *frontend* is an additive transport edge that:

1. Translates an inbound transport request into a stack-local
   `struct zephlet_call`.
2. Publishes that envelope on the target instance's `chan_<name>_command`
   channel (sync listener mutates in place).
3. Translates the mutated envelope back into a transport response.
4. For events, attaches an async listener to `chan_<name>_events` and emits
   transport-specific notifications.

Frontends share these invariants:

- **Envelope-bound.** A frontend never reads zephlet `config`/`data`
  directly; everything goes through the envelope.
- **Per-frontend opt-in.** A zephlet is exposed over a given frontend only
  when its `.proto` declares an opt-in option at service level:
  `option (zephlet.coap) = true;`, `option (zephlet.grpc) = true;`, etc.
  Default is not-exposed. The option is read by the existing codegen
  string/regex pre-scan — `protoc` never sees it.
- **Build-gated.** Each frontend has its own Kconfig (e.g.
  `CONFIG_ZEPHLETS_COAP`, `CONFIG_ZEPHLETS_GRPC`) defaulting to `n`. With a
  frontend disabled, the build's stripped section hash (`.text`, `.rodata`,
  `.data`, `.bss`) is **identical** to a pre-frontend reference build. No
  symbol, observer, or section entry from the frontend may leak in.
- **Per-frontend codegen artifact.** When a zephlet opts in to frontend `F`,
  codegen emits a sibling artifact `<prefix>_<F>_interface.{h,c}` alongside
  the existing `<prefix>_interface.{h,c}`. The core `_interface.{h,c}` files
  gain only the **frontend-aggregator macro plumbing** (a per-type
  aggregator that lists known frontends' hooks, plus a default-empty hook
  for each); their semantic output for the local-path (handlers, wrappers,
  events emitter, readiness query) is **unchanged**. When no frontend is
  opted-in for a type, the aggregator expands to nothing and the build
  output is byte-equivalent at link time.
- **No per-call shared state in the frontend.** Each inbound request maps
  to a stack-local envelope; concurrency is bounded by the transport's
  own request handling, not by a frontend semaphore.
- **No proto schema on the device.** Frontends may expose *resource
  discovery* (e.g. `/.well-known/core` for CoAP, gRPC reflection) but
  shipping a binary `FileDescriptorSet` is per-frontend, behind its own
  Kconfig, and not v1 for any frontend.
- **Per-frontend runtime in its own subdir.** Frontend runtime code lives
  under `modules/lib/zephlet/frontends/<name>/`. The infra root stays the
  core zephlet contract.

## Consequences

- Multiple frontends compose: a single zephlet can opt in to CoAP **and** a
  future MQTT frontend; both go through the same envelope; neither knows
  about the other.
- The "RPC mapping shape" (`<type>/{instance}/{method}` segments, header
  carriage of errno, etc.) is **per-frontend** and documented in each
  frontend's own plan/ADR.
- The disabled-build hash gate must be wired into CI as a standing check;
  every frontend phase ends by re-asserting the gate.
- Adding a new frontend = (a) one Kconfig, (b) one opt-in proto option,
  (c) one codegen branch, (d) one runtime module — no zephlet rewrite.

## Alternatives considered

- **Rewrite each zephlet for each transport.** Rejected: explodes
  maintenance, breaks the single-envelope invariant, makes domain logic
  transport-aware.
- **Single "bridge zephlet" per transport.** Rejected: every other zephlet
  would need to publish/subscribe in the bridge's terms; defeats the goal
  of leaving domain logic untouched.
- **Hand-written transport stubs (no codegen).** Rejected: duplicates
  schema knowledge already in the per-zephlet proto and drifts as protos
  evolve.

## Open frontend designs

| Frontend | Status | Plan |
|---|---|---|
| CoAP | Proposed (v1) | [`../plans/coap-frontend.md`](../plans/coap-frontend.md) |
| gRPC | Deferred | — |
| MQTT | Deferred | — |
