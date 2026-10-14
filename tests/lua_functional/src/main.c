#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "zlet_probe.h"

/**
 * @file
 * @brief Host app for the Lua functional twister target.
 *
 * Three `probe` instances: probe_a and probe_b are the event sources the
 * suites add policies to, probe_idle never gets one. main() hands the suite
 * chosen by CONFIG_TEST_LUA_SUITE to zephlet_lua_coordinate(); a second
 * thread then tries to start the frontend again, which must be refused.
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

/* scripts/<CONFIG_TEST_LUA_SUITE>.lua, precompiled. */
#include SUITE_HEADER

static void second_run(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int rc = zephlet_lua_coordinate(ZEPHLET_LUA_SCRIPT(SUITE));

	printk("second zephlet_lua_coordinate(): %s\n",
	       (rc == -EALREADY) ? "EALREADY" : "unexpected");
}

K_THREAD_DEFINE(second_run_tid, 1024, second_run, NULL, NULL, NULL, 5, 0, 200);

static const char *rc_name(int rc)
{
	switch (rc) {
	case -EINVAL:
		return "EINVAL";
	case -ENOEXEC:
		return "ENOEXEC";
	case -ENOMEM:
		return "ENOMEM";
	case -EFAULT:
		return "EFAULT";
	default:
		return "unexpected";
	}
}

#if defined(CONFIG_TEST_LUA_BAD_BUFFER)
/* A bytecode signature with nothing after it. */
static const char not_lua[] = "\x1bLua";
#endif

/* Every event staging turned away or overwrote, across all instances: the
 * drops counted after the seal and the overwrites before it. */
static long staged_drops(void)
{
	long n = 0;

	STRUCT_SECTION_FOREACH(zephlet_lua_instance, inst) {
		n += (long)atomic_get(&inst->stats.staged_drop);
		n += (long)atomic_get(&inst->preseal_drop);
	}

	return n;
}

int main(void)
{
#if defined(CONFIG_TEST_LUA_BAD_BUFFER)
	int rc = zephlet_lua_coordinate(not_lua, sizeof(not_lua) - 1U, "=not_lua");
#else
	int rc = zephlet_lua_coordinate(ZEPHLET_LUA_SCRIPT(SUITE));
#endif
	struct probe_burst burst = {.count = 2 * CONFIG_ZEPHLETS_LUA_STAGING_DEPTH};
	long before;

	printk("zephlet_lua_coordinate() returned %s\n", rc_name(rc));

	/* Once it has returned, nothing empties staging: the listeners must stop
	 * staging rather than fill it and count drops forever. */
	before = staged_drops();
	(void)probe_burst(&probe_a, &burst, K_MSEC(100));
	k_msleep(50);
	printk("after the frontend stopped: %ld new staged drops\n", staged_drops() - before);
	return 0;
}
#else
int main(void)
{
	printk("lua frontend disabled\n");
	return 0;
}
#endif
