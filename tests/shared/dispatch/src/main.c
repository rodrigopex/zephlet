#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "zephlet.h"

/**
 * @file
 * @brief Phase 1 dispatch unit tests.
 *
 * Exercises `zephlet_dispatch` against a synthetic method table. No
 * real zbus channels are involved — dispatch is a pure C function.
 */

static int sut_handler_ok(const struct zephlet *z, struct zephlet_call *call)
{
	ARG_UNUSED(z);
	ARG_UNUSED(call);
	return 0;
}

static int sut_handler_err(const struct zephlet *z, struct zephlet_call *call)
{
	ARG_UNUSED(z);
	ARG_UNUSED(call);
	return -EIO;
}

static const struct zephlet_method sut_methods[] = {
	/* method_id == 0 reserved: handler NULL. */
	{.req_desc = NULL, .resp_desc = NULL, .handler = NULL},
	/* method_id == 1: succeeds. */
	{.req_desc = NULL, .resp_desc = NULL, .handler = sut_handler_ok},
	/* method_id == 2: returns -EIO. */
	{.req_desc = NULL, .resp_desc = NULL, .handler = sut_handler_err},
};

static const struct zephlet_api sut_api = {
	.methods = sut_methods,
	.num_methods = ARRAY_SIZE(sut_methods),
};

static const struct zephlet sut = {
	.name = "sut",
	.api = &sut_api,
};

ZTEST(zephlet_dispatch, test_success)
{
	struct zephlet_call call = {.method_id = 1, .return_code = 1234};
	int ret = zephlet_dispatch(&sut, &call);

	zassert_equal(ret, 0, "dispatch ret=%d", ret);
	zassert_equal(call.return_code, 0, "return_code=%d", call.return_code);
}

ZTEST(zephlet_dispatch, test_handler_error)
{
	struct zephlet_call call = {.method_id = 2};
	int ret = zephlet_dispatch(&sut, &call);

	zassert_equal(ret, 0, "dispatch ret=%d", ret);
	zassert_equal(call.return_code, -EIO, "return_code=%d", call.return_code);
}

ZTEST(zephlet_dispatch, test_null_handler)
{
	struct zephlet_call call = {.method_id = 0};
	int ret = zephlet_dispatch(&sut, &call);

	zassert_equal(ret, 0, "dispatch ret=%d", ret);
	zassert_equal(call.return_code, -ENOSYS, "return_code=%d", call.return_code);
}

ZTEST(zephlet_dispatch, test_oob_method)
{
	struct zephlet_call call = {.method_id = 99};
	int ret = zephlet_dispatch(&sut, &call);

	zassert_equal(ret, 0, "dispatch ret=%d", ret);
	zassert_equal(call.return_code, -ENOSYS, "return_code=%d", call.return_code);
}

ZTEST_SUITE(zephlet_dispatch, NULL, NULL, NULL, NULL, NULL);
