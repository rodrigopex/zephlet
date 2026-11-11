#include "zlet_climate.h"

#include <errno.h>

/* The providers' public wrappers: the only way a consumer reaches them. */
#include "zlet_fan_interface.h"
#include "zlet_temperature_interface.h"

#define CLIMATE_CALL_TIMEOUT K_MSEC(100)

int climate_init_fn(const struct zephlet *z)
{
	struct climate_data *d = z->data;

	/* Every provider initialized first (its type ranks lower): usable here. */
	d->is_ready = true;
	return 0;
}

int climate_regulate_impl(const struct zephlet *z, struct climate_decision *resp)
{
	const struct climate_dependencies *deps = z->dependencies;
	const struct climate_config *cfg = z->config;
	struct temperature_reading reading;
	int err = temperature_read(deps->temperature, &reading, CLIMATE_CALL_TIMEOUT);

	if (err != 0) {
		return err;
	}

	int32_t above = reading.celsius - cfg->setpoint_c;
	struct fan_speed speed = {
		.percent = (uint32_t)CLAMP(above * 10, 0, 100),
	};

	err = fan_set_speed(deps->intake, &speed, CLIMATE_CALL_TIMEOUT);
	if (err != 0) {
		return err;
	}
	err = fan_set_speed(deps->exhaust, &speed, CLIMATE_CALL_TIMEOUT);
	if (err != 0) {
		return err;
	}

	if (resp != NULL) {
		resp->celsius = reading.celsius;
		resp->percent = speed.percent;
	}
	return 0;
}

int climate_get_status_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	const struct climate_data *d = z->data;

	if (resp != NULL) {
		resp->is_running = d->is_running;
		resp->is_ready = d->is_ready;
	}
	return 0;
}
