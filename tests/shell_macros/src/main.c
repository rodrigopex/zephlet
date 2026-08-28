#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "zephlet_shell_macros.h"
#include "zephlet_shell_value.h"

/* zephyr_nanopb_sources emits the generated header relative to the
 * proto's path under CMAKE_CURRENT_BINARY_DIR — for `src/fixture.proto`
 * that means `<build>/src/fixture.pb.h`. */
#include "src/fixture.pb.h"

/**
 * @file
 * @brief Full-type-coverage unit tests for the zlet shell macro
 * framework (frontends/shell/include/zephlet_shell_macros.h).
 *
 * `Fixture` (src/fixture.proto) declares exactly one field per nanopb
 * scalar ltype token the framework supports — all 18 — so a dropped
 * token fails the BUILD_ASSERT below, not just a missing test case.
 * No ZEPHLET_NEW/ZLET_SHELL_INSTANCE involved: this exercises the
 * PARSE_FIELD/PRINT_FIELD dispatch directly against a raw nanopb
 * message, independent of the per-instance handler generator.
 */

BUILD_ASSERT(ZLET_SHELL_FIELD_COUNT(FIXTURE_FIELDLIST) == 18,
	     "Fixture must exercise all 18 supported nanopb scalar ltypes");

/* Declaration order in fixture.proto: unsigned(5), signed(7), float(2),
 * bool(1), bytes(2), string(1). */
enum {
	IDX_UINT32,
	IDX_UINT64,
	IDX_FIXED32,
	IDX_FIXED64,
	IDX_UENUM,
	IDX_INT32,
	IDX_INT64,
	IDX_SINT32,
	IDX_SINT64,
	IDX_SFIXED32,
	IDX_SFIXED64,
	IDX_ENUM,
	IDX_FLOAT,
	IDX_DOUBLE,
	IDX_BOOL,
	IDX_BYTES,
	IDX_FIXED_BYTES,
	IDX_STRING,
	IDX_COUNT,
};

static int fixture_parse_all(struct fixture *req, char **argv)
{
	size_t argi = 0;

	FIXTURE_FIELDLIST(ZLET_SHELL_PARSE_FIELD, (*req))
	return 0;

zlet_shell_bad:
	return -EINVAL;
}

static void fixture_print_all(const struct shell *sh, const struct fixture *resp)
{
	FIXTURE_FIELDLIST(ZLET_SHELL_PRINT_FIELD, (*resp))
}

static const char *dummy_output(void)
{
	size_t size;
	const struct shell *sh = shell_backend_dummy_get_ptr();

	return shell_backend_dummy_get_output(sh, &size);
}

/* shell_print()/shell_error() are no-ops outside an active shell command
 * context (shell.c: "Sending a message to a non-active shell leads to a
 * dead lock" — state_get(sh) != SHELL_STATE_ACTIVE just returns). Real
 * ZLET_SHELL_DEFINE_METHOD handlers always run inside shell_execute_cmd(),
 * so this test routes fixture_print_all() through a throwaway registered
 * command instead of calling it directly, to match that context. */
static struct fixture g_print_fixture;

static int test_print_cmd(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	fixture_print_all(sh, &g_print_fixture);
	return 0;
}
SHELL_CMD_ARG_REGISTER(fixture_test_print, NULL, "print g_print_fixture", test_print_cmd, 1, 0);

ZTEST(zephlet_shell_macros, test_round_trip_decimal_and_hex_all_18_fields)
{
	char *argv[IDX_COUNT] = {
		[IDX_UINT32] = "4000000000",
		[IDX_UINT64] = "18000000000000000000",
		[IDX_FIXED32] = "h12345678",
		[IDX_FIXED64] = "h1122334455667788",
		[IDX_UENUM] = "1",
		[IDX_INT32] = "-2000000000",
		[IDX_INT64] = "-9000000000000000000",
		[IDX_SINT32] = "-12345",
		[IDX_SINT64] = "-123456789012345",
		[IDX_SFIXED32] = "hFFFFFFFF",
		[IDX_SFIXED64] = "hFFFFFFFFFFFFFFFF",
		[IDX_ENUM] = "-1",
		[IDX_FLOAT] = "3.5",
		[IDX_DOUBLE] = "-2.25",
		[IDX_BOOL] = "true",
		[IDX_BYTES] = "hDEADBEEF",
		[IDX_FIXED_BYTES] = "hCAFEBABE",
		[IDX_STRING] = "hello",
	};
	struct fixture req = FIXTURE_INIT_ZERO;
	const struct shell *sh = shell_backend_dummy_get_ptr();
	const char *out;

	zassert_equal(fixture_parse_all(&req, argv), 0);

	zassert_equal(req.f_uint32, 4000000000U);
	zassert_equal(req.f_uint64, 18000000000000000000ULL);
	zassert_equal(req.f_fixed32, 0x12345678U);
	zassert_equal(req.f_fixed64, 0x1122334455667788ULL);
	zassert_equal((int)req.f_uenum, 1);
	zassert_equal(req.f_int32, -2000000000);
	zassert_equal(req.f_int64, -9000000000000000000LL);
	zassert_equal(req.f_sint32, -12345);
	zassert_equal(req.f_sint64, -123456789012345LL);
	zassert_equal(req.f_sfixed32, -1); /* hFFFFFFFF truncated to int32_t */
	zassert_equal(req.f_sfixed64, -1); /* hFFFFFFFFFFFFFFFF truncated to int64_t */
	zassert_equal((int)req.f_enum, -1);
	zassert_within(req.f_float, 3.5f, 0.0001f);
	zassert_within(req.f_double, -2.25, 0.0001);
	zassert_true(req.f_bool);
	zassert_equal(req.f_bytes.size, 4);
	zassert_mem_equal(req.f_bytes.bytes, ((uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF}), 4);
	zassert_mem_equal(req.f_fixed_bytes, ((uint8_t[]){0xCA, 0xFE, 0xBA, 0xBE}), 4);
	zassert_str_equal(req.f_string, "hello");

	g_print_fixture = req;
	shell_backend_dummy_clear_output(sh);
	zassert_equal(shell_execute_cmd(sh, "fixture_test_print"), 0);
	out = dummy_output();

	zassert_true(strstr(out, "f_uint32 = 4000000000") != NULL, "%s", out);
	zassert_true(strstr(out, "f_uint64 = 18000000000000000000") != NULL, "%s", out);
	zassert_true(strstr(out, "f_fixed32 = 305419896") != NULL, "%s", out);
	zassert_true(strstr(out, "f_fixed64 = 1234605616436508552") != NULL, "%s", out);
	zassert_true(strstr(out, "f_uenum = 1") != NULL, "%s", out);
	zassert_true(strstr(out, "f_int32 = -2000000000") != NULL, "%s", out);
	zassert_true(strstr(out, "f_int64 = -9000000000000000000") != NULL, "%s", out);
	zassert_true(strstr(out, "f_sint32 = -12345") != NULL, "%s", out);
	zassert_true(strstr(out, "f_sint64 = -123456789012345") != NULL, "%s", out);
	zassert_true(strstr(out, "f_sfixed32 = -1") != NULL, "%s", out);
	zassert_true(strstr(out, "f_sfixed64 = -1") != NULL, "%s", out);
	zassert_true(strstr(out, "f_enum = -1") != NULL, "%s", out);
	zassert_true(strstr(out, "f_float = 3.5") != NULL, "%s", out);
	zassert_true(strstr(out, "f_double = -2.25") != NULL, "%s", out);
	zassert_true(strstr(out, "f_bool = true") != NULL, "%s", out);
	zassert_true(strstr(out, "f_bytes = h\"deadbeef\"") != NULL, "%s", out);
	zassert_true(strstr(out, "f_fixed_bytes = h\"cafebabe\"") != NULL, "%s", out);
	zassert_true(strstr(out, "f_string = \"hello\"") != NULL, "%s", out);
}

ZTEST(zephlet_shell_macros, test_bad_argument_aborts_parse_before_later_fields)
{
	char *argv[IDX_COUNT] = {
		[IDX_UINT32] = "42",
		[IDX_UINT64] = "1",
		[IDX_FIXED32] = "1",
		[IDX_FIXED64] = "1",
		[IDX_UENUM] = "0",
		[IDX_INT32] = "not-a-number", /* malformed: aborts here */
		[IDX_INT64] = "1",
		[IDX_SINT32] = "1",
		[IDX_SINT64] = "1",
		[IDX_SFIXED32] = "1",
		[IDX_SFIXED64] = "1",
		[IDX_ENUM] = "0",
		[IDX_FLOAT] = "1",
		[IDX_DOUBLE] = "1",
		[IDX_BOOL] = "true",
		[IDX_BYTES] = "hAA",
		[IDX_FIXED_BYTES] = "hAABBCCDD",
		[IDX_STRING] = "x",
	};
	struct fixture req = FIXTURE_INIT_ZERO;

	zassert_equal(fixture_parse_all(&req, argv), -EINVAL);
	zassert_equal(req.f_uint32, 42U);
	/* Field after the malformed token never gets touched. */
	zassert_equal(req.f_int64, 0);
}

ZTEST(zephlet_shell_macros, test_parse_uint_decimal_and_hex)
{
	uint64_t v;

	zassert_equal(zlet_shell_parse_uint("1000000", &v), 0);
	zassert_equal(v, 1000000U);

	zassert_equal(zlet_shell_parse_uint("hF4240", &v), 0);
	zassert_equal(v, 1000000U);

	zassert_equal(zlet_shell_parse_uint("", &v), -EINVAL);
	zassert_equal(zlet_shell_parse_uint("12x", &v), -EINVAL);
	zassert_equal(zlet_shell_parse_uint("h", &v), -EINVAL);
	zassert_equal(zlet_shell_parse_uint("-1", &v), -EINVAL);
	/* Decimal overflow past UINT64_MAX. */
	zassert_equal(zlet_shell_parse_uint("99999999999999999999999", &v), -EINVAL);
}

ZTEST(zephlet_shell_macros, test_parse_int_decimal_and_hex)
{
	int64_t v;

	zassert_equal(zlet_shell_parse_int("-1000000", &v), 0);
	zassert_equal(v, -1000000);

	zassert_equal(zlet_shell_parse_int("hFFFFFFFFFFFFFFFF", &v), 0);
	zassert_equal(v, -1);

	zassert_equal(zlet_shell_parse_int("bogus", &v), -EINVAL);
	zassert_equal(zlet_shell_parse_int("h", &v), -EINVAL);
}

ZTEST(zephlet_shell_macros, test_parse_float)
{
	double v;

	zassert_equal(zlet_shell_parse_float("3.5", &v), 0);
	zassert_within(v, 3.5, 0.0001);

	zassert_equal(zlet_shell_parse_float("-1e3", &v), 0);
	zassert_within(v, -1000.0, 0.0001);

	zassert_equal(zlet_shell_parse_float("not-a-float", &v), -EINVAL);
	zassert_equal(zlet_shell_parse_float("", &v), -EINVAL);
}

ZTEST(zephlet_shell_macros, test_parse_bool)
{
	bool v;

	zassert_equal(zlet_shell_parse_bool("true", &v), 0);
	zassert_true(v);
	zassert_equal(zlet_shell_parse_bool("0", &v), 0);
	zassert_false(v);
	zassert_equal(zlet_shell_parse_bool("yes", &v), -EINVAL);
}

ZTEST(zephlet_shell_macros, test_parse_hexbytes_errors)
{
	uint8_t buf[4];
	size_t len;

	zassert_equal(zlet_shell_parse_hexbytes("hDEAD", buf, sizeof(buf), &len), 0);
	zassert_equal(len, 2);
	zassert_mem_equal(buf, ((uint8_t[]){0xDE, 0xAD}), 2);

	/* Missing "h" prefix. */
	zassert_equal(zlet_shell_parse_hexbytes("DEAD", buf, sizeof(buf), &len), -EINVAL);
	/* Odd hex digit count. */
	zassert_equal(zlet_shell_parse_hexbytes("hABC", buf, sizeof(buf), &len), -EINVAL);
	/* Non-hex character. */
	zassert_equal(zlet_shell_parse_hexbytes("hZZ", buf, sizeof(buf), &len), -EINVAL);
	/* Decodes to more bytes than the field's own declared size. */
	zassert_equal(zlet_shell_parse_hexbytes("hAABBCCDDEE", buf, sizeof(buf), &len), -ENOSPC);
}

ZTEST(zephlet_shell_macros, test_parse_string_oversize)
{
	char buf[4];

	zassert_equal(zlet_shell_parse_string("abc", buf, sizeof(buf)), 0);
	zassert_str_equal(buf, "abc");

	/* "abcd" + NUL needs 5 bytes; buf only holds 4. */
	zassert_equal(zlet_shell_parse_string("abcd", buf, sizeof(buf)), -ENOSPC);
}

static void *zephlet_shell_macros_setup(void)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	/* shell_print()/shell_error() are no-ops until the dummy backend's
	 * own init thread has run — see the comment above
	 * fixture_test_print's registration. */
	WAIT_FOR(shell_ready(sh), 20000, k_msleep(1));
	zassert_true(shell_ready(sh), "timed out waiting for dummy shell backend");

	return NULL;
}

ZTEST_SUITE(zephlet_shell_macros, NULL, zephlet_shell_macros_setup, NULL, NULL, NULL);
