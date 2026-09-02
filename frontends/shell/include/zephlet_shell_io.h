/* Shell-side glue between the `zlet` frontend and nanopb-textformat:
 * a character sink that streams to a shell instance, and one error
 * reporter so every RPC handler reports a parse failure identically.
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
 * Passed to pb_tf_print_compact() with the `const struct shell *` as @p ctx,
 * so a response streams straight to the console with no intermediate buffer
 * — hence no output-size limit and no truncation case to handle. Compact
 * carries no terminator of its own; zlet_shell_print_msg() adds it.
 *
 * @param c   character to emit.
 * @param ctx the `const struct shell *` to write to.
 *
 * @return @p c, as cbprintf_cb requires.
 */
int zlet_shell_out(int c, void *ctx);

/**
 * @brief Print @p msg on @p sh as one terminated line.
 *
 * Prints compact -- one line, the shortest form that is still valid
 * standalone text format, so a `get_config` response can be pasted straight
 * back into a `config` call. Compact emits no terminator of its own, so
 * this adds one; keeping that here rather than in the generated handler
 * macros means there is one place to be wrong.
 *
 * @param err a return value from the library, i.e. 0 or a negated
 *            @c enum pb_tf_err.
 *
 * @param sh  shell to print on.
 * @param tf  descriptor from PB_TF_DEFINE.
 * @param msg the struct to print.
 */
void zlet_shell_print_msg(const struct shell *sh, const struct pb_tf_msg *tf, const void *msg);

/**
 * @brief Report a text-format parse failure on @p sh.
 *
 * The library's strerror() names only the fault, so the offset and the
 * field name are added here. @p status->field is NULL when no field was in
 * scope — an error in the message's own syntax rather than in a value.
 *
 * @param sh     shell to report on.
 * @param rpc    RPC name, for context.
 * @param err    the failure.
 * @param status where it happened.
 */
void zlet_shell_report_tf_err(const struct shell *sh, const char *rpc, int err,
			      const struct pb_tf_status *status);

#endif /* ZEPHLET_SHELL_IO_H_ */
