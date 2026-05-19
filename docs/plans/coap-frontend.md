# CoAP Frontend — Adoption Plan

Status: Proposed
Date: 2026-05-18
Anchor: [`../adr/0001-zephlet-frontends.md`](../adr/0001-zephlet-frontends.md)

## Scope

A **CoAP frontend** that exposes opted-in zephlets over UDP (DTLS optional)
without changing any zephlet's domain logic, handler signature, envelope, or
core codegen output. The frontend translates CoAP requests into
`struct zephlet_call`, publishes on `chan_<name>_command` (sync listener
mutates in place), and returns the translated result. Events bridge to CoAP
Observe via an async listener on `chan_<name>_events`.

## Locked Decisions

These are settled. Do not relitigate during execution.

- **RPC mapping:** `POST /zlet/{type}/{instance}/{method}`. `{type}` is the
  zephlet's snake-cased type name (`tick`, `ui`); `{instance}` is the
  section-iterable `name` (e.g. `tick_fast`); `{method}` is the
  proto-declared method name in snake_case. URI parsing is **runtime, in
  the catch-all dispatcher** (one resource handles all `/zlet/*`). The
  `{type}` segment is enforced as a *defensive* check: after resolving
  `z = zephlet_get_by_name({instance})`, the dispatcher requires
  `z->api == <matching type record>->api`; mismatch → `4.04`. So a request
  to `/zlet/ui/tick_fast/start` is rejected because the `ui` type record's
  `api` pointer does not match `tick_fast`'s `z->api`.
- **Payloads:** request/response bodies are nanopb-encoded messages, using
  the **same descriptors** emitted by nanopb in the per-zephlet
  `<prefix>.pb.h` (the same symbols `_interface.c` already references).
  No second source of schema truth.
- **Content-Format:** fixed custom ID **65001** (experimental range,
  RFC 7252 §12.3). `application/octet-stream` (42) is accepted as fallback
  on request; responses always use 65001. The constant lives in one header
  alongside the base path and `rt=` strings.
- **Result mapping:** `return_code == 0` → `2.04 Changed` (or `2.05 Content`
  when a non-empty response payload exists). Negative errno →
  `-EINVAL` → `4.00`, `-ENODEV` → `4.04`, `-EALREADY` → `4.09`,
  `-ENOSYS` → `4.05`, `-EBUSY`/`-EAGAIN` → `5.03`, `-ETIMEDOUT` → `5.04`,
  `-ENOMEM` → `5.00`. The **raw POSIX errno is always carried verbatim** in
  a custom CoAP option (number `65003`, see Q3-resolved note below), so the
  exact value survives the lossy class mapping.
- **Events:** CoAP Observe (RFC 7641) on `GET /zlet/{type}/{instance}/events`.
  v1 emits **NON** notifications only — CON support (per-event or service-level
  override) is deferred. Sequence number increments only on **successful send**,
  never on drop (RFC 7641 §3.4 — no desync).
- **Event queueing & backpressure:** one **`ZEPHLET_EVENTS_LISTENER` per
  opted-in zephlet *instance*** owns its own queue (the macro expands per
  `_name`, creating an independent `ZBUS_ASYNC_LISTENER_DEFINE`). Each
  instance's events run on their own async listener (zbus's per-listener
  message queue, configured via Kconfig), so a slow consumer of
  `tick_fast` cannot stall events for `tick_slow` or any other instance.
  The listener callback runs from the system workqueue, walks the global
  observer list filtered by `coap_observer.user_data == z`, and emits one
  NON per matching observer. Drops happen at **zbus enqueue time** when
  the listener's workqueue is behind the publisher — the publisher never
  blocks. Send-side errors (socket buffer full, no route) also count as
  drops.
- **Per-instance CoAP state.** Each opted-in instance has a small,
  compile-time `struct zephlet_coap_instance_state` containing
  `{ const struct zephlet *z; atomic_t dropped_total; atomic_t active_observers;
     uint32_t observe_seq; }`. These records live in a parallel section
  iterable `STRUCT_SECTION_ITERABLE(zephlet_coap_instance_state, ...)`
  populated by the per-instance `_ZLET_COAP_HOOK_<type>(_name)` macro
  expansion. The dispatcher and event callback look up an instance's
  state by walking that section and matching `s->z == z`.
- **Drop counter:** **per-instance aggregate** — single `atomic_t
  dropped_total` in the instance's state record, covering both
  enqueue-drops and send-failure-drops. Exposed two ways: (a) `LOG_WRN` on
  drop (with type+instance name), (b) `GET
  /zlet/{type}/{instance}/events/stats` returns a small nanopb payload
  (`CoapEventsStats { uint32 dropped_total; uint32 active_observers; }`,
  defined in `frontends/coap/proto/zlet_coap_stats.proto`). Drops are **not**
  surfaced in the Observe stream itself — observers who care must poll the
  stats resource.
- **Transport:** Zephyr `coap_service` + `zephyr/net/coap.h` over UDP. DTLS
  optional under `CONFIG_ZEPHLETS_COAP_DTLS` (default n, depends on the
  CoAP Kconfig). Not required for v1.
- **Build gating:** `CONFIG_ZEPHLETS_COAP=n` produces a build whose
  **stripped section hash** (sha256 over `.text`/`.rodata`/`.data`/`.bss`
  after `objcopy --strip-debug --remove-section=.note*`) is **identical**
  to a pre-CoAP reference build. No CoAP symbol, channel observer, or
  iterable section entry may leak into the disabled build. Hard acceptance
  criterion, tested in CI as a standing gate.
- **Codegen gate:** a zephlet is exposed over CoAP only when its proto opts
  in via `option (zephlet.coap) = true;` at service level. Default is
  not-exposed. The opt-in is detected by a regex pre-scan in
  `generate_zephlet.py`; `protoc` never sees the custom option.
- **Discovery:** `/.well-known/core` (RFC 6690) advertises opted-in
  resources with `rt=` attributes:
  `rt="zlet.rpc"` for each RPC resource,
  `rt="zlet.event";obs` for each events resource,
  `rt="zlet.stats"` for each per-zephlet stats resource.
  `ct=65001` on RPC and event resources. No proto schema is carried.
  Binary `FileDescriptorSet` discovery is out of scope for v1.
- **No per-call shared state.** Each CoAP request maps to a stack-local
  envelope, exactly like a local caller. Concurrency is bounded by
  `coap_service`'s request handling, not by a frontend semaphore.
- **Wiring is proto-only and compile-time.** Removing
  `option (zephlet.coap) = true;` from a zephlet's `.proto` MUST be
  sufficient to unexpose it — no change to `ZEPHLET_NEW` call sites, no
  extra macros at the user site, no CMake edits in the app. There are
  **no runtime `zbus_chan_add_obs` calls**; the events listener is
  declared statically via the existing `ZEPHLET_EVENTS_LISTENER` macro.
  Mechanism (generalized to support future frontends without touching
  `zephlet.h` again):

  1. `zephlet.h` adds **one line** to `ZEPHLET_NEW_PRIO`:
     `_ZLET_FRONTEND_HOOKS_##_type(_name);`. This is the only frontend-
     aware line in core infra; it never changes when frontends are added.
  2. Codegen's `<prefix>_interface.h` (always emitted, per-type) defines
     a frontend-aggregator macro listing every known frontend's per-type
     hook, plus a default-empty for each:
     ```c
     /* Aggregator: chains all known frontends' per-type hooks. */
     #define _ZLET_FRONTEND_HOOKS_tick(_name)  \
         _ZLET_COAP_HOOK_tick(_name)           \
         /* future frontends chain here */

     /* Default: each frontend's hook is empty unless overridden. */
     #ifndef _ZLET_COAP_HOOK_tick
     #define _ZLET_COAP_HOOK_tick(_name) /* nothing */
     #endif
     ```
  3. When the type has CoAP opt-in, codegen also emits
     `<prefix>_coap_interface.h` (included by `<prefix>_interface.h`)
     which overrides the default:
     ```c
     #ifdef CONFIG_ZEPHLETS_COAP
     #undef  _ZLET_COAP_HOOK_tick
     #define _ZLET_COAP_HOOK_tick(_name) \
         ZEPHLET_EVENTS_LISTENER(_name, tick, _tick_coap_event_cb)
     #endif
     ```
     Macro expansion is lazy, so the aggregator at the user's `ZEPHLET_NEW`
     call site picks up the overridden definition.
  4. Per-opted-in-type codegen also emits `<prefix>_coap_interface.c`
     containing the type record
     (`STRUCT_SECTION_ITERABLE(zephlet_coap_type, ...)`) and the event
     callback `_<type>_coap_event_cb`. Compiled only when
     `CONFIG_ZEPHLETS_COAP=y`.

  **Effect of removing the proto option:** codegen does not emit
  `<prefix>_coap_interface.{h,c}`; the default empty hook stays in force;
  no `ZEPHLET_EVENTS_LISTENER` is expanded for that type's instances; no
  `zephlet_coap_type` record exists; the catch-all dispatcher returns
  `4.04` for that type's URIs. Zero footprint elsewhere.

  **Effect of `CONFIG_ZEPHLETS_COAP=n`:** the frontend runtime is not
  compiled, the catch-all resource is not registered, and the per-type
  override `#ifdef CONFIG_ZEPHLETS_COAP` branch evaluates to the empty
  hook. Section-hash identity vs the pre-CoAP build holds.

  **Future frontends (MQTT, gRPC, …)** plug in by (a) extending the
  per-type aggregator in `<prefix>_interface.h` with their own
  `_ZLET_<NAME>_HOOK_<type>(_name)` slot, and (b) emitting an
  override-providing header when their proto opt-in is present. No edit
  to `zephlet.h` or `ZEPHLET_NEW_PRIO` is needed.
- **CoAP-observer-side registration is dynamic** (RFC 7641 has no static
  alternative for client subscriptions). Observers are added to Zephyr's
  global observer list via `coap_register_observer`, with
  `coap_observer.user_data` storing the `const struct zephlet *` so the
  per-instance event callback can filter the global list and send NON
  only to that instance's subscribers.
- **One catch-all CoAP resource** at path `["zlet", "#", NULL]`
  (multi-level wildcard, requires `CONFIG_COAP_URI_WILDCARD=y` which the
  CoAP Kconfig selects). The handler parses URI options into
  `(type, instance, method)` at request time and dispatches via
  `zephlet_get_by_name` + section-iterable lookup of the matching
  `zephlet_coap_type` record.
- **One header owns all CoAP constants** at
  `frontends/coap/include/zephlet_coap_consts.h`:
  Content-Format ID `65001`, errno option number `65003`, base path
  `/zlet`, events suffix `events`, stats suffix `events/stats`,
  `rt=` strings (`zlet.rpc`, `zlet.event`, `zlet.stats`). No magic literals
  scattered across templates.

## Resolved-at-planning-time questions

- **Q1 — per-resource user-data hook:** *Superseded by the
  one-catch-all-resource + per-instance state-record design.*
  Per-instance association is recovered three ways: (a) at dispatch time,
  by parsing the URI and looking up the instance via `zephlet_get_by_name`
  + `STRUCT_SECTION_FOREACH(zephlet_coap_type, ...)`; (b) for Observe
  subscribers, by storing `const struct zephlet *` in
  `coap_observer.user_data` (`coap.h:354`); (c) for counters/sequence
  numbers, by `STRUCT_SECTION_FOREACH(zephlet_coap_instance_state, ...)`
  matching on `s->z == z`. See the wiring locked-decision above for the
  authoritative description. *(Closed.)*
- **Q2 — Observe sequence vs. drop-on-backpressure:** RFC 7641 §3.4 says the
  Observe value (sequence number) is set by the *sender* at send time.
  Incrementing only on successful send keeps the per-observer sequence
  monotonic and gap-free from the observer's perspective. *(Closed.)*
- **Q3 — Content-Format / option numbers:** Content-Format **65001** for
  nanopb-encoded zephlet payloads; custom CoAP option **65003** (Critical=0,
  Unsafe=0, NoCacheKey=1, Repeatable=0; numeric, 4 bytes) carrying the raw
  errno as a signed 32-bit value. Both pinned in `frontends/coap/include/
  zephlet_coap_consts.h`. *(Closed.)*

## Phases

Each phase is independently buildable, ends green for both `=y` and `=n`,
and re-asserts the section-hash gate.

### Phase 0 — Skeleton + build gate + frontend-hook in core

- Add `CONFIG_ZEPHLETS_COAP` (default n; `select COAP_URI_WILDCARD` and
  `select NET_SOCKETS` etc.) and `CONFIG_ZEPHLETS_COAP_DTLS` (default n,
  depends on the first) under `modules/lib/zephlet/Kconfig`.
- Extend `ZEPHLET_NEW_PRIO` in `zephlet.h` by **one line**:
  `_ZLET_FRONTEND_HOOKS_##_type(_name);`. This is the only frontend-aware
  change to core infra and is permanent.
- Create `modules/lib/zephlet/frontends/coap/` with CMake wiring so `=y`
  compiles an empty TU, `=n` compiles nothing.
- Add `frontends/coap/include/zephlet_coap_consts.h` with the pinned
  constants (Content-Format `65001`, errno option `65003`, base path
  `/zlet`, suffixes, `rt=` strings).
- Land the section-hash baseline test under
  `modules/lib/zephlet/tests/coap_buildhash/` and wire it into CI as a
  standing gate. Baseline = build of the existing `ports_adapter_zbus`
  reference app at the commit *immediately before* this phase lands, with
  `CONFIG_ZEPHLETS_COAP=n`. Hash function:
  `sha256(objcopy --only-section .text --only-section .rodata
           --only-section .data --only-section .bss in.elf out.bin && cat out.bin)`.

**Acceptance:** disabled-build section hash recorded and reproducible; `=y`
builds and links with no functional code; the existing `_interface.{h,c}`
output for every zephlet is bit-unchanged; the `ZEPHLET_NEW_PRIO` hook
addition resolves to nothing when no frontend is opted-in (verified by
the section-hash gate itself).

### Phase 1 — Codegen: opt-in detection + frontend-hook plumbing

- Extend `generate_zephlet.py`'s pre-scan to detect
  `option (zephlet.coap) = true;` at service level (regex on the raw
  proto text — `proto_schema_parser` AST is not consulted for this).
- For **every** type (opted-in or not), add to the existing
  `<prefix>_interface.h`:
  - The aggregator macro
    `_ZLET_FRONTEND_HOOKS_<type>(_name)` listing each known frontend's
    per-type hook (v1 chains only `_ZLET_COAP_HOOK_<type>(_name)`; the
    chain is the extension point for future frontends).
  - A default-empty `_ZLET_COAP_HOOK_<type>(_name)` guarded by `#ifndef`.
- For opted-in types only, emit `<prefix>_coap_interface.{h,c}` into
  `${CMAKE_BINARY_DIR}/modules/<prefix>/`:
  - `<prefix>_coap_interface.h`: under `#ifdef CONFIG_ZEPHLETS_COAP`,
    `#undef` + redefine `_ZLET_COAP_HOOK_<type>(_name)` to expand to
    `ZEPHLET_EVENTS_LISTENER(_name, <type>, _<type>_coap_event_cb);`.
    Declares `_<type>_coap_event_cb` and the type's CoAP method table
    type.
  - `<prefix>_coap_interface.c`: defines the per-method descriptor table,
    the `STRUCT_SECTION_ITERABLE(zephlet_coap_type, <type>_coap_type)`
    record `{ type_name, &<type>_api, methods[], num_methods }`, and the
    `_<type>_coap_event_cb` body (walks Zephyr's global observer list,
    filters by `obs->user_data == z`, sends NON via Phase 2 encoder).
- Include order: `<prefix>_interface.h` `#include`s the coap header iff
  the type opted in (codegen-time decision; no `__has_include` runtime
  trick). When `CONFIG_ZEPHLETS_COAP=n`, the coap header's `#ifdef`
  branch is dead and the hook stays empty.
- CMake gate: `<prefix>_coap_interface.c` is added to the library's
  sources only when `CONFIG_ZEPHLETS_COAP=y` AND the type opted in.

**Acceptance:** an opted-in type produces the new `_coap_interface.{h,c}`
plus the aggregator + hook in `_interface.h`; a non-opted-in type
produces **only** the aggregator + empty hook (no `_coap_interface.*`);
the existing `_interface.c` output is bit-unchanged for every type
(regression diff against a saved baseline); with `CONFIG_ZEPHLETS_COAP=n`,
section-hash identity holds.

### Phase 2 — Envelope ↔ CoAP payload translation (pure, no network)

- Implement two transport-free functions in `frontends/coap/`:
  `zephlet_coap_decode_request(buf, len, method_entry, call_out)` →
  populates a stack-local `struct zephlet_call` from a CoAP request body;
  `zephlet_coap_encode_response(call, packet_out)` → packs the post-dispatch
  envelope's `return_code` + response payload into a CoAP response, with
  the verbatim-errno option attached.
- Implement the `return_code` → CoAP class mapping table.
- Unit tests with hand-built buffers: success+payload, success+no-payload,
  every mapped errno, malformed body → `4.00 Bad Request`, unknown method
  on a known path → `4.05 Method Not Allowed`, unknown path →
  `4.04 Not Found`, oversized body → `4.13 Request Entity Too Large`.

**Acceptance:** unit tests green; the two functions have **zero dependency
on `coap_service` or zbus** (pure, testable in isolation).

### Phase 3 — RPC dispatch (network, unary)

- In `frontends/coap/`, define one `COAP_SERVICE_DEFINE(zlet_coap_service,
  ...)` and a **single catch-all** resource:
  `COAP_RESOURCE_DEFINE(zlet_root, zlet_coap_service, { .path =
   (const char *const[]){ "zlet", "#", NULL }, .post = zlet_coap_rpc_post,
   .get = zlet_coap_get });`.

- **Dispatch tree (catch-all handler decides by method + segment shape):**

  | Method | Path shape                                 | Action                                  |
  |--------|--------------------------------------------|-----------------------------------------|
  | POST   | `/zlet/{type}/{instance}/{method}` (4 segs)| RPC dispatch (this phase)               |
  | POST   | anything else                              | `4.05 Method Not Allowed`               |
  | GET    | `/zlet/{type}/{instance}/events` (4 segs)  | Observe register/refresh (Phase 5)      |
  | GET    | `/zlet/{type}/{instance}/events/stats` (5) | stats query (Phase 5)                   |
  | GET    | anything else                              | `4.05 Method Not Allowed`               |
  | other  | any                                        | `4.05 Method Not Allowed`               |

- Path resolution (shared by all branches):
  - Walk `COAP_OPTION_URI_PATH` options to recover the segments
    `(type, instance, …)` as strings (stack buffers, sized by a Kconfig'd
    max segment length, default 32). Any segment longer than the buffer →
    `4.04 Not Found` with a `LOG_WRN`.
  - `z = zephlet_get_by_name(instance)` → NULL → `4.04`.
  - `STRUCT_SECTION_FOREACH(zephlet_coap_type, t)` finds the matching
    type record by `strcmp(t->type_name, parsed_type) == 0`. NULL → `4.04`.
  - Defensive check: `z->api != t->api` → `4.04` (type/instance mismatch,
    e.g. URI says `tick` but the instance is a `ui`).

- `zlet_coap_rpc_post` (RPC branch only):
  - Find method by `strcmp(m->path_segment, parsed_method) == 0` in
    `t->methods[0..t->num_methods)`. NULL → `4.05 Method Not Allowed`.
  - Phase 2 decode → stack-local `struct zephlet_call` →
    `zbus_chan_pub(z->channel.command, &p, K_FOREVER)` → Phase 2 encode
    → send.

- Integration test against `tick`: unary RPC round-trip plus unhappy paths
  (unknown type, unknown instance, type/instance mismatch, unknown method,
  malformed body, oversized segment) all return correct codes.

**Acceptance:** end-to-end unary RPC works against `tick` over CoAP; all
unhappy paths green; Phase 0 section-hash gate still holds with `=n`.

### Phase 4 — Discovery (`/.well-known/core`)

- Because the runtime registers one catch-all resource at `/zlet/#`,
  Zephyr's built-in `coap_well_known_core_get_len` would emit a single
  wildcarded link — useless to clients. Instead, register a **dedicated
  `COAP_RESOURCE_DEFINE` for `[".well-known", "core", NULL]`** in
  `frontends/coap/` with a custom GET handler.
- The custom handler enumerates per-instance virtual paths at runtime:
  for each `STRUCT_SECTION_FOREACH(zephlet, z)`, scan
  `STRUCT_SECTION_FOREACH(zephlet_coap_type, t)` to find the `t` whose
  `t->api == z->api`; if none, skip `z` (it's not opted in); otherwise
  iterate `t->methods[0..t->num_methods)` and emit links.
- For each opted-in instance, emit one link per RPC method
  (`</zlet/{type}/{instance}/{method}>;rt="zlet.rpc";ct=65001`), one
  events link (`</zlet/{type}/{instance}/events>;rt="zlet.event";obs;ct=65001`),
  and one stats link
  (`</zlet/{type}/{instance}/events/stats>;rt="zlet.stats";ct=65001`).
- Output is built into a stack-bounded buffer (max link-format length
  configurable via Kconfig, default 1024 bytes). If the cross-product
  exceeds the buffer, the handler returns `4.13 Request Entity Too Large`
  and a `LOG_WRN` — operators bump the Kconfig.

**Acceptance:** `GET /.well-known/core` lists exactly the opted-in
resources (RPC + events + stats per opted-in instance) with correct
attributes; a non-opted-in zephlet never appears; truncation is detected
and surfaced (not silently lost).

### Phase 5 — Event bridge (CoAP Observe)

- The compile-time `ZEPHLET_EVENTS_LISTENER` for each opted-in instance
  is already wired by the Phase 1 hook (it expanded inside
  `ZEPHLET_NEW_PRIO` via `_ZLET_COAP_HOOK_<type>(_name)`). The hook
  expansion also created the per-instance
  `STRUCT_SECTION_ITERABLE(zephlet_coap_instance_state, _<name>_coap_state)`
  record holding `dropped_total`, `active_observers`, `observe_seq`. Its
  workqueue callback `_<type>_coap_event_cb` is defined in
  `<prefix>_coap_interface.c`.
- The catch-all resource's `.get` branch for `/zlet/{type}/{instance}/events`
  + Observe option:
  - Resolve `(type, instance)` via the Phase 3 shared path resolution.
  - `coap_register_observer(resource, request, observer)`, then set
    `observer->user_data = (void *)z`.
  - Look up the instance's state record by scanning
    `STRUCT_SECTION_FOREACH(zephlet_coap_instance_state, s)` for
    `s->z == z`; `atomic_inc(&s->active_observers)`.
  - On observer removal (RFC 7641 deregistration or timeout): mirror
    decrement.
- The `_<type>_coap_event_cb` body:
  - Recovers `z` via `zbus_chan_user_data(chan)` and the matching `s` via
    the same section walk.
  - **Verify before Phase 5 starts** how Zephyr exposes a per-resource
    observer list publicly — the `coap_resource._observers` field has
    appeared as an internal `sys_slist` in the past but its public API
    surface should be confirmed (`COAP_OBSERVER_FOREACH` macro? iterator
    function?). If only the private field is available, the frontend
    keeps its own per-instance `sys_slist` of observer pointers,
    populated on register/remove.
  - For each observer with `observer->user_data == z`: build a NON
    packet, attach the Observe option using `s->observe_seq`,
    `coap_resource_send`. On send failure → `atomic_inc(&s->dropped_total)`
    + `LOG_WRN("<type>/<instance>: send drop")`. On success →
    `s->observe_seq++` (so the observer's view stays gap-free per
    RFC 7641 §3.4).
- Per-instance `dropped_total` is exported via the stats GET branch
  (`/zlet/{type}/{instance}/events/stats`) returning a `CoapEventsStats
   { uint32 dropped_total; uint32 active_observers; }` nanopb message
  defined in `frontends/coap/proto/zlet_coap_stats.proto`.
- Zbus async-listener enqueue drops (workqueue behind) also
  `atomic_inc(&s->dropped_total)` via a `ZBUS_LISTENER_ENABLED` /
  drop-callback hook, so both drop sources land in the same counter.
- Integration test: subscribe with libcoap/aiocoap, drive events, assert
  delivery; induce backpressure by pinning the workqueue, assert
  `dropped_total` increments and the observer's Observe values stay
  monotonic without gaps.

**Acceptance:** Observe delivers events; backpressure drops cleanly with
the per-zephlet counter visible via stats GET; observer-side sequence
monotonicity preserved under drop; publisher latency unaffected with/without
subscribers.

### Phase 6 — Hardening + optional DTLS

- Malformed/oversized request handling re-audit.
- Block-wise transfer (RFC 7959) **only if** a measured real payload
  exceeds a single-datagram MTU — do not pre-build.
- `CONFIG_ZEPHLETS_COAP_DTLS=y` path: PSK config surface (no secrets in
  code; documented config keys only). Cert support deferred to a follow-up.
- Final pass of the Phase 0 section-hash gate.
- Soak / interop test against `libcoap` or `aiocoap` from host.

**Acceptance:** hardening tests green; DTLS path builds and connects with
test credentials; `=n` section hash holds at final commit.

## Cross-cutting constraints

- Never modify zephlet domain logic, handler signatures, or core codegen
  output for the non-CoAP path. Phase 1 enforces this; subsequent phases
  must re-verify the regression diff.
- Every phase ends with: full `=y` build green, full `=n` build green +
  section-hash-identical to Phase 0 baseline, phase tests green.
- No per-call shared state. Stack-local envelope per request.
- No proto schema is shipped onto the device in v1.
- All CoAP constants live in `frontends/coap/include/zephlet_coap_consts.h`.

## Out of scope (v1)

- **Confirmable (CON) notifications.** v1 is NON-only. Per-event or
  service-level CON support is deferred (would need a proto annotation
  surface and per-resource outstanding-CON tracking per RFC 7641 §4.5).
- **Per-event proto annotations.** No field-level or message-level
  annotations on `Events` in v1. Add only when a real use case appears.
- **In-stream drop notification.** v1 surfaces drops only via the stats
  resource and logs. Synthetic "drops occurred" events are not emitted.
- Binary `FileDescriptorSet` discovery resource (later, own Kconfig).
- Bidirectional streaming (no clean CoAP analog).
- gRPC wire compatibility (non-goal at this layer).
- Block-wise transfer unless a measured payload requires it.
- DTLS certificate-based auth (PSK only in v1 DTLS path).

## Issue map

| Phase | Infra issue |
|---|---|
| Umbrella | [#27](https://github.com/rodrigopex/zephlet/issues/27) |
| 0 — skeleton + gate | [#28](https://github.com/rodrigopex/zephlet/issues/28) |
| 1 — codegen | [#29](https://github.com/rodrigopex/zephlet/issues/29) |
| 2 — translator | [#30](https://github.com/rodrigopex/zephlet/issues/30) |
| 3 — RPC dispatch | [#31](https://github.com/rodrigopex/zephlet/issues/31) |
| 4 — discovery | [#32](https://github.com/rodrigopex/zephlet/issues/32) |
| 5 — events bridge | [#33](https://github.com/rodrigopex/zephlet/issues/33) |
| 6 — hardening + DTLS | [#34](https://github.com/rodrigopex/zephlet/issues/34) |

## Adoption log

| Date | Phase | PR | Notes |
|---|---|---|---|
| — | — | — | — |
