#include "zephlet.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>

LOG_MODULE_REGISTER(zephlet, CONFIG_ZEPHLET_LOG_LEVEL);

static int gate(const struct zephlet *z);

int zephlet_dispatch(const struct zephlet *z, struct zephlet_call *call)
{
	if (z == NULL || call == NULL) {
		return -EINVAL;
	}

	if (z->api == NULL || z->api->methods == NULL) {
		call->return_code = -ENOSYS;
		return 0;
	}

	/* Not initialized yet (-EAGAIN), or skipped or failed (-ENODEV): its
	 * handlers would reach state init_fn never set up, or a binding
	 * initialization rejected. */
	int err = gate(z);

	if (err != 0) {
		call->return_code = err;
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

/* ----- Initialization ------------------------------------------------
 *
 * Every ZEPHLET_NEW instance has its own SYS_INIT entry
 * (ZEPHLET_INIT_ENTRY), at APPLICATION priority CONFIG_ZEPHLET_INIT_PRIORITY
 * + its type's init rank, which the dependency-graph build step computes.
 * A provider type ranks below each of its consumer types, so the kernel's
 * init sequence runs every provider first. Each entry calls
 * zephlet_init_one(), which checks the instance's slots and providers and
 * runs its init_fn once.
 *
 * Generated types cannot form a cycle (the build rejects one between
 * types). Hand-written records that do, or that rank a consumer before its
 * provider, find a provider not initialized yet and are skipped with
 * -ENODEV.
 *
 * Each instance points at its runtime record (struct zephlet::runtime).
 * The instance section is only scanned to check that a provider pointer is
 * an instance. No heap, no recursion. */

/* Flags in struct zephlet_runtime; a record starts zeroed. */
#define ZLET_RT_FINAL   BIT(0) /* initialized or skipped */
#define ZLET_RT_SERVING BIT(1) /* init_fn is running or returned 0 */

/* The post-link check (codegen/check_deps_elf.py) reads these structs
 * from the ELF as pointer-sized fields; update it with any change here. */
BUILD_ASSERT(sizeof(size_t) == sizeof(void *));
BUILD_ASSERT(offsetof(struct zephlet, api) == 1 * sizeof(void *));
BUILD_ASSERT(offsetof(struct zephlet, dependencies) == 6 * sizeof(void *));
BUILD_ASSERT(offsetof(struct zephlet, runtime) == 8 * sizeof(void *));
BUILD_ASSERT(sizeof(struct zephlet) == 10 * sizeof(void *));
BUILD_ASSERT(offsetof(struct zephlet_api, deps) == 2 * sizeof(void *));
BUILD_ASSERT(offsetof(struct zephlet_api, num_deps) == 3 * sizeof(void *));
BUILD_ASSERT(offsetof(struct zephlet_dep_slot, api) == 2 * sizeof(void *));
BUILD_ASSERT(offsetof(struct zephlet_dep_slot, offset) == 3 * sizeof(void *));
BUILD_ASSERT(sizeof(struct zephlet_dep_slot) == 4 * sizeof(void *));

/* Whether @p z is an instance of the section, compared by address. Only
 * for provider pointers of hand-written records, at init time. */
static bool is_instance(const struct zephlet *z)
{
	STRUCT_SECTION_FOREACH(zephlet, inst) {
		if (inst == z) {
			return true;
		}
	}
	return false;
}

static size_t num_deps(const struct zephlet *z)
{
	return (z->api != NULL) ? z->api->num_deps : 0;
}

static const struct zephlet *dep_at(const struct zephlet *z, size_t slot)
{
	const uint8_t *record = z->dependencies;

	return *(const struct zephlet *const *)(record + z->api->deps[slot].offset);
}

/* The instance bound to slot @p d of @p z, or NULL when the slot is
 * invalid: unbound, not an instance with a runtime record, or another
 * type. @p z has a dependency record. */
static const struct zephlet *provider_of(const struct zephlet *z, size_t d)
{
	const struct zephlet *p = dep_at(z, d);

	if (p == NULL || !is_instance(p) || p->runtime == NULL) {
		return NULL;
	}
	return (p->api == z->api->deps[d].api) ? p : NULL;
}

/* Log every invalid slot of @p z; return whether there was one. */
static bool binding_is_bad(const struct zephlet *z)
{
	bool bad = false;

	if (num_deps(z) == 0) {
		return false;
	}
	if (z->dependencies == NULL) {
		LOG_ERR("zephlet '%s' has no dependency record", z->name);
		return true;
	}
	for (size_t d = 0; d < z->api->num_deps; d++) {
		const struct zephlet_dep_slot *slot = &z->api->deps[d];
		const struct zephlet *p = dep_at(z, d);

		if (provider_of(z, d) != NULL) {
			continue;
		}
		bad = true;
		if (p == NULL) {
			LOG_ERR("zephlet '%s': dependency '%s' (a %s) is not bound", z->name,
				slot->name, slot->type);
		} else if (!is_instance(p) || p->runtime == NULL) {
			LOG_ERR("zephlet '%s': dependency '%s' is not bound to a zephlet instance",
				z->name, slot->name);
		} else {
			LOG_ERR("zephlet '%s': dependency '%s' needs a %s, got '%s'", z->name,
				slot->name, slot->type, p->name);
		}
	}
	return bad;
}

/* Whether a provider of @p z, whose slots are all valid, did not
 * initialize; logs the first one. */
static bool provider_is_down(const struct zephlet *z)
{
	for (size_t d = 0; d < num_deps(z); d++) {
		const struct zephlet *p = provider_of(z, d);
		const struct zephlet_runtime *r = p->runtime;

		if ((r->flags & ZLET_RT_FINAL) == 0) {
			LOG_ERR("zephlet '%s' not initialized: provider '%s' (dependency '%s') "
				"is not initialized yet; it must rank below its consumer",
				z->name, p->name, z->api->deps[d].name);
			return true;
		}
		if (r->result != 0) {
			LOG_ERR("zephlet '%s' not initialized: provider '%s' (dependency '%s') "
				"is unavailable (%d)",
				z->name, p->name, z->api->deps[d].name, r->result);
			return true;
		}
	}
	return false;
}

int zephlet_init_one(const struct zephlet *z)
{
	if (z == NULL || z->runtime == NULL) {
		return -ENOENT;
	}

	struct zephlet_runtime *r = z->runtime;

	if ((r->flags & (ZLET_RT_FINAL | ZLET_RT_SERVING)) != 0) {
		/* Done, or init_fn is running and calls back in. */
		return ((r->flags & ZLET_RT_FINAL) != 0) ? r->result : -EAGAIN;
	}
	if (binding_is_bad(z) || provider_is_down(z)) {
		r->result = -ENODEV;
		r->flags = ZLET_RT_FINAL;
		return r->result;
	}

	LOG_DBG("init '%s' (sub-priority %d)", z->name, z->init_priority);
	/* Serving while init_fn runs, so it may call its own RPCs. */
	r->flags = ZLET_RT_SERVING;
	r->result = (z->init_fn != NULL) ? z->init_fn(z) : 0;
	if (r->result != 0) {
		LOG_ERR("zephlet '%s' init_fn returned %d", z->name, r->result);
		r->flags = ZLET_RT_FINAL;
	} else {
		r->flags = ZLET_RT_FINAL | ZLET_RT_SERVING;
	}
	return r->result;
}

int zephlet_init_result(const struct zephlet *z)
{
	if (z == NULL || z->runtime == NULL) {
		return -ENOENT;
	}
	return ((z->runtime->flags & ZLET_RT_FINAL) != 0) ? z->runtime->result : -EAGAIN;
}

/* 0 when @p z may serve calls, else the call's return code: -EAGAIN before
 * its init_fn ran, -ENODEV once skipped or failed. O(1): a call's hot
 * path. */
static int gate(const struct zephlet *z)
{
	const struct zephlet_runtime *r = z->runtime;

	if (r == NULL) {
		return -EAGAIN;
	}
	if ((r->flags & ZLET_RT_SERVING) != 0) {
		return 0;
	}
	return ((r->flags & ZLET_RT_FINAL) != 0) ? -ENODEV : -EAGAIN;
}

/* After zbus: an init_fn may publish to a provider's command channel. Init
 * entries sit at this base plus their rank, so all of them come after. */
BUILD_ASSERT(CONFIG_ZEPHLET_INIT_PRIORITY > CONFIG_ZBUS_CHANNELS_SYS_INIT_PRIORITY,
	     "CONFIG_ZEPHLET_INIT_PRIORITY must be greater than "
	     "CONFIG_ZBUS_CHANNELS_SYS_INIT_PRIORITY");
