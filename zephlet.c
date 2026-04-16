#include "zephlet.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>

int zephlet_start_core(struct zephlet_data *data, struct zephlet_status *out_status)
{
	int ret = 0;

	if (!data->status->is_ready) {
		ret = -ENODEV;
	} else if (data->status->is_running) {
		ret = -EALREADY;
	} else {
		data->status->is_running = true;
	}

	*out_status = *data->status;
	return ret;
}

int zephlet_stop_core(struct zephlet_data *data, struct zephlet_status *out_status)
{
	int ret = 0;

	if (!data->status->is_running) {
		ret = -EALREADY;
	} else {
		data->status->is_running = false;
	}

	*out_status = *data->status;
	return ret;
}

int zephlet_get_status_core(const struct zephlet_data *data, struct zephlet_status *out_status)
{
	*out_status = *data->status;
	return 0;
}

int zephlet_get_settings_core(const struct zephlet_data *data, void *out_settings)
{
	if (data->settings == NULL || data->settings_size == 0) {
		return -ENOTSUP;
	}

	memcpy(out_settings, data->settings, data->settings_size);
	return 0;
}

int zephlet_update_settings_core(struct zephlet_data *data, const void *in_settings)
{
	if (data->settings == NULL || data->settings_size == 0) {
		return -ENOTSUP;
	}

	memcpy(data->settings, in_settings, data->settings_size);
	return 0;
}

int zephlet_get_events_core(const struct zephlet_data *data, void *out_events)
{
	if (data->events == NULL || data->events_size == 0) {
		return -ENOTSUP;
	}

	memcpy(out_events, data->events, data->events_size);
	return 0;
}

int zephlets_init_fn(void)
{
	printk("Init zephlets:\n");

	STRUCT_SECTION_FOREACH(zephlet, instance) {
		printk("%p: %s initialing...\n", instance, instance->name);
		if (instance->init_fn != NULL) {
			int err = instance->init_fn(instance);
			if (err == 0) {
				instance->data->status->is_ready = true;
			}
		}
	}

	return 0;
}

SYS_INIT(zephlets_init_fn, APPLICATION, 99);
