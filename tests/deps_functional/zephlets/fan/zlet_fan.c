#include "zlet_fan.h"

#include <errno.h>

int fan_init_fn(const struct zephlet *z)
{
	struct fan_data *d = z->data;

	d->is_ready = true;
	return 0;
}

int fan_set_speed_impl(const struct zephlet *z, const struct fan_speed *req)
{
	struct fan_data *d = z->data;
	const struct fan_config *cfg = z->config;

	if (!d->is_ready) {
		return -ENODEV;
	}
	d->percent = MIN(req->percent, cfg->max_percent);
	d->sets++;
	return 0;
}

int fan_get_speed_impl(const struct zephlet *z, struct fan_speed *resp)
{
	const struct fan_data *d = z->data;

	if (resp != NULL) {
		resp->percent = d->percent;
	}
	return 0;
}

int fan_get_status_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	const struct fan_data *d = z->data;

	if (resp != NULL) {
		resp->is_running = d->is_running;
		resp->is_ready = d->is_ready;
	}
	return 0;
}
