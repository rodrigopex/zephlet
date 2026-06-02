#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "zlet_silent.h"
#include "zlet_tick.h"
#include "zlet_ui.h"

LOG_MODULE_REGISTER(zlet_coap_functional, LOG_LEVEL_INF);

/**
 * @file
 * @brief Host app for the CoAP functional twister target.
 *
 * Instantiates one `tick`, one `ui`, and one `silent` zephlet so the
 * codegen-emitted per-type CoAP resources have live targets. `tick` and
 * `ui` opt into discovery; `silent` exposes RPCs but stays absent from
 * `/.well-known/core`, which the discovery test asserts. The CoAP
 * service + resources are registered automatically via the frontend's
 * `COAP_SERVICE_DEFINE` and each opted-in zephlet's
 * `COAP_RESOURCE_DEFINE` — there is no inline CoAP setup here.
 */

static struct tick_config tick_fast_cfg = {
	.duration_ms = 100,
	.period_ms = 100,
};
static struct tick_data tick_fast_data;
ZEPHLET_NEW(tick, tick_fast, &tick_fast_cfg, &tick_fast_data, tick_init_fn);

/* Second `tick` instance — exercises the multi-instance case of the
 * `/zlet/tick/instances` runtime walk. */
static struct tick_config tick_slow_cfg = {
	.duration_ms = 500,
	.period_ms = 500,
};
static struct tick_data tick_slow_data;
ZEPHLET_NEW(tick, tick_slow, &tick_slow_cfg, &tick_slow_data, tick_init_fn);

static struct ui_config ui_main_cfg = {
	.blink_period_ms = 250,
};
static struct ui_data ui_main_data;
ZEPHLET_NEW(ui, ui_main, &ui_main_cfg, &ui_main_data, ui_init_fn);

static struct silent_config silent_inst_cfg = {
	.nothing = 0,
};
static struct silent_data silent_inst_data;
ZEPHLET_NEW(silent, silent_inst, &silent_inst_cfg, &silent_inst_data, silent_init_fn);

int main(void)
{
	LOG_INF("zephlet coap functional host up on UDP/5683");
	return 0;
}
