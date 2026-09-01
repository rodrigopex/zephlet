#include "zephlet.pb.h"

#include "zephlet_shell_io.h"

/**
 * @file
 * @brief Shell-side glue for the nanopb-textformat frontend.
 *
 * Replaces the hand-rolled value scanner this frontend used to carry: the
 * library owns every parse and print now, so all that is left is where the
 * characters go and how a failure is worded.
 */

/* Text-format descriptor for the one shared zephlet.proto message that
 * reaches the shell: Lifecycle.Status, the response of start/stop/
 * get_status on every zephlet. Defined here rather than by codegen
 * because codegen runs once per zephlet and would emit a duplicate
 * symbol for each; the shared proto belongs to the infra, so the infra
 * defines it once. `Empty` has no fields and needs no descriptor. */
PB_TF_DEFINE(LIFECYCLE_STATUS, lifecycle_status_t);

int zlet_shell_out(int c, void *ctx)
{
	const struct shell *sh = ctx;

	/* shell_print() itself just appends "\n" to its format string, so a
	 * bare newline through this path gets the same transport handling —
	 * no manual carriage return belongs here. */
	shell_fprintf(sh, SHELL_NORMAL, "%c", (char)c);

	return c;
}

void zlet_shell_report_tf_err(const struct shell *sh, const char *rpc, enum pb_tf_err err,
			      const struct pb_tf_status *status)
{
	if (status->field != NULL) {
		shell_error(sh, "%s: at offset %u: %s in field '%s'", rpc, status->offset,
			    pb_tf_strerror(err), status->field);
	} else {
		shell_error(sh, "%s: at offset %u: %s", rpc, status->offset, pb_tf_strerror(err));
	}
}
