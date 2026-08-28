#ifndef ZEPHLET_TESTS_SHELL_FUNCTIONAL_UI_H_
#define ZEPHLET_TESTS_SHELL_FUNCTIONAL_UI_H_

#include <stdbool.h>

#include "zlet_ui_interface.h"

/**
 * @file
 * @brief Test-only `ui` zephlet types — a second type (distinct from
 *        `tick`) so the shell functional test can confirm two
 *        different types' instances don't cross-talk under the `zlet`
 *        root command.
 */

struct ui_data {
	bool is_running;
	bool is_ready;
};

int ui_init_fn(const struct zephlet *z);

#endif /* ZEPHLET_TESTS_SHELL_FUNCTIONAL_UI_H_ */
