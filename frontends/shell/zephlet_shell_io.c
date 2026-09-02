#include "zephlet_shell_io.h"

/**
 * @file
 * @brief Shell-side glue for the nanopb-textformat frontend.
 *
 * Replaces the hand-rolled value scanner this frontend used to carry: the
 * library owns every parse and print now, so all that is left is where the
 * characters go and how a failure is worded.
 */

int zlet_shell_out(int c, void *ctx)
{
	const struct shell *sh = ctx;

	/* shell_print() itself just appends "\n" to its format string, so a
	 * bare newline through this path gets the same transport handling —
	 * no manual carriage return belongs here. */
	shell_fprintf(sh, SHELL_NORMAL, "%c", (char)c);

	return c;
}

void zlet_shell_print_msg(const struct shell *sh, const struct pb_tf_msg *tf, const void *msg)
{
	(void)pb_tf_print_compact(tf, msg, zlet_shell_out, (void *)sh);

	/* Compact prints one line and no terminator of its own, so without
	 * this the response runs into the next prompt. Naming the style at the
	 * call site rather than reading a Kconfig choice is why this is now a
	 * plain statement: there is no other style to be wrong about. */
	shell_fprintf(sh, SHELL_NORMAL, "\n");
}

void zlet_shell_report_tf_err(const struct shell *sh, const char *rpc, int err,
			      const struct pb_tf_status *status)
{
	if (status->field != NULL) {
		shell_error(sh, "%s: at offset %u: %s in field '%s'", rpc, status->offset,
			    pb_tf_strerror(err), status->field);
	} else {
		shell_error(sh, "%s: at offset %u: %s", rpc, status->offset, pb_tf_strerror(err));
	}
}
