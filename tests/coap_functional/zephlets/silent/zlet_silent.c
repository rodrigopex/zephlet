#include "zlet_silent.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zlet_silent, CONFIG_ZEPHLET_SILENT_LOG_LEVEL);

int silent_init_fn(const struct zephlet *z)
{
	struct silent_data *d = z->data;

	d->is_running = false;
	d->is_ready = true;
	return 0;
}
