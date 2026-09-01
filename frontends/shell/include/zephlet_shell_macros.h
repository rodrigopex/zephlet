/* Static, macro-generated `zlet` shell frontend.
 *
 * RPC arguments are protobuf text format, parsed by
 * zephyr-nanopb-textformat. Nothing here walks a message's fields: the
 * library builds a descriptor tree from nanopb's own `<MSG>_FIELDLIST`
 * at compile time and walks it iteratively, so every field shape works —
 * optional, repeated, submessage, oneof, nested — without this header
 * knowing anything about them.
 *
 * What remains macro-generated is the part that is genuinely per-instance:
 * one handler and one subcommand entry per (instance x RPC), driven by
 * `_ZLET_SHELL_METHODS_APPLY_<type>` from generate_zephlet.py. The
 * descriptors those handlers reference are emitted by the same codegen —
 * `PB_TF_DEFINE` in `<prefix>_interface.c`, `PB_TF_DECLARE` in the header.
 */

#ifndef ZEPHLET_SHELL_MACROS_H_
#define ZEPHLET_SHELL_MACROS_H_

#include <errno.h>

#include <nanopb_textformat.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "zephlet_shell_io.h"

/* ----- Per-(instance x RPC) handler generator ---------------------------
 *
 * Four shapes, one per (req_is_empty, resp_is_empty) combination, so a
 * shape with no request declares no parse locals and a shape with no
 * response declares none for printing. `call_shape` is supplied by codegen
 * per RPC — see zephlet_interface.h.jinja.
 *
 * Every shape that takes a request reads `argv[1]`, the untouched
 * remainder of the command line delivered by SHELL_OPT_ARG_RAW (see
 * ZLET_SHELL_SUBCMD_ENTRY). That argument is optional, so `argc` can be 1.
 *
 * The request is parsed into a stack local and the RPC is dispatched only
 * on PB_TF_OK. That ordering is required, not stylistic: the parser writes
 * decoded bytes straight into the destination with no scratch buffer and
 * refuses a value at the byte that overflows, so a failed parse can leave
 * partial bytes behind. A zephlet never sees a partially-filled message.
 *
 * `pb_tf_parse()` zeroes the message before writing into it, so the
 * request locals below need no initialiser of their own.
 */

#define ZLET_SHELL_DEFINE_METHOD_EMPTY_EMPTY(_type, _instance, _name, _req_lc, _req_uc, _resp_lc,  \
					     _resp_uc)                                             \
	static int zlet_shell_##_instance##_##_name##_cmd(const struct shell *sh, size_t argc,     \
							  char **argv)                             \
	{                                                                                          \
		int rc;                                                                            \
		ARG_UNUSED(argc);                                                                  \
		ARG_UNUSED(argv);                                                                  \
		rc = _type##_##_name(&(_instance), K_MSEC(CONFIG_ZEPHLETS_SHELL_CMD_TIMEOUT_MS));  \
		if (rc != 0) {                                                                     \
			shell_error(sh, "%s: %d", #_name, rc);                                     \
			return rc;                                                                 \
		}                                                                                  \
		return 0;                                                                          \
	}

#define ZLET_SHELL_DEFINE_METHOD_EMPTY_RESP(_type, _instance, _name, _req_lc, _req_uc, _resp_lc,   \
					    _resp_uc)                                              \
	static int zlet_shell_##_instance##_##_name##_cmd(const struct shell *sh, size_t argc,     \
							  char **argv)                             \
	{                                                                                          \
		struct _resp_lc resp = _resp_uc##_INIT_ZERO;                                       \
		int rc;                                                                            \
		ARG_UNUSED(argc);                                                                  \
		ARG_UNUSED(argv);                                                                  \
		rc = _type##_##_name(&(_instance), &resp,                                          \
				     K_MSEC(CONFIG_ZEPHLETS_SHELL_CMD_TIMEOUT_MS));                \
		if (rc != 0) {                                                                     \
			shell_error(sh, "%s: %d", #_name, rc);                                     \
			return rc;                                                                 \
		}                                                                                  \
		(void)pb_tf_print(&_resp_lc##_t_tf, &resp, zlet_shell_out, (void *)sh,             \
				  PB_TF_MULTILINE);                                                \
		return 0;                                                                          \
	}

#define ZLET_SHELL_DEFINE_METHOD_REQ_EMPTY(_type, _instance, _name, _req_lc, _req_uc, _resp_lc,    \
					   _resp_uc)                                               \
	static int zlet_shell_##_instance##_##_name##_cmd(const struct shell *sh, size_t argc,     \
							  char **argv)                             \
	{                                                                                          \
		struct _req_lc req;                                                                \
		struct pb_tf_status st;                                                            \
		enum pb_tf_err terr;                                                               \
		int rc;                                                                            \
		if (argc < 2) {                                                                    \
			shell_error(sh, "%s: expected a text-format message", #_name);             \
			return -EINVAL;                                                            \
		}                                                                                  \
		terr = pb_tf_parse(&_req_lc##_t_tf, &req, argv[1], 0U, &st);                       \
		if (terr != PB_TF_OK) {                                                            \
			zlet_shell_report_tf_err(sh, #_name, terr, &st);                           \
			return -EINVAL;                                                            \
		}                                                                                  \
		rc = _type##_##_name(&(_instance), &req,                                           \
				     K_MSEC(CONFIG_ZEPHLETS_SHELL_CMD_TIMEOUT_MS));                \
		if (rc != 0) {                                                                     \
			shell_error(sh, "%s: %d", #_name, rc);                                     \
			return rc;                                                                 \
		}                                                                                  \
		return 0;                                                                          \
	}

#define ZLET_SHELL_DEFINE_METHOD_REQ_RESP(_type, _instance, _name, _req_lc, _req_uc, _resp_lc,     \
					  _resp_uc)                                                \
	static int zlet_shell_##_instance##_##_name##_cmd(const struct shell *sh, size_t argc,     \
							  char **argv)                             \
	{                                                                                          \
		struct _req_lc req;                                                                \
		struct _resp_lc resp = _resp_uc##_INIT_ZERO;                                       \
		struct pb_tf_status st;                                                            \
		enum pb_tf_err terr;                                                               \
		int rc;                                                                            \
		if (argc < 2) {                                                                    \
			shell_error(sh, "%s: expected a text-format message", #_name);             \
			return -EINVAL;                                                            \
		}                                                                                  \
		terr = pb_tf_parse(&_req_lc##_t_tf, &req, argv[1], 0U, &st);                       \
		if (terr != PB_TF_OK) {                                                            \
			zlet_shell_report_tf_err(sh, #_name, terr, &st);                           \
			return -EINVAL;                                                            \
		}                                                                                  \
		rc = _type##_##_name(&(_instance), &req, &resp,                                    \
				     K_MSEC(CONFIG_ZEPHLETS_SHELL_CMD_TIMEOUT_MS));                \
		if (rc != 0) {                                                                     \
			shell_error(sh, "%s: %d", #_name, rc);                                     \
			return rc;                                                                 \
		}                                                                                  \
		(void)pb_tf_print(&_resp_lc##_t_tf, &resp, zlet_shell_out, (void *)sh,             \
				  PB_TF_MULTILINE);                                                \
		return 0;                                                                          \
	}

#define ZLET_SHELL_DEFINE_METHOD(_type, _instance, _name, _req_lc, _req_uc, _resp_lc, _resp_uc,    \
				 _shape)                                                           \
	ZLET_SHELL_DEFINE_METHOD_##_shape(_type, _instance, _name, _req_lc, _req_uc, _resp_lc,     \
					  _resp_uc)

/* ----- Per-instance subcommand entry ------------------------------------
 *
 * SHELL_OPT_ARG_RAW is required, not a convenience. Taking the message as
 * ordinary arguments and rejoining argv is broken three ways, all silent:
 * CONFIG_SHELL_ARGC_MAX (default 20) makes the shell drop every token past
 * the cap; z_shell_make_argv() collapses whitespace runs and strips quotes
 * and backslashes, so no rejoin can reconstruct a string value containing
 * any of them; and an escape like `\x0F` would lose its backslash before
 * the handler ever ran.
 *
 * On a leaf command — which every RPC entry is, its subcommand set being
 * NULL — raw stops tokenising after the mandatory arguments and hands the
 * handler the untouched remainder of the command line at `argv[mandatory]`.
 * `mandatory` is 1 (the RPC name alone), so that is `argv[1]`.
 *
 * The help string names the request message rather than listing fields:
 * fields are the library's business now, and `<message>` is the honest
 * summary of what the command accepts.
 */
#define ZLET_SHELL_SUBCMD_ENTRY(_type, _instance, _name, _req_lc, _req_uc, _resp_lc, _resp_uc,     \
				_shape)                                                            \
	SHELL_CMD_ARG(_name, NULL, "<text-format " #_req_lc ">",                                   \
		      zlet_shell_##_instance##_##_name##_cmd, 1, SHELL_OPT_ARG_RAW),

/* ----- Per-instance registration ----------------------------------------
 *
 * Invoked automatically by ZEPHLET_NEW(...)/ZEPHLET_NEW_PRIO(...) for
 * every instance, via the per-type `_ZLET_SHELL_HOOK_<type>` hook that
 * `zephlet_interface.h.jinja` chains into `_ZLET_FRONTEND_HOOKS_<type>`
 * when CONFIG_ZEPHLETS_SHELL=y — mirroring the CoAP frontend's own hook
 * (see ADR-0001). No app-level call needed.
 *
 * Registration into the `zlet` root command uses Zephyr's decentralized,
 * multi-file shell subcommand mechanism (SHELL_SUBCMD_SET_CREATE +
 * SHELL_SUBCMD_ADD — see zephyr/subsys/shell/modules/kernel_service/ for
 * the in-tree precedent), so no central manifest of instance names is
 * needed either: each instance's translation unit adds itself to the
 * `zlet` parent's iterable subcommand section independently. The root
 * itself is defined once, unconditionally, in zephlet_shell_root.c.
 *
 * `_ZLET_SHELL_METHODS_APPLY_<type>` is emitted by generate_zephlet.py
 * with `<type>` baked in literally (not pasted from `_type` here), so no
 * case-conversion trick is needed at the paste site below. */
#define ZLET_SHELL_INSTANCE(_type, _instance)                                                      \
	_ZLET_SHELL_METHODS_APPLY_##_type(_instance, ZLET_SHELL_DEFINE_METHOD)                     \
		SHELL_STATIC_SUBCMD_SET_CREATE(                                                    \
			_zlet_shell_subcmds_##_instance,                                           \
			_ZLET_SHELL_METHODS_APPLY_##_type(_instance, ZLET_SHELL_SUBCMD_ENTRY)      \
				SHELL_SUBCMD_SET_END);                                             \
	SHELL_SUBCMD_ADD((zlet), _instance, &_zlet_shell_subcmds_##_instance, "Zephlet instance",  \
			 NULL, 1, 0)

#endif /* ZEPHLET_SHELL_MACROS_H_ */
