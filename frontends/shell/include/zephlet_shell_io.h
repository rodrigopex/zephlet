/* Shell-side glue between the `zlet` frontend and nanopb-textformat:
 * a character sink that streams to a shell instance, one response
 * printer, and one error reporter so every RPC handler reports a parse
 * failure identically.
 */

#ifndef ZEPHLET_SHELL_IO_H_
#define ZEPHLET_SHELL_IO_H_

#include <nanopb_textformat.h>

#include <zephyr/shell/shell.h>

/* Brings PB_TF_DECLARE(lifecycle_status_t) — the shared zephlet.proto
 * descriptor, which start/stop/get_status responses print through. */
#include "zephlet_textformat.h"

/**
 * @brief cbprintf_cb that writes one character to a shell instance.
 *
 * Passed to the library's print functions with the `const struct shell *`
 * as @p ctx, so a response streams straight to the console with no
 * intermediate buffer — hence no output-size limit and no truncation case
 * to handle.
 *
 * @param c   character to emit.
 * @param ctx the `const struct shell *` to write to.
 *
 * @return @p c, as cbprintf_cb requires.
 */
int zlet_shell_out(int c, void *ctx);

/**
 * @brief Print @p msg on @p sh in the configured response form.
 *
 * Emits a readable indented block, the compact single line, or both
 * separated by a blank line, per the CONFIG_ZEPHLETS_SHELL_PRINT_* choice
 * and any later `zlet_fmt` override. Both forms are valid text format;
 * only the compact one can be pasted back, because the shell submits a
 * command on every newline.
 *
 * When the compact line is emitted it is prefixed with
 * `zlet <instance> <paste_rpc> `, making it a complete command that writes
 * the message back. @p paste_rpc is NULL for a response type no RPC in the
 * service accepts, and the line is then printed unprefixed rather than
 * skipped.
 *
 * @param sh        shell to print on.
 * @param tf        descriptor from PB_TF_DEFINE.
 * @param msg       the struct to print.
 * @param instance  instance name, as registered under the `zlet` root.
 * @param paste_rpc name of the RPC that accepts @p msg as its request, or
 *                  NULL when the service declares none.
 */
void zlet_shell_print_msg(const struct shell *sh, const struct pb_tf_msg *tf, const void *msg,
			  const char *instance, const char *paste_rpc);

/**
 * @brief Print the whole command as a fillable, copyable template.
 *
 * Emitted after a missing or unparseable request, so the shape is shown
 * exactly when it is needed. Carries the `zlet <instance> <rpc>` prefix for
 * the same reason a successful response's pasteable line does: the copied
 * line is then a complete command, with nothing left to retype.
 *
 * Deliberately not part of the command's help string. Help is word-wrapped
 * with a hanging indent, which puts real newlines into anything copied out
 * of it; ordinary output is never wrapped, so this stays one line.
 *
 * @param sh       shell to print on.
 * @param instance instance name, as registered under the `zlet` root.
 * @param rpc      the RPC that just failed, so the line re-runs it.
 * @param request  the request template codegen rendered for this RPC.
 */
void zlet_shell_print_template(const struct shell *sh, const char *instance, const char *rpc,
			       const char *request);

/**
 * @brief Report a text-format parse failure on @p sh.
 *
 * The library's strerror() names only the fault, so the offset and the
 * field name are added here. @p status->field is NULL when no field was in
 * scope — an error in the message's own syntax rather than in a value.
 *
 * @param sh     shell to report on.
 * @param rpc    RPC name, for context.
 * @param err    the failure, i.e. a negated @c enum pb_tf_err.
 * @param status where it happened.
 */
void zlet_shell_report_tf_err(const struct shell *sh, const char *rpc, int err,
			      const struct pb_tf_status *status);

#endif /* ZEPHLET_SHELL_IO_H_ */
