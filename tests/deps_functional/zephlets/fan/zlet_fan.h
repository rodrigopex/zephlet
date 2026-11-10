#ifndef ZEPHLET_TESTS_DEPS_FUNCTIONAL_FAN_H_
#define ZEPHLET_TESTS_DEPS_FUNCTIONAL_FAN_H_

#include <stdbool.h>

#include <zephyr/kernel.h>

#include "zlet_fan_interface.h"

/** @brief Simulated fan: stores the last speed it was set to. */
struct fan_data {
	bool is_running;
	bool is_ready;
	/* ----- fixture fields ----- */
	uint32_t percent;
	/** Number of `set_speed` calls served. */
	uint32_t sets;
};

int fan_init_fn(const struct zephlet *z);

#endif /* ZEPHLET_TESTS_DEPS_FUNCTIONAL_FAN_H_ */
