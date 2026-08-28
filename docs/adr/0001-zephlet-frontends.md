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

## Addendum: shell's build-gating differs from the pattern above

The shell frontend (#53, [`../plans/shell-frontend.md`](../plans/shell-frontend.md))
follows every invariant above except one: it has **no per-service proto
opt-in** and **no aggregator-macro hook**. `ZLET_SHELL_INSTANCE(_type,
_instance)` is a macro the app author invokes explicitly, once per
instance — usually right after that instance's `ZEPHLET_NEW(...)` call —
not something wired into `zephlet.h`'s `_ZLET_FRONTEND_HOOKS_<type>`
aggregator the way `_ZLET_COAP_HOOK_<type>` is. `zephlet.h` and
`zephlet_interface.h.jinja`'s aggregator plumbing stay byte-unchanged by
the shell frontend.

This is a deliberate, narrower case, not a gap in the pattern:

- **Why no per-service opt-in.** CoAP's opt-in exists because a network
  frontend changes a zephlet's *exposure surface* — a proto author must
  consciously choose to put an RPC on the wire. Shell has no such
  question: `CONFIG_ZEPHLETS_SHELL=y` already means "local, privileged
  console access to this binary," a broader trust boundary than any
  individual zephlet's opt-in could narrow.
- **Why no aggregator hook, and therefore no disabled-build hash-gate
  requirement.** CoAP's aggregator hook exists so a *disabled* frontend
  costs nothing while still letting an *opted-in* zephlet's interface
  header `#include` the frontend's generated artifact unconditionally.
  Shell's `ZLET_SHELL_INSTANCE`/`ZLET_SHELL_DEFINE` calls live in
  application code (`main.c`), guarded the ordinary way — an app that
  never calls them, or wraps the calls in `#if
  defined(CONFIG_ZEPHLETS_SHELL)`, has zero shell-frontend code in its
  build. There is no "did a hook leak into the disabled build" risk
  class to gate against, because there is no hook.
- **The one thing that *is* unconditional**, matching CoAP's aggregator
  spirit: `<TYPE>_SHELL_METHODS`/`_ZLET_SHELL_METHODS_APPLY_<type>` in
  `<prefix>_interface.h` always render, opt-in or not (verified
  byte-identical across the CoAP opt-in toggle by
  `test_shell_methods_invariant_across_coap_opt_in`). They cost nothing
  unused: a `#define` table is not a function call, and nothing
  references it unless `ZLET_SHELL_INSTANCE` is invoked for that type.

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
| Shell | Implemented (v1) | [`../plans/shell-frontend.md`](../plans/shell-frontend.md) |
| gRPC | Deferred | — |
| MQTT | Deferred | — |
