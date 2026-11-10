#include "zlet_temperature.h"

#include <errno.h>

int temperature_init_fn(const struct zephlet *z)
{
	struct temperature_data *d = z->data;

	d->is_ready = true;
	return 0;
}

int temperature_read_impl(const struct zephlet *z, struct temperature_reading *resp)
{
	struct temperature_data *d = z->data;
	const struct temperature_config *cfg = z->config;

	if (!d->is_ready) {
		return -ENODEV;
	}
	d->reads++;
	if (resp != NULL) {
		resp->celsius = d->celsius + cfg->offset_c;
	}
	return 0;
}

int temperature_set_impl(const struct zephlet *z, const struct temperature_reading *req)
{
	struct temperature_data *d = z->data;

	d->celsius = req->celsius;
	return 0;
}

int temperature_get_status_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	const struct temperature_data *d = z->data;

	if (resp != NULL) {
		resp->is_running = d->is_running;
		resp->is_ready = d->is_ready;
	}
	return 0;
}
