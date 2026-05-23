#ifndef ZEPHLET_TESTS_COAP_FUNCTIONAL_UI_H_
#define ZEPHLET_TESTS_COAP_FUNCTIONAL_UI_H_

#include <stdbool.h>

#include "zlet_ui_interface.h"

/**
 * @file
 * @brief Test-only `ui` zephlet types — minimal placeholder used by the
 *        CoAP functional test to verify api-mismatch routing.
 */

struct ui_data {
	bool is_running;
	bool is_ready;
};

int ui_init_fn(const struct zephlet *z);

#endif /* ZEPHLET_TESTS_COAP_FUNCTIONAL_UI_H_ */
