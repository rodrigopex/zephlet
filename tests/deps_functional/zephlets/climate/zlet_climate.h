#ifndef ZEPHLET_TESTS_DEPS_FUNCTIONAL_CLIMATE_H_
#define ZEPHLET_TESTS_DEPS_FUNCTIONAL_CLIMATE_H_

#include <stdbool.h>

#include <zephyr/kernel.h>

#include "zlet_climate_interface.h"

/** @brief Climate controller: one sensor in, two fans out. */
struct climate_data {
	bool is_running;
	bool is_ready;
};

int climate_init_fn(const struct zephlet *z);

#endif /* ZEPHLET_TESTS_DEPS_FUNCTIONAL_CLIMATE_H_ */
