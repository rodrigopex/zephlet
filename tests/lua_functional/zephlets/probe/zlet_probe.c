#include "zlet_probe.h"

#include <errno.h>

#include <zephyr/kernel.h>

int probe_start_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	struct probe_data *d = z->data;

	d->is_running = true;
	if (resp != NULL) {
		resp->is_running = true;
		resp->is_ready = d->is_ready;
	}
	return 0;
}

int probe_get_status_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	struct probe_data *d = z->data;

	if (resp != NULL) {
		resp->is_running = d->is_running;
		resp->is_ready = d->is_ready;
	}
	return 0;
}

int probe_config_impl(const struct zephlet *z, const struct probe_config *req,
		      struct probe_config *resp)
{
	struct probe_config *cfg = z->config;

	*cfg = *req;
	if (resp != NULL) {
		*resp = *cfg;
	}
	return 0;
}

int probe_get_config_impl(const struct zephlet *z, struct probe_config *resp)
{
	if (resp != NULL) {
		*resp = *(struct probe_config *)z->config;
	}
	return 0;
}

int probe_burst_impl(const struct zephlet *z, const struct probe_burst *req)
{
	struct probe_data *d = z->data;

	for (uint32_t i = 0U; i < req->count; i++) {
		struct probe_events ev = {
			.seq = ++d->seq,
			.value = req->value,
			.mode = (probe_mode_t)(i % 3U),
			.alarm = (i % 2U) == 1U,
			.has_inner = true,
			.inner = {.a = (int32_t)i, .b = i * 2U},
		};
		int rc = probe_emit(z, &ev, K_MSEC(100));

		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

int probe_fail_impl(const struct zephlet *z, const struct probe_fail *req)
{
	ARG_UNUSED(z);
	return req->code;
}

int probe_ping_impl(const struct zephlet *z)
{
	ARG_UNUSED(z);
	return 0;
}

int probe_echo_impl(const struct zephlet *z, const struct probe_wide *req, struct probe_wide *resp)
{
	ARG_UNUSED(z);
	*resp = *req;
	return 0;
}

int probe_init_fn(const struct zephlet *z)
{
	struct probe_data *d = z->data;

	d->is_running = false;
	d->is_ready = true;
	d->seq = 0U;
	return 0;
}

#if defined(CONFIG_ZEPHLETS_LUA)
#include <zephyr/sys/iterable_sections.h>

#include <zephlet_lua.h>

int probe_stage_short_impl(const struct zephlet *z)
{
	static const uint8_t byte;

	STRUCT_SECTION_FOREACH(zephlet_lua_instance, inst) {
		if (inst->z == z) {
			zephlet_lua_stage(inst, &byte, sizeof(byte));
			return 0;
		}
	}
	return -ENODEV;
}
#endif
