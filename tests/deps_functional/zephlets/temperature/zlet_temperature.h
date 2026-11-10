#ifndef ZEPHLET_TESTS_DEPS_FUNCTIONAL_TEMPERATURE_H_
#define ZEPHLET_TESTS_DEPS_FUNCTIONAL_TEMPERATURE_H_

#include <stdbool.h>

#include <zephyr/kernel.h>

#include "zlet_temperature_interface.h"

/** @brief Simulated temperature sensor: reports what `set` last stored. */
struct temperature_data {
	bool is_running;
	bool is_ready;
	/* ----- fixture fields ----- */
	int32_t celsius;
	/** Number of `read` calls served. */
	uint32_t reads;
};

int temperature_init_fn(const struct zephlet *z);

#endif /* ZEPHLET_TESTS_DEPS_FUNCTIONAL_TEMPERATURE_H_ */
