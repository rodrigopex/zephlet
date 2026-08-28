#include "zlet_ui.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zlet_ui, CONFIG_ZEPHLET_UI_LOG_LEVEL);

int ui_start_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	struct ui_data *d = z->data;

	if (!d->is_ready) {
		return -ENODEV;
	}
	if (d->is_running) {
		return -EALREADY;
	}
	d->is_running = true;
	if (resp != NULL) {
		resp->is_running = true;
		resp->is_ready = true;
	}
	return 0;
}

int ui_get_status_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	struct ui_data *d = z->data;

	if (resp != NULL) {
		resp->is_running = d->is_running;
		resp->is_ready = d->is_ready;
	}
	return 0;
}

int ui_config_impl(const struct zephlet *z, const struct ui_config *req, struct ui_config *resp)
{
	struct ui_config *cfg = z->config;

	*cfg = *req;
	if (resp != NULL) {
		*resp = *cfg;
	}
	return 0;
}

int ui_get_config_impl(const struct zephlet *z, struct ui_config *resp)
{
	struct ui_config *cfg = z->config;

	if (resp != NULL) {
		*resp = *cfg;
	}
	return 0;
}

int ui_init_fn(const struct zephlet *z)
{
	struct ui_data *d = z->data;

	d->is_running = false;
	d->is_ready = true;
	return 0;
}
