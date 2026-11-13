# ADR-0002 — Explicit dependencies between zephlets

Status: Proposed
Date: 2026-11-13
Supersedes: the "zephlets never talk to each other directly" rule; v0.3's single SYS_INIT that sorts instances by `init_priority`

## Context

A zephlet owns its state and exposes it through RPCs on its command channel
and events on its events channel. Until now zephlets never called each other:

- **Events and policies** (`ZEPHLET_EVENTS_LISTENER` in `policies.c`) turn
  one zephlet's event into a command on another.
- **Coordinators** run multi-step, application-level flows across zephlets,
  waiting on events and timeouts.

Both are asynchronous and both live outside the zephlets. Neither fits a
zephlet that needs a *reply* from another service to do its own job, inside
one of its own RPCs: a climate controller that reads a temperature and sets
a fan before answering `regulate`. Emulating that with events needs:

- a request event and a reply command, with a correlation id and a timeout;
- a policy per consumer instance that knows which provider instances belong
  to which consumer;
- the consumer's RPC split into "ask" and "on reply" halves.

That wiring is application code that encodes what is really a property of
the consumer type ("it needs a temperature and two fans") plus a per-instance
choice ("these ones").

## Decision

1. **Declare.** A zephlet type names the provider types it calls in its proto
   (`option (zephlet.requires) = { type: "fan" name: "intake" };`) or its
   `.zephlet.options` (`requires:intake=fan`). Codegen emits a
   `struct <t>_dependencies` with one `const struct zephlet *` per slot and a
   slot table in `<t>_api`.
2. **Bind.** The application binds provider instances per consumer instance
   with `ZEPHLET_NEW_WITH_DEPS`, each slot written `ZEPHLET_DEP(type, inst)`,
   which `_Generic` checks against the instance's type at compile time.
3. **Call.** A handler reads `z->dependencies` and calls providers only
   through their public RPC wrappers: the ordinary synchronous RPC, in the
   caller's thread.
4. **Check at build time.**
   - A build step reads every zephlet's requires, from the `<prefix>.requires`
     file its own generation writes, and fails before compiling on a missing
     type or a cycle between types.
   - A post-link step (`CONFIG_ZEPHLET_DEPS_LINK_CHECK`) fails on an unbound
     or mistyped slot. A slot only accepts its own type, so an instance cycle
     needs a type cycle, which the build step already rejects.
5. **Initialize with the kernel.** Each instance has its own
   `SYS_INIT` entry at `APPLICATION` priority `CONFIG_ZEPHLET_INIT_PRIORITY` +
   its type's *init rank*: 0 for a type that requires nothing, else one above
   the highest provider rank. The build step writes the ranks to
   `zephlet_init_prio.h`. `init_priority` is the entry's sub-priority, a bare
   literal 0–9. Each entry calls `zephlet_init_one()`:
   - it skips the instance with `-ENODEV` on a bad slot or a provider that
     failed or has not run yet;
   - otherwise it runs `init_fn` once.
6. **One gate.** Each instance points at a runtime record in `.bss`.
   `zephlet_dispatch()` answers every instance alike, in O(1):
   - `-EAGAIN` before its init;
   - served while its `init_fn` runs (so it may call its own RPCs) and after
     it returned 0;
   - `-ENODEV` once skipped or failed.

## When to use which

| Need | Mechanism |
|---|---|
| A reply from another service inside one of my RPCs | Dependencies |
| React to another zephlet's event; neither knows the other | Events and policies (adapters) |
| An application flow over several zephlets, events and timeouts | Coordinators |

Dependencies are synchronous and nest. A consumer's command-channel lock is
held for the whole provider call: no deadlock, since locks are always taken
consumer before provider and the graph is acyclic, but a slow provider
blocks every caller of the consumer. A consumer with dependencies cannot be
called from an ISR.

## Why one init entry per instance

In v0.3 a single `SYS_INIT` function at `APPLICATION` priority 0 sorts the
instances by `init_priority`, a hand-set `int`, and runs each `init_fn`.
With dependencies, a provider must run before its consumers, and the build
already knows that order: it computes it from the type graph, which it also
checks for cycles.

Options weighed:

| Option | Why not |
|---|---|
| A runtime scheduler that orders instances by their bindings | Runtime code to sort and to handle cycles, for an order the build already has. |
| A generated init chain per type | Splits ordering between codegen and runtime; hand-written apis get no ordering. |
| Runtime depth-first initialization of providers | Recursion (MISRA 17.2), or an explicit stack. |
| A two-stage link with a generated table | A prelink/relink pipeline. |
| Lazy init on first use | Event sources would never start; timing becomes unpredictable; cycles become deadlocks. |
| Two-phase lifecycle (`init` without RPCs, then `start`) | A larger contract change; kept as a possible future direction. |

One `SYS_INIT` entry per instance reuses the kernel's ordering, shows up in
`west build -t initlevels`, and leaves the runtime with no scheduler.

## Consequences

- **Order** is by type rank, then sub-priority. Ties are unspecified: GCC may
  reorder top-level definitions.
- **Migration from v0.3:**
  - zephlets initialize at priority 10 + rank instead of 0;
  - `init_priority` is a 0–9 literal instead of any `int`;
  - instances with equal priorities run in an unspecified order instead of
    name order;
  - calls are gated by init state: `-EAGAIN` before init, `-ENODEV` after a
    failure. v0.3 served every call.
- **Hand-written records** use `ZEPHLET_INIT_DEFINE` and set `.runtime`. Its
  sub-priority relies on Zephyr's internal `Z_INIT_ENTRY_SECTION`, which a
  Zephyr upgrade may need revisited.
- **Footprint:** per instance, 4 B for the runtime pointer, the 8 B
  runtime record (`.bss`), and an 8 B init entry plus a small wrapper in
  flash.

## Tests

- `tests/codegen`: requires parsing, the type graph and the init ranks.
- `tests/deps_config` (pytest builds): the graph check, macro misuse,
  sub-priorities that are not a literal 0–9, the post-link check, and
  rebuilds after a proto edit.
- `tests/deps_functional`: a climate fixture on real zephlets, including an
  `init_fn` that calls its providers and its own RPCs.
- `tests/shared/deps_init`: hand-written records, including cycles.
- `tests/shared/init_unit` (FFF, the real `zephlet.c` on `unit_testing`):
  - the gate in every state, and self-calls from `init_fn`;
  - a failed init with and without dependencies;
  - priority and sub-priority order; providers first; run-once;
  - shared providers and repeated slots;
  - bad, unbound, wrong-type and non-instance slots, and records without a
    runtime;
  - hand-written cycles.
