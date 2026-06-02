#ifndef ZEPHLET_TESTS_COAP_FUNCTIONAL_SILENT_H_
#define ZEPHLET_TESTS_COAP_FUNCTIONAL_SILENT_H_

#include <stdbool.h>

#include "zlet_silent_interface.h"

/**
 * @file
 * @brief Test-only `silent` zephlet types.
 *
 * `silent` opts into the CoAP RPC frontend but NOT into discovery, so a
 * `silent_inst` instance is callable yet never advertised in
 * `/.well-known/core`. Used by the discovery functional test to prove
 * the `(zephlet.coap_discoverable)` opt-in gates emission.
 *
 * All handlers fall through to the codegen-emitted `__weak` defaults
 * returning -ENOSYS; the test does not exercise SilentApi RPCs directly.
 */

struct silent_data {
	bool is_running;
	bool is_ready;
};

int silent_init_fn(const struct zephlet *z);

#endif /* ZEPHLET_TESTS_COAP_FUNCTIONAL_SILENT_H_ */
