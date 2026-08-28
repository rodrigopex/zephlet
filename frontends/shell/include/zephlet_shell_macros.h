/* Static, macro-generated `zlet` shell frontend — the core X-macro
 * framework. See docs/plans/shell-frontend.md for the full design.
 *
 * Nothing here walks a message's fields at runtime: every dispatch
 * below is driven by nanopb's own compile-time `<MSG>_FIELDLIST(X, a)`
 * macro (emitted by nanopb itself) and `<TYPE>_SHELL_METHODS_APPLY`
 * (emitted by generate_zephlet.py, see zephlet_interface.h.jinja).
 */

#ifndef ZEPHLET_SHELL_MACROS_H_
#define ZEPHLET_SHELL_MACROS_H_

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util_macro.h>

#include "zephlet_shell_value.h"

/* ----- Compile-time atype/htype guards ---------------------------------
 *
 * Every nanopb field row is `X(a, atype, htype, ltype, name, tag)`. This
 * framework only supports `STATIC` allocation and `SINGULAR` fields
 * (scope explicitly cut for v1 — see docs/plans/shell-frontend.md §2).
 * A field using any other atype/htype (CALLBACK, POINTER, REPEATED,
 * OPTIONAL, ONEOF, FIXARRAY, ...) pastes into an undefined macro name
 * below, which the compiler then reports as an undeclared identifier —
 * a compile-time error at the offending zephlet's build, not a silently
 * wrong shell command. */
#define ZLET_SHELL_CHECK_ATYPE_STATIC(x) x
#define ZLET_SHELL_CHECK_HTYPE_SINGULAR(x) x

/* ----- Per-family parse macros ------------------------------------------
 *
 * One generic macro per nanopb ltype "family" (not per exact token):
 * parse into a family-wide scratch type, then assign through
 * __typeof__((a).name) so the field's *real* declared width/signedness/enum
 * type is what actually gets range-checked and stored. `argv`/`argi` and
 * the `zlet_shell_bad` error label come from the enclosing handler
 * (ZLET_SHELL_DEFINE_METHOD_*, below).
 */

#define ZLET_SHELL_PARSE_UINT_FAMILY(a, name) \
	{ \
		uint64_t zlet_shell_v_; \
		if (zlet_shell_parse_uint(argv[argi], &zlet_shell_v_) != 0) { \
			goto zlet_shell_bad; \
		} \
		argi++; \
		(a).name = (__typeof__((a).name))zlet_shell_v_; \
	}

#define ZLET_SHELL_PARSE_INT_FAMILY(a, name) \
	{ \
		int64_t zlet_shell_v_; \
		if (zlet_shell_parse_int(argv[argi], &zlet_shell_v_) != 0) { \
			goto zlet_shell_bad; \
		} \
		argi++; \
		(a).name = (__typeof__((a).name))zlet_shell_v_; \
	}

#define ZLET_SHELL_PARSE_FLOAT_FAMILY(a, name) \
	{ \
		double zlet_shell_v_; \
		if (zlet_shell_parse_float(argv[argi], &zlet_shell_v_) != 0) { \
			goto zlet_shell_bad; \
		} \
		argi++; \
		(a).name = (__typeof__((a).name))zlet_shell_v_; \
	}

#define ZLET_SHELL_PARSE_BOOL(a, name) \
	{ \
		bool zlet_shell_v_; \
		if (zlet_shell_parse_bool(argv[argi], &zlet_shell_v_) != 0) { \
			goto zlet_shell_bad; \
		} \
		argi++; \
		(a).name = zlet_shell_v_; \
	}

/* BYTES: nanopb's static-allocation storage is a generated
 * `struct { pb_size_t size; pb_byte_t bytes[N]; }` (PB_BYTES_ARRAY_T). */
#define ZLET_SHELL_PARSE_BYTES(a, name) \
	{ \
		size_t zlet_shell_len_; \
		if (zlet_shell_parse_hexbytes(argv[argi], (a).name.bytes, \
					       sizeof((a).name.bytes), \
					       &zlet_shell_len_) != 0) { \
			goto zlet_shell_bad; \
		} \
		argi++; \
		(a).name.size = zlet_shell_len_; \
	}

/* FIXED_LENGTH_BYTES: plain `pb_byte_t name[N]`, no size field — the
 * decoded length must exactly fill the array. */
#define ZLET_SHELL_PARSE_FIXED_LENGTH_BYTES(a, name) \
	{ \
		size_t zlet_shell_len_; \
		if (zlet_shell_parse_hexbytes(argv[argi], (a).name, sizeof((a).name), \
					       &zlet_shell_len_) != 0 || \
		    zlet_shell_len_ != sizeof((a).name)) { \
			goto zlet_shell_bad; \
		} \
		argi++; \
	}

#define ZLET_SHELL_PARSE_STRING(a, name) \
	{ \
		if (zlet_shell_parse_string(argv[argi], (a).name, sizeof((a).name)) != 0) { \
			goto zlet_shell_bad; \
		} \
		argi++; \
	}

/* Unsupported ltypes have no ZLET_SHELL_PARSE_<TOKEN> — a submessage,
 * callback-typed, or extension field pastes into an undefined macro
 * name in ZLET_SHELL_PARSE_FIELD below and fails to compile. */
#define ZLET_SHELL_PARSE_UINT32(a, name)  ZLET_SHELL_PARSE_UINT_FAMILY(a, name)
#define ZLET_SHELL_PARSE_UINT64(a, name)  ZLET_SHELL_PARSE_UINT_FAMILY(a, name)
#define ZLET_SHELL_PARSE_FIXED32(a, name) ZLET_SHELL_PARSE_UINT_FAMILY(a, name)
#define ZLET_SHELL_PARSE_FIXED64(a, name) ZLET_SHELL_PARSE_UINT_FAMILY(a, name)
#define ZLET_SHELL_PARSE_UENUM(a, name)   ZLET_SHELL_PARSE_UINT_FAMILY(a, name)

#define ZLET_SHELL_PARSE_INT32(a, name)   ZLET_SHELL_PARSE_INT_FAMILY(a, name)
#define ZLET_SHELL_PARSE_INT64(a, name)   ZLET_SHELL_PARSE_INT_FAMILY(a, name)
#define ZLET_SHELL_PARSE_SINT32(a, name)  ZLET_SHELL_PARSE_INT_FAMILY(a, name)
#define ZLET_SHELL_PARSE_SINT64(a, name)  ZLET_SHELL_PARSE_INT_FAMILY(a, name)
#define ZLET_SHELL_PARSE_SFIXED32(a, name) ZLET_SHELL_PARSE_INT_FAMILY(a, name)
#define ZLET_SHELL_PARSE_SFIXED64(a, name) ZLET_SHELL_PARSE_INT_FAMILY(a, name)
#define ZLET_SHELL_PARSE_ENUM(a, name)    ZLET_SHELL_PARSE_INT_FAMILY(a, name)

#define ZLET_SHELL_PARSE_FLOAT(a, name)  ZLET_SHELL_PARSE_FLOAT_FAMILY(a, name)
#define ZLET_SHELL_PARSE_DOUBLE(a, name) ZLET_SHELL_PARSE_FLOAT_FAMILY(a, name)

/* One dispatch statement per field, routed through the token-specific
 * macro above, guarded by the atype/htype checks. */
#define ZLET_SHELL_PARSE_FIELD(a, atype, htype, ltype, name, tag) \
	ZLET_SHELL_CHECK_ATYPE_##atype(ZLET_SHELL_CHECK_HTYPE_##htype(ZLET_SHELL_PARSE_##ltype(a, name)))

/* ----- Per-family print macros ------------------------------------------ */

#define ZLET_SHELL_PRINT_UINT_FAMILY(name, val)  zlet_shell_print_uint(sh, name, (uint64_t)(val))
#define ZLET_SHELL_PRINT_INT_FAMILY(name, val)   zlet_shell_print_int(sh, name, (int64_t)(val))
#define ZLET_SHELL_PRINT_FLOAT_FAMILY(name, val) zlet_shell_print_float(sh, name, (double)(val))

#define ZLET_SHELL_PRINT_BOOL(name, val) zlet_shell_print_bool(sh, name, (val))

#define ZLET_SHELL_PRINT_BYTES(name, val) \
	zlet_shell_print_hexbytes(sh, name, (val).bytes, (val).size)
#define ZLET_SHELL_PRINT_FIXED_LENGTH_BYTES(name, val) \
	zlet_shell_print_hexbytes(sh, name, (val), sizeof(val))
#define ZLET_SHELL_PRINT_STRING(name, val) zlet_shell_print_string(sh, name, (val))

#define ZLET_SHELL_PRINT_UINT32(name, val)   ZLET_SHELL_PRINT_UINT_FAMILY(name, val)
#define ZLET_SHELL_PRINT_UINT64(name, val)   ZLET_SHELL_PRINT_UINT_FAMILY(name, val)
#define ZLET_SHELL_PRINT_FIXED32(name, val)  ZLET_SHELL_PRINT_UINT_FAMILY(name, val)
#define ZLET_SHELL_PRINT_FIXED64(name, val)  ZLET_SHELL_PRINT_UINT_FAMILY(name, val)
#define ZLET_SHELL_PRINT_UENUM(name, val)    ZLET_SHELL_PRINT_UINT_FAMILY(name, val)

#define ZLET_SHELL_PRINT_INT32(name, val)    ZLET_SHELL_PRINT_INT_FAMILY(name, val)
#define ZLET_SHELL_PRINT_INT64(name, val)    ZLET_SHELL_PRINT_INT_FAMILY(name, val)
#define ZLET_SHELL_PRINT_SINT32(name, val)   ZLET_SHELL_PRINT_INT_FAMILY(name, val)
#define ZLET_SHELL_PRINT_SINT64(name, val)   ZLET_SHELL_PRINT_INT_FAMILY(name, val)
#define ZLET_SHELL_PRINT_SFIXED32(name, val) ZLET_SHELL_PRINT_INT_FAMILY(name, val)
#define ZLET_SHELL_PRINT_SFIXED64(name, val) ZLET_SHELL_PRINT_INT_FAMILY(name, val)
#define ZLET_SHELL_PRINT_ENUM(name, val)     ZLET_SHELL_PRINT_INT_FAMILY(name, val)

#define ZLET_SHELL_PRINT_FLOAT(name, val)  ZLET_SHELL_PRINT_FLOAT_FAMILY(name, val)
#define ZLET_SHELL_PRINT_DOUBLE(name, val) ZLET_SHELL_PRINT_FLOAT_FAMILY(name, val)

/* One print statement per response field. */
#define ZLET_SHELL_PRINT_FIELD(a, atype, htype, ltype, name, tag) \
	ZLET_SHELL_CHECK_ATYPE_##atype(ZLET_SHELL_CHECK_HTYPE_##htype(ZLET_SHELL_PRINT_##ltype(#name, (a).name)));

/* ----- Field count / help-string helpers --------------------------------
 *
 * Both take a `<MSG>_FIELDLIST` macro *name* (not yet invoked) and invoke
 * it themselves — the standard "pass a macro name, invoke it once
 * substituted" indirection, safe because the C preprocessor rescans a
 * macro's expansion for further macro calls. */

#define ZLET_SHELL_COUNT_ONE(a, atype, htype, ltype, name, tag) +1
#define ZLET_SHELL_FIELD_COUNT(FIELDLIST) (0 FIELDLIST(ZLET_SHELL_COUNT_ONE, _))

#define ZLET_SHELL_HELP_ONE(a, atype, htype, ltype, name, tag) "<" #name "> "
#define ZLET_SHELL_HELP(FIELDLIST) ("" FIELDLIST(ZLET_SHELL_HELP_ONE, _))

/* ----- Per-(instance x RPC) handler generator ---------------------------
 *
 * Four shapes, one per (req_is_empty, resp_is_empty) combination — kept
 * separate (rather than one generic macro branching on a name match)
 * so a shape with no request fields declares no unused `argi`/`argv`
 * locals and no unreachable `zlet_shell_bad` label; each shape calls the
 * exact wrapper signature `generate_zephlet.py` already emits for that
 * RPC in `<prefix>_interface.h`. `call_shape` (one of EMPTY_EMPTY,
 * EMPTY_RESP, REQ_EMPTY, REQ_RESP) is supplied by codegen per RPC, which
 * already knows req_is_empty/resp_is_empty — see zephlet_interface.h.jinja.
 */

#define ZLET_SHELL_DEFINE_METHOD_EMPTY_EMPTY(_type, _instance, _name, _req_lc, _req_uc, \
					      _resp_lc, _resp_uc) \
	static int zlet_shell_##_instance##_##_name##_cmd(const struct shell *sh, \
							    size_t argc, char **argv) \
	{ \
		int rc; \
		ARG_UNUSED(argc); \
		ARG_UNUSED(argv); \
		rc = _type##_##_name(&(_instance), K_MSEC(CONFIG_ZEPHLETS_SHELL_CMD_TIMEOUT_MS)); \
		if (rc != 0) { \
			shell_error(sh, "%s: %d", #_name, rc); \
			return rc; \
		} \
		return 0; \
	}

#define ZLET_SHELL_DEFINE_METHOD_EMPTY_RESP(_type, _instance, _name, _req_lc, _req_uc, \
					     _resp_lc, _resp_uc) \
	static int zlet_shell_##_instance##_##_name##_cmd(const struct shell *sh, \
							    size_t argc, char **argv) \
	{ \
		struct _resp_lc resp = _resp_uc##_INIT_ZERO; \
		int rc; \
		ARG_UNUSED(argc); \
		ARG_UNUSED(argv); \
		rc = _type##_##_name(&(_instance), &resp, \
				      K_MSEC(CONFIG_ZEPHLETS_SHELL_CMD_TIMEOUT_MS)); \
		if (rc != 0) { \
			shell_error(sh, "%s: %d", #_name, rc); \
			return rc; \
		} \
		_resp_uc##_FIELDLIST(ZLET_SHELL_PRINT_FIELD, resp) \
		return 0; \
	}

#define ZLET_SHELL_DEFINE_METHOD_REQ_EMPTY(_type, _instance, _name, _req_lc, _req_uc, \
					    _resp_lc, _resp_uc) \
	static int zlet_shell_##_instance##_##_name##_cmd(const struct shell *sh, \
							    size_t argc, char **argv) \
	{ \
		struct _req_lc req = _req_uc##_INIT_ZERO; \
		size_t argi = 1; \
		int rc; \
		ARG_UNUSED(argc); \
		_req_uc##_FIELDLIST(ZLET_SHELL_PARSE_FIELD, req) \
		rc = _type##_##_name(&(_instance), &req, \
				      K_MSEC(CONFIG_ZEPHLETS_SHELL_CMD_TIMEOUT_MS)); \
		if (rc != 0) { \
			shell_error(sh, "%s: %d", #_name, rc); \
			return rc; \
		} \
		return 0; \
	zlet_shell_bad: \
		shell_error(sh, "%s: bad argument %zu", #_name, argi); \
		return -EINVAL; \
	}

#define ZLET_SHELL_DEFINE_METHOD_REQ_RESP(_type, _instance, _name, _req_lc, _req_uc, \
					   _resp_lc, _resp_uc) \
	static int zlet_shell_##_instance##_##_name##_cmd(const struct shell *sh, \
							    size_t argc, char **argv) \
	{ \
		struct _req_lc req = _req_uc##_INIT_ZERO; \
		struct _resp_lc resp = _resp_uc##_INIT_ZERO; \
		size_t argi = 1; \
		int rc; \
		ARG_UNUSED(argc); \
		_req_uc##_FIELDLIST(ZLET_SHELL_PARSE_FIELD, req) \
		rc = _type##_##_name(&(_instance), &req, &resp, \
				      K_MSEC(CONFIG_ZEPHLETS_SHELL_CMD_TIMEOUT_MS)); \
		if (rc != 0) { \
			shell_error(sh, "%s: %d", #_name, rc); \
			return rc; \
		} \
		_resp_uc##_FIELDLIST(ZLET_SHELL_PRINT_FIELD, resp) \
		return 0; \
	zlet_shell_bad: \
		shell_error(sh, "%s: bad argument %zu", #_name, argi); \
		return -EINVAL; \
	}

#define ZLET_SHELL_DEFINE_METHOD(_type, _instance, _name, _req_lc, _req_uc, _resp_lc, \
				  _resp_uc, _shape) \
	ZLET_SHELL_DEFINE_METHOD_##_shape(_type, _instance, _name, _req_lc, _req_uc, \
					   _resp_lc, _resp_uc)

/* ----- Per-instance subcommand entry ------------------------------------
 *
 * `mandatory` includes the command name itself (Zephyr shell
 * convention) — 1 (the RPC name) plus one token per request field. No
 * optional arguments in v1. */
#define ZLET_SHELL_SUBCMD_ENTRY(_type, _instance, _name, _req_lc, _req_uc, _resp_lc, \
				 _resp_uc, _shape) \
	SHELL_CMD_ARG(_name, NULL, ZLET_SHELL_HELP(_req_uc##_FIELDLIST), \
		      zlet_shell_##_instance##_##_name##_cmd, \
		      1 + ZLET_SHELL_FIELD_COUNT(_req_uc##_FIELDLIST), 0),

/* ----- Per-instance registration ----------------------------------------
 *
 * Invoke once per `ZEPHLET_NEW(...)` instance, e.g. right after it:
 *
 *   ZEPHLET_NEW(tick, tick_fast, &tick_fast_cfg, &tick_fast_data, tick_init_fn);
 *   ZLET_SHELL_INSTANCE(tick, tick_fast);
 *
 * `_ZLET_SHELL_METHODS_APPLY_<type>` is emitted by generate_zephlet.py
 * with `<type>` baked in literally (not pasted from `_type` here), so no
 * case-conversion trick is needed at the paste site below. */
#define ZLET_SHELL_INSTANCE(_type, _instance) \
	_ZLET_SHELL_METHODS_APPLY_##_type(_instance, ZLET_SHELL_DEFINE_METHOD) \
	SHELL_STATIC_SUBCMD_SET_CREATE(_zlet_shell_subcmds_##_instance, \
		_ZLET_SHELL_METHODS_APPLY_##_type(_instance, ZLET_SHELL_SUBCMD_ENTRY) \
		SHELL_SUBCMD_SET_END)

/* ----- Top-level manifest ------------------------------------------------
 *
 * Invoke exactly once, after every ZLET_SHELL_INSTANCE(...) call, listing
 * every instance that should appear under the `zlet` root command:
 *
 *   ZLET_SHELL_DEFINE(tick_fast, tick_slow, ui_main);
 */
#define ZLET_SHELL_ENTRY(_instance) \
	SHELL_CMD(_instance, &_zlet_shell_subcmds_##_instance, "Zephlet instance", NULL),

#define ZLET_SHELL_DEFINE(...) \
	SHELL_STATIC_SUBCMD_SET_CREATE(zlet_shell_instances, \
		FOR_EACH(ZLET_SHELL_ENTRY, (), __VA_ARGS__) \
		SHELL_SUBCMD_SET_END); \
	SHELL_CMD_REGISTER(zlet, &zlet_shell_instances, "Invoke a zephlet instance's RPC.", NULL)

#endif /* ZEPHLET_SHELL_MACROS_H_ */
