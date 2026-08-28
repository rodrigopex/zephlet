#include <zephyr/kernel.h>

/**
 * @file
 * @brief Build-only host app for the codegen ordering regression test.
 *
 * Instantiates nothing on purpose. The whole assertion lives in the
 * `consumer` module, which includes a generated zephlet header from a
 * plain zephyr_library. See test_build_order.py.
 */

int main(void)
{
	return 0;
}
