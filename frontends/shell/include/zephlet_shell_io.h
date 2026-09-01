/* Shell-side glue between the `zlet` frontend and nanopb-textformat:
 * a character sink that streams to a shell instance, and one error
 * reporter so every RPC handler reports a parse failure identically.
 */

#ifndef ZEPHLET_SHELL_IO_H_
#define ZEPHLET_SHELL_IO_H_

#include <nanopb_textformat.h>

#include <zephyr/shell/shell.h>

/* Shared zephlet.proto descriptor, defined in zephlet_shell_io.c — see
 * there for why it is not emitted by codegen. */
PB_TF_DECLARE(lifecycle_status_t);

/**
 * @brief cbprintf_cb that writes one character to a shell instance.
 *
 * Passed to pb_tf_print() with the `const struct shell *` as @p ctx, so a
 * response streams straight to the console with no intermediate buffer —
 * hence no output-size limit and no truncation case to handle. The
 * multi-line printer terminates every field with a newline, including the
 * last, so a caller adds none of its own.
 *
 * @param c   character to emit.
 * @param ctx the `const struct shell *` to write to.
 *
 * @return @p c, as cbprintf_cb requires.
 */
int zlet_shell_out(int c, void *ctx);

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
void zlet_shell_report_tf_err(const struct shell *sh, const char *rpc, enum pb_tf_err err,
			      const struct pb_tf_status *status);

#endif /* ZEPHLET_SHELL_IO_H_ */
