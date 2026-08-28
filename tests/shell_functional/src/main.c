#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "zlet_tick.h"
#include "zlet_typelab.h"
#include "zlet_ui.h"

LOG_MODULE_REGISTER(zlet_shell_functional, LOG_LEVEL_INF);

/**
 * @file
 * @brief Host app for the shell functional twister target.
 *
 * Instantiates two `tick` instances (tick_fast, tick_slow), one `ui`
 * instance (ui_main), and one `typelab` instance (typelab_bench). Each
 * ZEPHLET_NEW(...) call registers its instance under the `zlet` shell
 * root command automatically (CONFIG_ZEPHLETS_SHELL=y, set in prj.conf)
 * via the `_ZLET_SHELL_HOOK_<type>` hook chained into
 * `_ZLET_FRONTEND_HOOKS_<type>` — no shell-specific code needed here.
 *
 * `typelab_bench` is the full-type-coverage bench: one Config field and
 * one set_X/get_X rpc pair per nanopb scalar type the shell frontend
 * supports, so `tests/shell_functional/pytest/test_typelab_types.py`
 * exercises every shell parse/print path through a real `zlet
 * typelab_bench ...` command, not just the couple of scalar types
 * tick/ui happen to use.
 */

static struct tick_config tick_fast_cfg = {
	.duration_ms = 100,
	.period_ms = 100,
};
static struct tick_data tick_fast_data;
ZEPHLET_NEW(tick, tick_fast, &tick_fast_cfg, &tick_fast_data, tick_init_fn);

/* Second `tick` instance — exercises the no-cross-talk case between two
 * instances of the same type. */
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

static struct typelab_config typelab_bench_cfg;
static struct typelab_data typelab_bench_data;
ZEPHLET_NEW(typelab, typelab_bench, &typelab_bench_cfg, &typelab_bench_data, typelab_init_fn);

int main(void)
{
	LOG_INF("zephlet shell functional host up");
	return 0;
}
