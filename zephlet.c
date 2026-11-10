#include "zephlet.h"

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/iterable_sections.h>

LOG_MODULE_REGISTER(zephlet, CONFIG_ZEPHLET_LOG_LEVEL);

int zephlet_dispatch(const struct zephlet *z, struct zephlet_call *call)
{
	if (z == NULL || call == NULL) {
		return -EINVAL;
	}

	if (z->api == NULL || z->api->methods == NULL) {
		call->return_code = -ENOSYS;
		return 0;
	}

	if ((size_t)call->method_id >= z->api->num_methods) {
		call->return_code = -ENOSYS;
		return 0;
	}

	const struct zephlet_method *m = &z->api->methods[call->method_id];

	if (m->handler == NULL) {
		call->return_code = -ENOSYS;
		return 0;
	}

	call->return_code = m->handler(z, call);
	return 0;
}

const struct zephlet *zephlet_get_by_name(const char *name)
{
	if (name == NULL) {
		return NULL;
	}

	STRUCT_SECTION_FOREACH(zephlet, z) {
		if (strcmp(z->name, name) == 0) {
			return z;
		}
	}
	return NULL;
}

/* ----- SYS_INIT walker ------------------------------------------------
 *
 * Collects every instance, sorts them by init_priority (stable), checks each
 * dependency binding, rejects dependency cycles, then repeatedly runs the
 * first instance in that order whose providers have all initialized. An
 * instance whose binding is bad, which is on a cycle, or one of whose
 * providers failed or was skipped is never initialized. Every array is
 * bounded by CONFIG_ZEPHLET_MAX_INSTANCES; no heap, no recursion. */

enum zephlet_init_state {
	ZLET_INIT_PENDING,
	ZLET_INIT_DONE,
	ZLET_INIT_SKIPPED,
};

/* Instances in init order, and how the walker left each one. */
static const struct zephlet *ordered[CONFIG_ZEPHLET_MAX_INSTANCES];
static uint8_t state[CONFIG_ZEPHLET_MAX_INSTANCES];
static int result[CONFIG_ZEPHLET_MAX_INSTANCES];
static size_t count;
static bool walked;

/* Longest line a dependency cycle prints as, `a -> b -> a`; a longer one is
 * cut short with `...`. */
#define ZLET_CYCLE_LINE_MAX 128

static size_t index_of(const struct zephlet *z)
{
	for (size_t k = 0; k < count; k++) {
		if (ordered[k] == z) {
			return k;
		}
	}
	return count;
}

static const struct zephlet *dep_at(const struct zephlet *z, size_t slot)
{
	const uint8_t *record = z->dependencies;

	return *(const struct zephlet *const *)(record + z->api->deps[slot].offset);
}

static size_t num_deps(const struct zephlet *z)
{
	return (z->api != NULL) ? z->api->num_deps : 0;
}

static void skip(size_t k, int err)
{
	state[k] = ZLET_INIT_SKIPPED;
	result[k] = err;
}

/* Check every slot of instance @p k; skip it, with the reason, if any is
 * unbound, bound to an instance of another type, or bound to an instance
 * the walker does not hold. */
static void check_binding(size_t k)
{
	const struct zephlet *z = ordered[k];

	if (num_deps(z) == 0) {
		return;
	}
	if (z->dependencies == NULL) {
		LOG_ERR("zephlet '%s' has no dependency record", z->name);
		skip(k, -ENODEV);
		return;
	}
	for (size_t d = 0; d < z->api->num_deps; d++) {
		const struct zephlet_dep_slot *slot = &z->api->deps[d];
		const struct zephlet *p = dep_at(z, d);

		if (p == NULL) {
			LOG_ERR("zephlet '%s': dependency '%s' (a %s) is not bound", z->name,
				slot->name, slot->type);
			skip(k, -ENODEV);
			return;
		}
		if (p->api != slot->api) {
			LOG_ERR("zephlet '%s': dependency '%s' needs a %s instance; '%s' is not one",
				z->name, slot->name, slot->type, p->name);
			skip(k, -ENODEV);
			return;
		}
		if (index_of(p) == count) {
			LOG_ERR("zephlet '%s': dependency '%s' is bound to '%s', which is beyond "
				"CONFIG_ZEPHLET_MAX_INSTANCES",
				z->name, slot->name, p->name);
			skip(k, -ENODEV);
			return;
		}
	}
}

/* Log the cycle that runs from path[from] to the top of @p path and back to
 * path[from]. */
static void log_cycle(const size_t *path, size_t from, size_t depth)
{
	char line[ZLET_CYCLE_LINE_MAX];
	size_t len = 0;

	for (size_t i = from; i <= depth && len < sizeof(line); i++) {
		const char *name = ordered[path[(i < depth) ? i : from]]->name;
		int n = snprintk(line + len, sizeof(line) - len, "%s%s", (i == from) ? "" : " -> ",
				 name);

		if (n < 0) {
			break;
		}
		len += (size_t)n;
	}
	if (len >= sizeof(line)) {
		strcpy(&line[sizeof(line) - 4], "...");
	}
	LOG_ERR("zephlet dependency cycle: %s", line);
}

/* Skip every instance on a dependency cycle, with -ELOOP. Iterative DFS
 * over the bindings check_binding() accepted, colouring an instance grey
 * while it is on the current path. */
static void reject_cycles(void)
{
	enum {
		WHITE,
		GREY,
		BLACK
	};
	static uint8_t colour[CONFIG_ZEPHLET_MAX_INSTANCES];
	static size_t path[CONFIG_ZEPHLET_MAX_INSTANCES];
	static size_t next[CONFIG_ZEPHLET_MAX_INSTANCES];
	/* Cycle members found, skipped only after the walk so that every cycle
	 * through them is still followed and logged. */
	static bool on_cycle[CONFIG_ZEPHLET_MAX_INSTANCES];

	memset(colour, WHITE, sizeof(colour));
	memset(on_cycle, 0, sizeof(on_cycle));

	for (size_t root = 0; root < count; root++) {
		size_t depth = 0;

		if (colour[root] != WHITE || state[root] == ZLET_INIT_SKIPPED) {
			continue;
		}
		colour[root] = GREY;
		path[0] = root;
		next[0] = 0;

		while (true) {
			size_t k = path[depth];
			const struct zephlet *z = ordered[k];

			if (next[depth] >= num_deps(z)) {
				colour[k] = BLACK;
				if (depth == 0) {
					break;
				}
				depth--;
				continue;
			}

			size_t p = index_of(dep_at(z, next[depth]++));

			if (state[p] == ZLET_INIT_SKIPPED) {
				continue;
			}
			if (colour[p] == GREY) {
				size_t from = 0;

				while (path[from] != p) {
					from++;
				}
				log_cycle(path, from, depth + 1);
				for (size_t i = from; i <= depth; i++) {
					on_cycle[path[i]] = true;
				}
				continue;
			}
			if (colour[p] == WHITE) {
				colour[p] = GREY;
				depth++;
				path[depth] = p;
				next[depth] = 0;
			}
		}
	}

	for (size_t k = 0; k < count; k++) {
		if (on_cycle[k]) {
			skip(k, -ELOOP);
		}
	}
}

/* Skip the pending instance @p k if one of its providers was not
 * initialized; return whether it did. */
static bool skip_if_provider_unavailable(size_t k)
{
	const struct zephlet *z = ordered[k];

	for (size_t d = 0; d < num_deps(z); d++) {
		size_t p = index_of(dep_at(z, d));

		if (state[p] != ZLET_INIT_PENDING && result[p] != 0) {
			LOG_ERR("zephlet '%s' not initialized: provider '%s' (dependency '%s') "
				"is unavailable (%d)",
				z->name, ordered[p]->name, z->api->deps[d].name, result[p]);
			skip(k, -ENODEV);
			return true;
		}
	}
	return false;
}

static bool providers_done(const struct zephlet *z)
{
	for (size_t d = 0; d < num_deps(z); d++) {
		if (state[index_of(dep_at(z, d))] == ZLET_INIT_PENDING) {
			return false;
		}
	}
	return true;
}

/* The first pending instance in init order whose providers have all
 * initialized, or count when none is left. Skips first, until nothing
 * changes, every pending instance one of whose providers was not
 * initialized: a skip can cascade to a consumer earlier in the order. */
static size_t next_ready(void)
{
	bool skipped;

	do {
		skipped = false;
		for (size_t k = 0; k < count; k++) {
			if (state[k] == ZLET_INIT_PENDING && skip_if_provider_unavailable(k)) {
				skipped = true;
			}
		}
	} while (skipped);

	for (size_t k = 0; k < count; k++) {
		if (state[k] == ZLET_INIT_PENDING && providers_done(ordered[k])) {
			return k;
		}
	}
	return count;
}

static int zephlet_init_walker(void)
{
	count = 0;
	STRUCT_SECTION_FOREACH(zephlet, z) {
		if (count >= CONFIG_ZEPHLET_MAX_INSTANCES) {
			LOG_ERR("zephlet count exceeds CONFIG_ZEPHLET_MAX_INSTANCES=%d; "
				"skipping '%s' and later instances",
				CONFIG_ZEPHLET_MAX_INSTANCES, z->name);
			break;
		}
		ordered[count++] = z;
	}

	/* Stable insertion sort by init_priority. Count is small (bounded by
	 * CONFIG_ZEPHLET_MAX_INSTANCES); O(n^2) is fine. */
	for (size_t k = 1; k < count; k++) {
		const struct zephlet *cur = ordered[k];
		size_t j = k;
		while (j > 0 && ordered[j - 1]->init_priority > cur->init_priority) {
			ordered[j] = ordered[j - 1];
			j--;
		}
		ordered[j] = cur;
	}

	for (size_t k = 0; k < count; k++) {
		state[k] = ZLET_INIT_PENDING;
		result[k] = -EAGAIN;
	}
	walked = true;

	for (size_t k = 0; k < count; k++) {
		check_binding(k);
	}
	reject_cycles();

	for (size_t k = next_ready(); k < count; k = next_ready()) {
		const struct zephlet *z = ordered[k];

		LOG_DBG("init '%s' (prio=%d)", z->name, z->init_priority);
		result[k] = (z->init_fn != NULL) ? z->init_fn(z) : 0;
		state[k] = ZLET_INIT_DONE;
		if (result[k] != 0) {
			LOG_ERR("zephlet '%s' init_fn returned %d", z->name, result[k]);
		}
	}

	for (size_t k = 0; k < count; k++) {
		__ASSERT(result[k] != -ELOOP, "zephlet '%s' is on a dependency cycle",
			 ordered[k]->name);
	}
	return 0;
}

int zephlet_init_result(const struct zephlet *z)
{
	if (z == NULL) {
		return -ENOENT;
	}
	if (!walked) {
		return -EAGAIN;
	}

	size_t k = index_of(z);

	return (k < count) ? result[k] : -ENOENT;
}

SYS_INIT(zephlet_init_walker, APPLICATION, 0);
