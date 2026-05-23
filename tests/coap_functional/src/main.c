#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "zlet_tick.h"
#include "zlet_ui.h"

LOG_MODULE_REGISTER(zlet_coap_functional, LOG_LEVEL_INF);

/**
 * @file
 * @brief Host app for the CoAP functional twister target.
 *
 * Instantiates one `tick` and one `ui` zephlet so the codegen-emitted
 * per-type CoAP resources have live targets. The CoAP service +
 * resources are registered automatically via the frontend's
 * `COAP_SERVICE_DEFINE` and each opted-in zephlet's
 * `COAP_RESOURCE_DEFINE` — there is no inline CoAP setup here.
 */

static struct tick_config tick_fast_cfg = {
	.duration_ms = 100,
	.period_ms = 100,
};
static struct tick_data tick_fast_data;
ZEPHLET_NEW(tick, tick_fast, &tick_fast_cfg, &tick_fast_data, tick_init_fn);

static struct ui_config ui_main_cfg = {
	.blink_period_ms = 250,
};
static struct ui_data ui_main_data;
ZEPHLET_NEW(ui, ui_main, &ui_main_cfg, &ui_main_data, ui_init_fn);

int main(void)
{
	LOG_INF("zephlet coap functional host up on UDP/5683");
	return 0;
}
