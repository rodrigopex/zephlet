# Shell Frontend — Design & As-Built Notes

Status: Implemented (v1)
Closes: #53
Supersedes: an earlier, rejected draft that used Zephyr's *dynamic* shell
subcommands (`SHELL_DYNAMIC_CMD_CREATE`, runtime linker-section walks, a
stashed context between completion levels) plus new Python codegen that
parsed each proto message's fields itself.

## Scope

Expose every zephlet instance's RPCs under one Zephyr shell root command,
`zlet <instance> <rpc> [args]`, generated as thin wrappers over the RPCs
each zephlet's `.proto` already defines — full tab-completion, a
type-driven value syntax (`h<hex>` for hex/byte values, `"<string>"` for
strings), zero runtime introspection.

## Locked decisions

- **Fully static.** Every command-tree node is a compile-time
  `SHELL_STATIC_SUBCMD_SET_CREATE` entry. No `SHELL_DYNAMIC_CMD_CREATE`,
  no linker-section walk for dispatch, no stashed/global context.
- **One handler per (instance × RPC), not per (type × RPC).** Confirmed by
  reading `zephyr/subsys/shell/shell.c`'s `execute()`/`active_cmd_prepare()`:
  the `argv` handed to the deepest handler starts at *that command's own
  token* — ancestor tokens (the instance name) are dropped before the
  handler ever sees them. A handler shared across instances of a type has
  no way to know which instance invoked it. Baking the instance in as a
  compile-time literal sidesteps this with no stashed context.
- **Driven entirely by nanopb's own generated `<MSG>_FIELDLIST(X, a)`
  macro** (confirmed in a real generated header,
  `TICK_CONFIG_FIELDLIST(X, a) X(a, STATIC, SINGULAR, UINT32, duration_ms, 1) ...`).
  Defining our own `X` and invoking `<MSG>_FIELDLIST(OUR_X, req)` expands,
  at compile time, to one call per field carrying the field's C member
  name and nanopb's own exact type token. An empty message expands to
  nothing — no special-casing for `Empty` needed.
- **No per-service proto opt-in, but full frontend-hook auto-registration.**
  Unlike CoAP (see ADR-0001), shell exposure is gated only by
  `CONFIG_ZEPHLETS_SHELL` — enabling shell already implies local,
  privileged console access, so there's no per-service opt-in to make.
  What shell *does* share with CoAP: `_ZLET_SHELL_HOOK_<type>` is chained
  into `_ZLET_FRONTEND_HOOKS_<type>` exactly like `_ZLET_COAP_HOOK_<type>`
  is, so a plain `ZEPHLET_NEW(...)` call registers the instance under the
  `zlet` shell root automatically — no app-level `ZLET_SHELL_INSTANCE`
  call, no manifest of instance names. Root-level registration uses
  Zephyr's own decentralized multi-file shell mechanism
  (`SHELL_SUBCMD_SET_CREATE`/`SHELL_SUBCMD_ADD`, the same primitive the
  in-tree `kernel`/`thread` commands use) so each instance's translation
  unit can add itself independently. See "Deviations" below.
- **Family-based value dispatch, not per-token.** nanopb's full scalar
  `ltype` set (21 tokens: `BOOL, BYTES, DOUBLE, ENUM, UENUM, FIXED32,
  FIXED64, FLOAT, INT32, INT64, MESSAGE, MSG_W_CB, SFIXED32, SFIXED64,
  SINT32, SINT64, STRING, UINT32, UINT64, EXTENSION,
  FIXED_LENGTH_BYTES`) groups into 6 families (unsigned int, signed int,
  float, bool, bytes, string). One shared parse/print function per family
  (5 total — bool has no separate function), with the exact token still
  driving a compile-time-correct assignment via `__typeof__`. `MESSAGE`,
  `MSG_W_CB`, `EXTENSION` have no entry — a field of one of these types
  fails to compile (undefined macro → undeclared-identifier error), not a
  silent runtime gap.
- **Scope cut: `SINGULAR` + `STATIC` fields only**, enforced at compile
  time (`ZLET_SHELL_CHECK_ATYPE_STATIC`/`ZLET_SHELL_CHECK_HTYPE_SINGULAR`
  — any other atype/htype pastes into an undefined macro name and fails
  to build). `REPEATED` fields, proto3 explicit-`optional` presence
  tracking, and submessages are out of scope for v1 — flagged here, not
  silently unsupported.

## Design

### 1. Codegen — one new X-macro table per type

`codegen/generate_zephlet.py`'s `parse_proto()` already builds a
`commands` list with `name`/`req_c_name`/`resp_c_name`/`req_is_empty`/
`resp_is_empty` per RPC. Two fields were added per command, computed
purely from data already parsed (no new proto walking):

- `req_shell_lc`/`req_shell_uc`, `resp_shell_lc`/`resp_shell_uc` — the
  lowercase/uppercase nanopb struct name for the request/response
  message, with `Empty` (whose `req_c_name`/`resp_c_name` is `""`)
  resolved to `empty`/`EMPTY` — the shared `zephlet.proto` `Empty`
  message's own `EMPTY_FIELDLIST` macro name.
- `shell_call_shape` — one of `EMPTY_EMPTY`/`EMPTY_RESP`/`REQ_EMPTY`/
  `REQ_RESP`, decided here since codegen already knows
  `req_is_empty`/`resp_is_empty`. This is a deliberate deviation from the
  issue's illustrative 5-tuple row shape (`name, req_lc, req_uc, resp_lc,
  resp_uc`) — see "Deviations from the original proposal" below.

`zephlet_interface.h.jinja` renders two macros per type, always (no
opt-in branch — verified byte-identical across the CoAP opt-in toggle by
`test_shell_methods_invariant_across_coap_opt_in`
in `tests/codegen/test_codegen.py`):

```c
#define TICK_SHELL_METHODS(X) \
	X(start, empty, EMPTY, lifecycle_status, LIFECYCLE_STATUS, EMPTY_RESP) \
	...
	X(kick, empty, EMPTY, lifecycle_status, LIFECYCLE_STATUS, EMPTY_RESP)

#define _ZLET_SHELL_METHODS_APPLY_tick(_instance, X) \
	X(tick, _instance, start, empty, EMPTY, lifecycle_status, LIFECYCLE_STATUS, EMPTY_RESP) \
	...
```

`_ZLET_SHELL_METHODS_APPLY_<type>` bakes the type in **literally** (not
as a forwarded macro parameter) so its own name can be pasted directly
from the lowercase `_type` token passed to `ZLET_SHELL_INSTANCE(_type,
_instance)` — mirroring this same file's existing
`_ZLET_COAP_HOOK_{{ type_snake }}` convention. The C preprocessor cannot
case-convert a token, so a macro invoked via `_type##_SUFFIX` paste must
itself be spelled in `_type`'s own case.

### 2. Macro framework (`frontends/shell/include/zephlet_shell_macros.h`)

Hand-written, header-only. Key pieces, in the order they appear in the
file:

- `ZLET_SHELL_CHECK_ATYPE_STATIC`/`_HTYPE_SINGULAR` — the compile-time
  scope guard described above.
- `ZLET_SHELL_PARSE_<TOKEN>`/`ZLET_SHELL_PRINT_<TOKEN>` for all 18
  supported tokens, each aliasing one of 6 family implementations
  (`_UINT_FAMILY`, `_INT_FAMILY`, `_FLOAT_FAMILY`, `_BOOL`, `_BYTES`,
  `_FIXED_LENGTH_BYTES`, `_STRING`). The family macros parse into a
  scratch type (`uint64_t`/`int64_t`/`double`) then assign through
  `__typeof__((a).name)` — **not** `typeof`: Zephyr's C files build
  without GNU-dialect `typeof` recognition, and this repo hit that build
  failure directly (`error: implicit declaration of function 'typeof'`)
  before switching to `__typeof__`, which Zephyr's own headers
  (`zephyr/sys/dlist.h`, `rb.h`, ...) already rely on.
- `ZLET_SHELL_PARSE_FIELD`/`ZLET_SHELL_PRINT_FIELD` — the per-field
  dispatcher, called once per row by nanopb's own `_FIELDLIST` macro.
- `ZLET_SHELL_FIELD_COUNT`/`ZLET_SHELL_HELP` — take a `_FIELDLIST` macro
  *name* (unexpanded) and invoke it themselves; safe because the
  preprocessor rescans a macro's expansion for further macro calls.
  `ZLET_SHELL_HELP` seeds its concatenation with a leading `""` so a
  zero-field message (`Empty`) still produces a valid, non-empty string
  expression rather than a syntactically-empty macro argument.
- `ZLET_SHELL_DEFINE_METHOD_EMPTY_EMPTY`/`_EMPTY_RESP`/`_REQ_EMPTY`/
  `_REQ_RESP` — **four separate generator macros**, one per call shape,
  dispatched by `ZLET_SHELL_DEFINE_METHOD` via `##_shape` paste. See
  "Deviations" below for why this replaced the issue's single generic
  template.
- `ZLET_SHELL_SUBCMD_ENTRY` — one `SHELL_CMD_ARG(...)` per RPC,
  `mandatory = 1 + ZLET_SHELL_FIELD_COUNT(...)` (Zephyr's `mandatory`
  counts the command token itself).
- `ZLET_SHELL_INSTANCE(_type, _instance)` — defines every handler for
  that instance, builds its own `SHELL_STATIC_SUBCMD_SET_CREATE`, then
  self-registers under the `zlet` root via
  `SHELL_SUBCMD_ADD((zlet), _instance, &_zlet_shell_subcmds_##_instance, ...)`.
  Invoked automatically, once per instance, by the
  `_ZLET_SHELL_HOOK_<type>` hook chained into `zephlet.h`'s
  `_ZLET_FRONTEND_HOOKS_<type>` (see §1 and "Deviations" below) — never
  called directly by application code.

The `zlet` root command itself is defined exactly once, unconditionally,
in `frontends/shell/zephlet_shell_root.c`:
```c
SHELL_SUBCMD_SET_CREATE(zlet_shell_root_cmds, (zlet));
SHELL_CMD_REGISTER(zlet, &zlet_shell_root_cmds, "Invoke a zephlet instance's RPC.", NULL);
```
`SHELL_SUBCMD_SET_CREATE`/`SHELL_SUBCMD_ADD` (`zephyr/include/zephyr/shell/shell.h`)
are Zephyr's own primitive for "commands added from multiple files" —
each addition is a `TYPE_SECTION_ITERABLE` entry tagged by parent, walked
at dispatch time (`shell_utils.c`'s `is_section_cmd()`/`z_shell_cmd_get()`).
The in-tree `kernel`/`thread` shell commands
(`zephyr/subsys/shell/modules/kernel_service/`) use exactly this pattern:
`kernel_shell.c` creates the `kernel` parent; `thread/thread.c`, in a
completely different translation unit, adds `thread` as its child. No
central manifest of children exists anywhere for either tree.

### 3. Value parse/print (`frontends/shell/zephlet_shell_value.{h,c}`)

Five parse functions (`zlet_shell_parse_uint/_int/_float/_bool/
_hexbytes`) plus a bounded string copy, and matching print functions.
Notable implementation details found only by testing against real
nanopb-generated structs (not knowable from the issue's illustrative
pseudocode alone):

- `zlet_shell_parse_uint()` must explicitly reject a leading `-`.
  `strtoull()` accepts one and silently returns the two's-complement
  wraparound (`"-1"` → `UINT64_MAX`) — a real bug caught by
  `test_parse_uint_decimal_and_hex` in `tests/shell_macros/`.
  `zlet_shell_parse_int()` has no such issue (signed).
- `h<hex>` is a hex *numeral*, not a byte-dump, for every integer family:
  `h1F4` parses as the number 500. Only the bytes/fixed-length-bytes
  family decodes `h<hex>` into raw bytes.
- Bytes fields require an even hex-digit count — rejected explicitly,
  even though `hex2bin()` itself tolerates odd length by inserting a
  leading zero nibble (`zephyr/lib/utils/hex.c`).
- `zlet_shell_parse_string()`'s `cap` includes the NUL terminator,
  matching nanopb's own STRING field sizing (`char name[max_size+1]`).

### 4. Value syntax

Per Zephyr's own tokenizer (`shell_utils.c`'s `make_argv()`), a quoted
token's quotes are stripped before the handler ever sees `argv[]`, and
quoting only matters for letting one argument contain spaces. So:

- Integer families: decimal (`-` allowed for signed), or `h<hex>` (a hex
  numeral). The literal quotes in `h"<hex>"` are a purely cosmetic
  typing convention — `hDEADBEEF` alone is already unambiguous.
- `float`/`double`: `strtod()` syntax (`3.14`, `-0.5`, `1e-3`).
- `bool`: `true`/`false`/`1`/`0`.
- `bytes`/`fixed_length_bytes`: `h<hex>`, even digit count, bounded by
  the field's own `sizeof()`.
- `string`: an ordinary (optionally quoted) shell token, bounds-checked
  against the field's declared `char[N]`.

## Deviations from the original issue proposal

The issue's own design (§1–§7) is thorough and was treated as final per
user direction, but three implementation-level gaps surfaced only once
real nanopb-generated code was compiled, exercised, and — for the
third — actually flashed to hardware. All three are called out in the
issue itself as "implementation detail, not a design risk", or fall
squarely within its own explicit ask for "an extra macro call per
instance", which turned out to be avoidable entirely:

1. **Call-shape dispatch is explicit, not name-inferred.** The issue's
   illustrative `ZLET_SHELL_DEFINE_METHOD` template always calls
   `_type##_##_name(&_instance, &req, &resp)` — a uniform 3-arg shape.
   But the real generated wrappers (`zephlet_interface.h.jinja`) have
   **four** shapes depending on `(req_is_empty, resp_is_empty)`, each
   with a trailing `k_timeout_t` the illustrative template also omitted.
   Inferring the shape from whether `req_uc`/`resp_uc == "EMPTY"` at the
   macro layer would need a nontrivial preprocessor "is-token-equal"
   hack (the classic `BOOST_PP_IS_EMPTY`-style probe/paste trick).
   Instead, codegen — which already computes `req_is_empty`/
   `resp_is_empty` today — emits the shape as an explicit 6th field
   (`shell_call_shape`), and the macro framework provides one generator
   macro per shape. This also sidesteps declaring an unused `argi`/
   `argv`-consuming loop or an unreachable `zlet_shell_bad:` label for
   the two shapes with no request fields, which would otherwise warn (or
   error, under `-Werror`) on an unused label.
2. **The per-type "apply" macro bakes its type in literally**, emitted
   as `_ZLET_SHELL_METHODS_APPLY_<type>(_instance, X)` rather than the
   issue's `<TYPE>_SHELL_METHODS_APPLY(_type, _instance, X)`. The
   preprocessor cannot uppercase a token, so a macro meant to be invoked
   via `_type##_SUFFIX` paste (from `ZLET_SHELL_INSTANCE`'s lowercase
   `_type` argument) must itself be spelled in lowercase.
3. **Registration is fully automatic — no `ZLET_SHELL_INSTANCE`/
   `ZLET_SHELL_DEFINE` call anywhere in application code.** The issue's
   design still asked the app author to invoke `ZLET_SHELL_INSTANCE(...)`
   once per instance plus one final `ZLET_SHELL_DEFINE(...)` manifest.
   The first real-hardware smoke test (flashing the example app,
   `ports_adapters_zbus`) surfaced this as friction the moment it required
   *any* app-repo edit for what was scoped as an infra-only issue. Since
   `_ZLET_FRONTEND_HOOKS_<type>` already exists precisely to let a
   frontend hook itself into `ZEPHLET_NEW(...)` (CoAP already does this —
   see ADR-0001), and Zephyr's shell subsystem already ships a
   decentralized multi-file registration primitive
   (`SHELL_SUBCMD_SET_CREATE`/`SHELL_SUBCMD_ADD`, used in-tree by the
   `kernel`/`thread` shell commands), both app-level macros are gone:
   `ZLET_SHELL_INSTANCE` is invoked automatically by the new
   `_ZLET_SHELL_HOOK_<type>` hook, and it self-registers into the `zlet`
   root (defined once in `frontends/shell/zephlet_shell_root.c`) instead
   of building a static list a `ZLET_SHELL_DEFINE(...)` manifest would
   have needed. `CONFIG_ZEPHLETS_SHELL=y` is now the *entire* adoption
   cost for an app — matching CoAP's own zero-app-code experience for an
   opted-in zephlet.

None of the three change the design's guarantees for the shell command
itself (still fully static, still one handler per instance×RPC, still
compile-time field-type checking) — they're exactly the kind of
naming/dispatch/ergonomics detail the issue flagged as open, or an
improvement the issue's own reasoning (§1's aggregator precedent) already
pointed toward without spelling out.

## Known integration gotcha

`zlet_shell_print_float()`/`_print_double()` use `"%g"`. Zephyr's
`cbprintf` silently **drops** floating-point conversions unless built
with `CONFIG_CBPRINTF_COMPLETE=y` and `CONFIG_CBPRINTF_FP_SUPPORT=y` — a
zephlet with a `float`/`double` field will otherwise print the literal
string `f_float = %g` instead of the value (caught by
`test_round_trip_decimal_and_hex_all_18_fields` in
`tests/shell_macros/`, and now documented on `CONFIG_ZEPHLETS_SHELL`'s
own Kconfig help). Neither symbol is force-selected — the cost is
real (code size) and most zephlets have no float/double shell field —
so an app author with one must opt in explicitly.

## Testing

- `tests/shell_macros/` (ZTEST, native_sim): `src/fixture.proto`
  declares exactly one field per supported token — all 18, cross-checked
  by `BUILD_ASSERT(ZLET_SHELL_FIELD_COUNT(FIXTURE_FIELDLIST) == 18, ...)`
  so a dropped token fails the build, not just a missing test case.
  Covers decimal + `h<hex>` round-trip (parse then print, via a
  throwaway registered shell command — `shell_print()`/`shell_error()`
  are no-ops outside an active shell command context) and malformed-input
  rejection for every family.
- `tests/shell_functional/` (pytest + `twister_harness.Shell`,
  native_sim): two `tick` instances + one `ui` instance, each a plain
  `ZEPHLET_NEW(...)` call with no shell-specific code — registration
  under `zlet` is automatic. Covers a base RPC
  round-trip, decimal + hex `config`/`get_config`, a custom (non-base)
  RPC (`kick`), the `mandatory`-argument-count gate, validation-error
  propagation, unknown-instance/unknown-RPC clean failure (Zephyr's own
  "print this level's subcommands" behavior for a NULL-handler group —
  not literally "command not found", which is reserved for an unknown
  *root* command), and no cross-talk between same-type and
  cross-type instances.
- `tests/codegen/test_codegen.py`: two new cases assert the emitted
  `<TYPE>_SHELL_METHODS`/`_ZLET_SHELL_METHODS_APPLY_<type>` rows exactly,
  and that they're byte-identical whether or not the CoAP opt-in option
  is set; a third asserts `_ZLET_SHELL_HOOK_tick` is chained into
  `_ZLET_FRONTEND_HOOKS_tick` alongside the CoAP hook.
- `tests/coap_functional/pytest/test_dual_frontend.py` (new scenario,
  `zephlet.coap_functional.dual_frontend`, `CONFIG_ZEPHLETS_SHELL=y`
  added on top of `tick_fast`'s existing CoAP opt-in): proves the two
  hooks chained into `tick_fast`'s `_ZLET_FRONTEND_HOOKS_tick` operate on
  the *same* live instance, not two independent copies — a `config`
  write through one frontend (shell or CoAP) is read back correctly
  through the other. This is the test that answers "how do you know both
  are actually running at the same time" — a symbol-level check
  (`nm` showing both `_shell_zlet` and `tick_coap_resource` in one
  binary) or two frontends' test suites passing independently would only
  prove they compile and function in isolation, not that they share
  state.

## Out of scope (v1)

`repeated` fields, proto3 explicit-`optional` presence tracking,
submessage/extension fields (rejected at compile time by design, not
merely unimplemented), events/watch, protobuf text format, a per-service
proto opt-in (shell's gate is `CONFIG_ZEPHLETS_SHELL` alone).
