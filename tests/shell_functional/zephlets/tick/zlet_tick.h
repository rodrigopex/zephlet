#ifndef ZEPHLET_TESTS_SHELL_FUNCTIONAL_TICK_H_
#define ZEPHLET_TESTS_SHELL_FUNCTIONAL_TICK_H_

#include <stdbool.h>

#include <zephyr/kernel.h>

#include "zlet_tick_interface.h"

/**
 * @file
 * @brief Test-only `tick` zephlet types — mirrors the app's tick so the
 *        shell functional test exercises a realistic dispatch path.
 */

struct tick_data {
	bool is_running;
	bool is_ready;
	struct k_timer timer;
	uint32_t kicks;
};

int tick_init_fn(const struct zephlet *z);

#endif /* ZEPHLET_TESTS_SHELL_FUNCTIONAL_TICK_H_ */
