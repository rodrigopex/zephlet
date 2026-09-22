#include "zephlet_shell_io.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

/**
 * @file
 * @brief Shell-side glue for the nanopb-textformat frontend.
 *
 * Replaces the hand-rolled value scanner this frontend used to carry: the
 * library owns every parse and print now, so all that is left is where the
 * characters go, which response forms are emitted, and how a failure is
 * worded.
 */

/** Which of the two response forms zlet_shell_print_msg() emits. */
enum zlet_shell_form {
	/** Readable block, a blank line, then the pasteable command line. */
	ZLET_SHELL_FORM_BOTH,
	/** Readable block only. */
	ZLET_SHELL_FORM_PRETTY,
	/** Compact line only. */
	ZLET_SHELL_FORM_COMPACT,
};

#if defined(CONFIG_ZEPHLETS_SHELL_PRINT_BOTH)
#define ZLET_SHELL_FORM_DEFAULT ZLET_SHELL_FORM_BOTH
#elif defined(CONFIG_ZEPHLETS_SHELL_PRINT_PRETTY)
#define ZLET_SHELL_FORM_DEFAULT ZLET_SHELL_FORM_PRETTY
#else
#define ZLET_SHELL_FORM_DEFAULT ZLET_SHELL_FORM_COMPACT
#endif

/*
 * One form for every instance and every shell backend. Keying this on the
 * shell pointer would buy a case this frontend does not serve -- two consoles
 * reading the same target at once -- at the cost of a lookup on every
 * response.
 *
 * `const` when the runtime command is not compiled in, so the comparisons
 * below fold to constants and the unreachable print path is dropped by the
 * linker rather than carried.
 */
#if defined(CONFIG_ZEPHLETS_SHELL_PRINT_RUNTIME)
static enum zlet_shell_form zlet_shell_selected = ZLET_SHELL_FORM_DEFAULT;
#else
static const enum zlet_shell_form zlet_shell_selected = ZLET_SHELL_FORM_DEFAULT;
#endif

int zlet_shell_out(int c, void *ctx)
{
	const struct shell *sh = ctx;

	/* shell_print() itself just appends "\n" to its format string, so a
	 * bare newline through this path gets the same transport handling —
	 * no manual carriage return belongs here. */
	shell_fprintf(sh, SHELL_NORMAL, "%c", (char)c);

	return c;
}

void zlet_shell_print_msg(const struct shell *sh, const struct pb_tf_msg *tf, const void *msg,
			  const char *instance, const char *paste_rpc)
{
	if (zlet_shell_selected != ZLET_SHELL_FORM_COMPACT) {
		/* Multi-line terminates every field including the last, so the
		 * block needs no terminator of its own. */
		(void)pb_tf_print_multiline(tf, msg, zlet_shell_out, (void *)sh);
	}

	if (zlet_shell_selected == ZLET_SHELL_FORM_PRETTY) {
		return;
	}

	if (zlet_shell_selected == ZLET_SHELL_FORM_BOTH) {
		/* Separate the two forms, so a reader scrolling back cannot
		 * mistake the command line for more of the message. */
		shell_fprintf(sh, SHELL_NORMAL, "\n");
	}

	/*
	 * The prefix is what makes the line pasteable, but only a response
	 * whose type some RPC accepts has a command to name -- nothing takes
	 * the Lifecycle.Status returned by start, stop and get_status. Such a
	 * response still prints its message, unprefixed, so the compact-only
	 * form never emits nothing at all.
	 */
	if (paste_rpc != NULL) {
		shell_fprintf(sh, SHELL_NORMAL, "zlet %s %s ", instance, paste_rpc);
	}

	(void)pb_tf_print_compact(tf, msg, zlet_shell_out, (void *)sh);

	/* Compact carries no terminator of its own, so without this the
	 * response runs into the next prompt. */
	shell_fprintf(sh, SHELL_NORMAL, "\n");
}

void zlet_shell_print_template(const struct shell *sh, const char *instance, const char *rpc,
			       const char *request)
{
	/*
	 * The whole command, not just the message, so the line is complete in
	 * the same way a successful response's pasteable line is -- copy it,
	 * replace the placeholders, run it. Nothing to retype.
	 *
	 * Printed through shell_print() rather than carried in the command's
	 * help string, because help goes through formatted_text_print(), which
	 * wraps long text with a hanging indent -- emitting real newlines and
	 * indent spaces that a copy then picks up, so a wrapped help line
	 * cannot be pasted back. Ordinary output is not wrapped by the shell at
	 * all, so however long this is it stays one logical line that the
	 * terminal soft-wraps and the clipboard keeps intact.
	 *
	 * The label gets its own line for the same reason: on the template's
	 * line, selecting the line would copy the label with it.
	 */
	shell_print(sh, "");
	shell_print(sh, "Tip: copy the line below and replace each <...>");
	shell_print(sh, "zlet %s %s %s", instance, rpc, request);
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

#if defined(CONFIG_ZEPHLETS_SHELL_PRINT_RUNTIME)

/* Indexed by enum zlet_shell_form, so the command parses and reports through
 * one table rather than two parallel switches. */
static const char *const zlet_shell_form_names[] = {
	[ZLET_SHELL_FORM_BOTH] = "both",
	[ZLET_SHELL_FORM_PRETTY] = "pretty",
	[ZLET_SHELL_FORM_COMPACT] = "compact",
};

static int zlet_shell_fmt_cmd(const struct shell *sh, size_t argc, char **argv)
{
	size_t i;

	if (argc < 2) {
		shell_print(sh, "%s", zlet_shell_form_names[zlet_shell_selected]);
		return 0;
	}

	for (i = 0U; i < ARRAY_SIZE(zlet_shell_form_names); i++) {
		if (strcmp(argv[1], zlet_shell_form_names[i]) == 0) {
			zlet_shell_selected = (enum zlet_shell_form)i;
			return 0;
		}
	}

	shell_error(sh, "expected one of: both, pretty, compact");

	return -EINVAL;
}

/*
 * A root command of its own rather than a child of `zlet`, whose subcommands
 * are the instance names: every instance adds itself under that parent, so a
 * control command sharing the namespace would be shadowed by an instance that
 * happened to be named the same.
 */
SHELL_CMD_ARG_REGISTER(zlet_fmt, NULL, "Response form for zlet commands: both|pretty|compact.",
		       zlet_shell_fmt_cmd, 1, 1);

#endif /* CONFIG_ZEPHLETS_SHELL_PRINT_RUNTIME */
