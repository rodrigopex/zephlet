#include "zephlet.h"

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
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

/* SYS_INIT walker: collect, sort by init_priority ascending, call init_fn. */

static int zephlet_init_walker(void)
{
	static const struct zephlet *ordered[CONFIG_ZEPHLET_MAX_INSTANCES];
	size_t count = 0;

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
		const struct zephlet *z = ordered[k];
		LOG_DBG("init '%s' (prio=%d)", z->name, z->init_priority);
		if (z->init_fn == NULL) {
			continue;
		}
		int err = z->init_fn(z);
		if (err != 0) {
			LOG_ERR("zephlet '%s' init_fn returned %d", z->name, err);
		}
	}
	return 0;
}

SYS_INIT(zephlet_init_walker, APPLICATION, 0);
