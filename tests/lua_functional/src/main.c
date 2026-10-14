#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "zlet_probe.h"

/**
 * @file
 * @brief Host app for the Lua functional twister target.
 *
 * Three `probe` instances: probe_a and probe_b are the event sources the
 * suites connect to, probe_idle is never connected. main() hands the suite
 * chosen by CONFIG_TEST_LUA_SUITE to zephlet_lua_run(); a second thread then
 * tries to start the frontend again, which must be refused.
 */

static struct probe_config probe_a_cfg;
static struct probe_data probe_a_data;
ZEPHLET_NEW(probe, probe_a, &probe_a_cfg, &probe_a_data, probe_init_fn);

static struct probe_config probe_b_cfg;
static struct probe_data probe_b_data;
ZEPHLET_NEW(probe, probe_b, &probe_b_cfg, &probe_b_data, probe_init_fn);

static struct probe_config probe_idle_cfg;
static struct probe_data probe_idle_data;
ZEPHLET_NEW(probe, probe_idle, &probe_idle_cfg, &probe_idle_data, probe_init_fn);

#if defined(CONFIG_ZEPHLETS_LUA)
#include <zephlet_lua.h>

static const char suite[] = {
#include "suite.lua.inc"
};

static void second_run(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int rc = zephlet_lua_run(suite, sizeof(suite), "=again");

	printk("second zephlet_lua_run(): %s\n", (rc == -EALREADY) ? "EALREADY" : "unexpected");
}

K_THREAD_DEFINE(second_run_tid, 1024, second_run, NULL, NULL, NULL, 5, 0, 200);

int main(void)
{
	int rc = zephlet_lua_run(suite, sizeof(suite), "=" CONFIG_TEST_LUA_SUITE);

	printk("zephlet_lua_run() returned %s\n", (rc == -EINVAL) ? "EINVAL" : "unexpected");
	return 0;
}
#else
int main(void)
{
	printk("lua frontend disabled\n");
	return 0;
}
#endif
