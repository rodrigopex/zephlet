#include "zlet_blob.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(zlet_blob, CONFIG_ZEPHLET_BLOB_LOG_LEVEL);

int blob_get_status_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	struct blob_data *d = z->data;

	if (resp != NULL) {
		resp->is_running = d->is_running;
		resp->is_ready = d->is_ready;
	}
	return 0;
}

int blob_get_config_impl(const struct zephlet *z, struct blob_config *resp)
{
	if (resp != NULL) {
		*resp = *(struct blob_config *)z->config;
	}
	return 0;
}

/* Every run counts, so a test sees how often a transfer ran the rpc */
int blob_read_impl(const struct zephlet *z, struct blob_chunk *resp)
{
	struct blob_data *d = z->data;
	const struct blob_config *cfg = z->config;

	d->runs++;
	if (resp != NULL) {
		resp->data.size = MIN(cfg->size, sizeof(resp->data.bytes));
		for (size_t i = 0; i < resp->data.size; i++) {
			resp->data.bytes[i] = (uint8_t)i;
		}
		resp->runs = d->runs;
	}
	return 0;
}

int blob_init_fn(const struct zephlet *z)
{
	struct blob_data *d = z->data;

	d->is_running = false;
	d->is_ready = true;
	d->runs = 0;
	return 0;
}
