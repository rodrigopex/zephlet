#include "zlet_tick.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zlet_tick, CONFIG_ZEPHLET_TICK_LOG_LEVEL);

static void tick_timer_handler(struct k_timer *timer_id)
{
	const struct zephlet *z = k_timer_user_data_get(timer_id);
	struct tick_events ev = {
		.timestamp = (int32_t)k_uptime_get(),
	};

	(void)tick_emit(z, &ev, K_NO_WAIT);
}

static int validate_config(const struct tick_config *c)
{
	if (c->period_ms == 0 || c->duration_ms == 0) {
		return -EINVAL;
	}
	return 0;
}

int tick_start_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	struct tick_data *d = z->data;
	struct tick_config *cfg = z->config;

	if (!d->is_ready) {
		if (resp != NULL) {
			resp->is_running = false;
			resp->is_ready = false;
		}
		return -ENODEV;
	}
	if (d->is_running) {
		if (resp != NULL) {
			resp->is_running = true;
			resp->is_ready = true;
		}
		return -EALREADY;
	}

	k_timer_start(&d->timer, K_MSEC(cfg->duration_ms), K_MSEC(cfg->period_ms));
	d->is_running = true;

	if (resp != NULL) {
		resp->is_running = true;
		resp->is_ready = true;
	}
	return 0;
}

int tick_stop_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	struct tick_data *d = z->data;

	if (!d->is_running) {
		if (resp != NULL) {
			resp->is_running = false;
			resp->is_ready = d->is_ready;
		}
		return -EALREADY;
	}

	k_timer_stop(&d->timer);
	d->is_running = false;

	if (resp != NULL) {
		resp->is_running = false;
		resp->is_ready = d->is_ready;
	}
	return 0;
}

int tick_get_status_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	struct tick_data *d = z->data;

	if (resp != NULL) {
		resp->is_running = d->is_running;
		resp->is_ready = d->is_ready;
	}
	return 0;
}

int tick_config_impl(const struct zephlet *z, const struct tick_config *req,
		     struct tick_config *resp)
{
	struct tick_data *d = z->data;
	struct tick_config *cfg = z->config;
	int err = validate_config(req);

	if (err != 0) {
		if (resp != NULL) {
			*resp = *cfg;
		}
		return err;
	}

	*cfg = *req;

	if (d->is_running) {
		k_timer_stop(&d->timer);
		k_timer_start(&d->timer, K_MSEC(cfg->duration_ms), K_MSEC(cfg->period_ms));
	}

	if (resp != NULL) {
		*resp = *cfg;
	}
	return 0;
}

int tick_get_config_impl(const struct zephlet *z, struct tick_config *resp)
{
	struct tick_config *cfg = z->config;

	if (resp != NULL) {
		*resp = *cfg;
	}
	return 0;
}

int tick_init_fn(const struct zephlet *self)
{
	struct tick_data *d = self->data;

	k_timer_init(&d->timer, tick_timer_handler, NULL);
	k_timer_user_data_set(&d->timer, (void *)self);

	d->is_running = false;
	d->is_ready = true;

	return 0;
}
