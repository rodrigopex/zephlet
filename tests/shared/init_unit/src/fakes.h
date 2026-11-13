#ifndef ZEPHLET_TESTS_INIT_UNIT_FAKES_H
#define ZEPHLET_TESTS_INIT_UNIT_FAKES_H

#include <zephyr/fff.h>

/*
 * FFF fake for the logging backend zephlet.c links against; no logging
 * implementation is part of this test binary. snprintk() comes from the
 * unit_testing harness (ztest_mock.c), a host vsnprintf().
 */

DECLARE_FAKE_VOID_FUNC_VARARG(z_log_minimal_printk, const char *, ...);

#define INIT_UNIT_FAKES_LIST(FAKE) FAKE(z_log_minimal_printk)

#endif /* ZEPHLET_TESTS_INIT_UNIT_FAKES_H */
