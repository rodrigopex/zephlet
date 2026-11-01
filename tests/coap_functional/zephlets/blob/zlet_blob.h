#ifndef ZEPHLET_TESTS_COAP_FUNCTIONAL_BLOB_H_
#define ZEPHLET_TESTS_COAP_FUNCTIONAL_BLOB_H_

#include <stdbool.h>
#include <stdint.h>

#include "zlet_blob_interface.h"

/**
 * @file
 * @brief Test-only `blob` zephlet: a response longer than one block, for
 *        the CoAP Block2 scenario.
 */

struct blob_data {
	bool is_running;
	bool is_ready;
	/* ---- blob ---- */
	uint32_t runs;
};

int blob_init_fn(const struct zephlet *z);

#endif /* ZEPHLET_TESTS_COAP_FUNCTIONAL_BLOB_H_ */
