#ifndef ZEPHLET_TESTS_LUA_FUNCTIONAL_PROBE_H_
#define ZEPHLET_TESTS_LUA_FUNCTIONAL_PROBE_H_

#include <stdbool.h>
#include <stdint.h>

#include "zlet_probe_interface.h"

/**
 * @file
 * @brief Test-only `probe` zephlet for the Lua functional test.
 */

struct probe_data {
	bool is_running;
	bool is_ready;
	uint32_t seq;
};

int probe_init_fn(const struct zephlet *z);

#endif /* ZEPHLET_TESTS_LUA_FUNCTIONAL_PROBE_H_ */
