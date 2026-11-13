/*
 * `zlet_init` over the hand-built records of this suite, through the shell's
 * dummy backend. Built only for the shell scenario (CONFIG_ZEPHLETS_SHELL).
 *
 * Besides main.c's records (one bound to an instance of another type), two
 * more bindings the runtime rejects: an unbound slot, and a slot holding a
 * pointer that is no instance and cannot be read. Initialization skips both
 * with -ENODEV without following them; the command must not follow them
 * either, and must go on listing the instances after them.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include "zephlet.h"

extern const struct zephlet_api node_api;

struct node_dependencies {
	const struct zephlet *next;
};

static const struct node_dependencies unbound_deps = {.next = NULL};
ZEPHLET_INIT_DEFINE(shell_unbound, 20, 0);
const STRUCT_SECTION_ITERABLE(zephlet, shell_unbound) = {
	.name = "shell_unbound",
	.api = &node_api,
	.dependencies = &unbound_deps,
	.runtime = ZEPHLET_RUNTIME(shell_unbound),
};

/* Not an instance, and not readable on any of the suite's platforms. */
static const struct node_dependencies garbage_deps = {
	.next = (const struct zephlet *)0xdeadbeefU,
};
ZEPHLET_INIT_DEFINE(shell_garbage, 20, 0);
const STRUCT_SECTION_ITERABLE(zephlet, shell_garbage) = {
	.name = "shell_garbage",
	.api = &node_api,
	.dependencies = &garbage_deps,
	.runtime = ZEPHLET_RUNTIME(shell_garbage),
};

/* Sorts after both: the listing must still reach it. */
extern const struct zephlet cyc1_a;
static const struct node_dependencies valid_deps = {.next = &cyc1_a};
ZEPHLET_INIT_DEFINE(shell_zz_last, 20, 0);
const STRUCT_SECTION_ITERABLE(zephlet, shell_zz_last) = {
	.name = "shell_zz_last",
	.api = &node_api,
	.dependencies = &valid_deps,
	.runtime = ZEPHLET_RUNTIME(shell_zz_last),
};

static const char *zlet_init_output(void)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	size_t size;

	WAIT_FOR(shell_ready(sh), 20000, k_msleep(1));
	zassert_true(shell_ready(sh), "dummy shell backend never came up");
	shell_backend_dummy_clear_output(sh);
	zassert_ok(shell_execute_cmd(sh, "zlet_init"));
	return shell_backend_dummy_get_output(sh, &size);
}

ZTEST(deps_init_shell, test_zlet_init_renders_rejected_bindings_safely)
{
	zassert_equal(zephlet_init_result(&shell_unbound), -ENODEV);
	zassert_equal(zephlet_init_result(&shell_garbage), -ENODEV);

	const char *out = zlet_init_output();

	zassert_not_null(strstr(out, "shell_unbound: init -19"), "%s", out);
	zassert_not_null(strstr(out, "  next -> (unbound)"), "%s", out);
	zassert_not_null(strstr(out, "shell_garbage: init -19"), "%s", out);
	zassert_not_null(strstr(out, "  next -> (not an instance)"), "%s", out);
	zassert_not_null(strstr(out, "wrong_type: init -19"), "%s", out);
	zassert_not_null(strstr(out, "  next -> leaf_ok (another type)"), "%s", out);
	zassert_not_null(strstr(out, "shell_zz_last: init -19"), "later instances still listed");
	const char *valid = strstr(out, "  next -> cyc1_a");

	zassert_not_null(valid, "a valid binding: %s", out);
	zassert_true(valid[strlen("  next -> cyc1_a")] == '\r' ||
			     valid[strlen("  next -> cyc1_a")] == '\n',
		     "the plain name, nothing after it: %s", out);
}

ZTEST_SUITE(deps_init_shell, NULL, NULL, NULL, NULL, NULL);
