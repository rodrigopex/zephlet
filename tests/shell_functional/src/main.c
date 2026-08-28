#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "zephlet_shell_macros.h"
#include "zlet_tick.h"
#include "zlet_ui.h"

LOG_MODULE_REGISTER(zlet_shell_functional, LOG_LEVEL_INF);

/**
 * @file
 * @brief Host app for the shell functional twister target.
 *
 * Instantiates two `tick` instances (tick_fast, tick_slow) and one
 * `ui` instance (ui_main), each registered under the `zlet` shell root
 * command via ZLET_SHELL_INSTANCE()/ZLET_SHELL_DEFINE() — no inline
 * shell setup beyond these macro calls.
 */

static struct tick_config tick_fast_cfg = {
	.duration_ms = 100,
	.period_ms = 100,
};
static struct tick_data tick_fast_data;
ZEPHLET_NEW(tick, tick_fast, &tick_fast_cfg, &tick_fast_data, tick_init_fn);
ZLET_SHELL_INSTANCE(tick, tick_fast);

/* Second `tick` instance — exercises the no-cross-talk case between two
 * instances of the same type. */
static struct tick_config tick_slow_cfg = {
	.duration_ms = 500,
	.period_ms = 500,
};
static struct tick_data tick_slow_data;
ZEPHLET_NEW(tick, tick_slow, &tick_slow_cfg, &tick_slow_data, tick_init_fn);
ZLET_SHELL_INSTANCE(tick, tick_slow);

static struct ui_config ui_main_cfg = {
	.blink_period_ms = 250,
};
static struct ui_data ui_main_data;
ZEPHLET_NEW(ui, ui_main, &ui_main_cfg, &ui_main_data, ui_init_fn);
ZLET_SHELL_INSTANCE(ui, ui_main);

ZLET_SHELL_DEFINE(tick_fast, tick_slow, ui_main);

int main(void)
{
	LOG_INF("zephlet shell functional host up");
	return 0;
}
