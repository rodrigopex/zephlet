#include <stdint.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/iterable_sections.h>

#include "zephlet.h"

/**
 * @file
 * @brief The `zlet_init` shell command: every instance with its
 * zephlet_init_result() and its dependency bindings, `slot -> instance`.
 *
 * A command of its own, beside `zlet`: a handler on the `zlet` root would
 * run in place of its help and of an rpc's usage.
 */

/* The instance @p p points at, found by address among the registered
 * instances, or NULL when it is not one: a rejected binding is never
 * dereferenced, as initialization never dereferences it. */
static const struct zephlet *registered(const struct zephlet *p)
{
	STRUCT_SECTION_FOREACH(zephlet, z) {
		if (z == p) {
			return z;
		}
	}
	return NULL;
}

static void print_slot(const struct shell *sh, const struct zephlet_dep_slot *slot,
		       const struct zephlet *p)
{
	const struct zephlet *inst = registered(p);

	if (p == NULL) {
		shell_print(sh, "  %s -> (unbound)", slot->name);
	} else if (inst == NULL) {
		shell_print(sh, "  %s -> (not an instance)", slot->name);
	} else if (inst->api != slot->api) {
		shell_print(sh, "  %s -> %s (another type)", slot->name, inst->name);
	} else {
		shell_print(sh, "  %s -> %s", slot->name, inst->name);
	}
}

static int cmd_zlet_init(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	STRUCT_SECTION_FOREACH(zephlet, z) {
		size_t num = (z->api != NULL && z->dependencies != NULL) ? z->api->num_deps : 0;

		shell_print(sh, "%s: init %d", z->name, zephlet_init_result(z));
		for (size_t d = 0; d < num; d++) {
			const struct zephlet_dep_slot *slot = &z->api->deps[d];
			const uint8_t *record = z->dependencies;

			print_slot(sh, slot,
				   *(const struct zephlet *const *)(record + slot->offset));
		}
	}
	return 0;
}

SHELL_CMD_REGISTER(zlet_init, NULL,
		   "List the zephlet instances, their init result and their dependency bindings.",
		   cmd_zlet_init);
