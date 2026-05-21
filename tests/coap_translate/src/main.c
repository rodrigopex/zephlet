#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>
#include <zephyr/ztest.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "zephlet.h"
#include "zephlet_coap_consts.h"
#include "zephlet_coap_translate.h"
#include "zephlet_coap_types.h"

/* zephyr_nanopb_sources emits the generated header relative to the
 * proto's path under CMAKE_CURRENT_BINARY_DIR — for `src/test_msg.proto`
 * that means `<build>/src/test_msg.pb.h`. The include path is the
 * binary-dir root, hence the `src/` prefix here. */
#include "src/test_msg.pb.h"

/**
 * @file
 * @brief Phase 2 unit tests for zephlet_coap_translate.
 *
 * Exercises the pure decode/encode helpers and the return_code → CoAP
 * class mapping table with hand-built buffers. No coap_service, no
 * zbus, no sockets — the translator API is the entire surface under
 * test.
 */

/* ----- Method-descriptor fixtures -------------------------------------- */

/* Methods with TestReq → TestResp; matches what the Phase 3 dispatcher
 * would see for a service method that takes and returns a payload. */
static const struct zephlet_coap_method method_req_resp = {
	.path_segment = "echo",
	.method_id = 1,
	.req_desc = &test_req_t_msg,
	.resp_desc = &test_resp_t_msg,
	.req_max_size = TEST_REQ_SIZE,
	.resp_max_size = TEST_RESP_SIZE,
};

/* Method with Empty request (req_desc == NULL, req_max_size == 0). */
static const struct zephlet_coap_method method_empty_req = {
	.path_segment = "ping",
	.method_id = 2,
	.req_desc = NULL,
	.resp_desc = &test_resp_t_msg,
	.req_max_size = 0,
	.resp_max_size = TEST_RESP_SIZE,
};

/* ----- Helpers --------------------------------------------------------- */

/**
 * @brief Initialise a CoAP packet over a stack buffer and prepare it for
 * response encoding. Mirrors what coap_service would hand the dispatcher
 * after parsing the inbound request.
 */
static void init_response_packet(struct coap_packet *cpkt, uint8_t *buf, size_t buf_size)
{
	int err = coap_packet_init(cpkt, buf, (uint16_t)buf_size,
				   /* ver  = */ 1,
				   /* type = */ COAP_TYPE_ACK,
				   /* tkl  = */ 0,
				   /* tok  = */ NULL,
				   /* code = */ 0, /* overwritten by translate */
				   /* mid  = */ 0x1234);
	zassert_equal(err, 0, "coap_packet_init err=%d", err);
}

/**
 * @brief Locate the first CoAP option matching @p code in @p cpkt and
 * decode its value as an unsigned integer. Returns true on hit.
 */
static bool find_option_int(const struct coap_packet *cpkt, uint16_t code, uint32_t *value_out)
{
	struct coap_option opt = {0};
	int n = coap_find_options(cpkt, code, &opt, 1);

	if (n <= 0) {
		return false;
	}
	*value_out = (uint32_t)coap_option_value_to_int(&opt);
	return true;
}

/* ====================================================================== */
/* Mapping table                                                          */
/* ====================================================================== */

ZTEST(zephlet_coap_translate, test_map_success_with_payload)
{
	zassert_equal(zephlet_coap_map_return_code(0, true), COAP_RESPONSE_CODE_CONTENT,
		      "expected 2.05");
}

ZTEST(zephlet_coap_translate, test_map_success_no_payload)
{
	zassert_equal(zephlet_coap_map_return_code(0, false), COAP_RESPONSE_CODE_CHANGED,
		      "expected 2.04");
}

ZTEST(zephlet_coap_translate, test_map_einval)
{
	zassert_equal(zephlet_coap_map_return_code(-EINVAL, false), COAP_RESPONSE_CODE_BAD_REQUEST,
		      "expected 4.00");
}

ZTEST(zephlet_coap_translate, test_map_enodev)
{
	zassert_equal(zephlet_coap_map_return_code(-ENODEV, false), COAP_RESPONSE_CODE_NOT_FOUND,
		      "expected 4.04");
}

ZTEST(zephlet_coap_translate, test_map_enosys)
{
	zassert_equal(zephlet_coap_map_return_code(-ENOSYS, false), COAP_RESPONSE_CODE_NOT_ALLOWED,
		      "expected 4.05");
}

ZTEST(zephlet_coap_translate, test_map_ealready)
{
	zassert_equal(zephlet_coap_map_return_code(-EALREADY, false), COAP_RESPONSE_CODE_CONFLICT,
		      "expected 4.09");
}

ZTEST(zephlet_coap_translate, test_map_emsgsize)
{
	zassert_equal(zephlet_coap_map_return_code(-EMSGSIZE, false),
		      COAP_RESPONSE_CODE_REQUEST_TOO_LARGE, "expected 4.13");
}

ZTEST(zephlet_coap_translate, test_map_ebusy)
{
	zassert_equal(zephlet_coap_map_return_code(-EBUSY, false),
		      COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE, "expected 5.03");
}

ZTEST(zephlet_coap_translate, test_map_eagain)
{
	zassert_equal(zephlet_coap_map_return_code(-EAGAIN, false),
		      COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE, "expected 5.03");
}

ZTEST(zephlet_coap_translate, test_map_etimedout)
{
	zassert_equal(zephlet_coap_map_return_code(-ETIMEDOUT, false),
		      COAP_RESPONSE_CODE_GATEWAY_TIMEOUT, "expected 5.04");
}

ZTEST(zephlet_coap_translate, test_map_enomem)
{
	zassert_equal(zephlet_coap_map_return_code(-ENOMEM, false),
		      COAP_RESPONSE_CODE_INTERNAL_ERROR, "expected 5.00");
}

ZTEST(zephlet_coap_translate, test_map_unknown_neg)
{
	/* Any unmapped negative errno collapses to 5.00 — preserves the
	 * "lossy class mapping" property; the precise errno survives via
	 * the verbatim-errno option attached by encode_response. */
	zassert_equal(zephlet_coap_map_return_code(-EIO, false), COAP_RESPONSE_CODE_INTERNAL_ERROR,
		      "expected 5.00 default");
}

/* ====================================================================== */
/* decode_request                                                          */
/* ====================================================================== */

/**
 * @brief Hand-encode a TestReq via nanopb so the decoder receives a
 * realistic wire image (rather than a hard-coded byte sequence the test
 * would have to keep in sync with the proto).
 */
static size_t encode_test_req(uint8_t *buf, size_t buf_size, uint32_t v)
{
	test_req_t msg = {.v = v};
	pb_ostream_t s = pb_ostream_from_buffer(buf, buf_size);

	zassert_true(pb_encode(&s, &test_req_t_msg, &msg), "pb_encode for test req failed");
	return s.bytes_written;
}

ZTEST(zephlet_coap_translate, test_decode_ok)
{
	uint8_t buf[TEST_REQ_SIZE];
	size_t len = encode_test_req(buf, sizeof(buf), 42);

	struct zephlet_call call = {0};
	test_req_t req = {0};

	int err = zephlet_coap_decode_request(buf, len, &method_req_resp, &call, &req);
	zassert_equal(err, 0, "decode err=%d", err);
	zassert_equal(call.method_id, method_req_resp.method_id, "method_id mismatch");
	zassert_equal_ptr(call.req_desc, &test_req_t_msg, "req_desc");
	zassert_equal_ptr(call.resp_desc, &test_resp_t_msg, "resp_desc");
	zassert_equal_ptr(call.req, &req, "req storage");
	zassert_equal(req.v, 42, "decoded v=%u", req.v);
}

ZTEST(zephlet_coap_translate, test_decode_malformed)
{
	/* 0xFF is reserved-for-future-use in proto wire format and is
	 * rejected at the field-tag varint pass. */
	const uint8_t bogus[] = {0xFF, 0xFF, 0xFF};
	struct zephlet_call call = {0};
	test_req_t req = {0};

	int err = zephlet_coap_decode_request(bogus, sizeof(bogus), &method_req_resp, &call, &req);
	zassert_equal(err, -EINVAL, "expected -EINVAL got %d", err);
}

ZTEST(zephlet_coap_translate, test_decode_oversize)
{
	uint8_t buf[TEST_REQ_SIZE + 4] = {0};
	struct zephlet_call call = {0};
	test_req_t req = {0};

	int err = zephlet_coap_decode_request(buf, sizeof(buf), &method_req_resp, &call, &req);
	zassert_equal(err, -EMSGSIZE, "expected -EMSGSIZE got %d", err);
}

ZTEST(zephlet_coap_translate, test_decode_empty_req_no_body)
{
	struct zephlet_call call = {0};

	int err = zephlet_coap_decode_request(NULL, 0, &method_empty_req, &call, NULL);
	zassert_equal(err, 0, "decode empty err=%d", err);
	zassert_equal(call.method_id, method_empty_req.method_id, "method_id");
	zassert_is_null(call.req_desc, "req_desc must be NULL for Empty");
	zassert_is_null(call.req, "req must be NULL for Empty");
}

ZTEST(zephlet_coap_translate, test_decode_empty_req_with_body)
{
	const uint8_t one_byte[1] = {0x08};
	struct zephlet_call call = {0};

	int err = zephlet_coap_decode_request(one_byte, sizeof(one_byte), &method_empty_req, &call,
					      NULL);
	zassert_equal(err, -EMSGSIZE, "expected -EMSGSIZE for body on Empty req, got %d", err);
}

/* ====================================================================== */
/* encode_response                                                         */
/* ====================================================================== */

ZTEST(zephlet_coap_translate, test_encode_success_with_resp)
{
	uint8_t pkt_buf[128];
	uint8_t scratch[TEST_RESP_SIZE];
	test_resp_t resp = {.v = 99};

	struct zephlet_call call = {
		.method_id = method_req_resp.method_id,
		.return_code = 0,
		.resp_desc = &test_resp_t_msg,
		.resp = &resp,
	};
	struct coap_packet cpkt;
	init_response_packet(&cpkt, pkt_buf, sizeof(pkt_buf));

	int err = zephlet_coap_encode_response(&call, &cpkt, scratch, sizeof(scratch));
	zassert_equal(err, 0, "encode err=%d", err);

	zassert_equal(coap_header_get_code(&cpkt), COAP_RESPONSE_CODE_CONTENT, "expected 2.05");

	uint32_t errno_opt = 0xDEADBEEFu;
	zassert_true(find_option_int(&cpkt, ZEPHLET_COAP_OPT_ERRNO, &errno_opt),
		     "errno option missing");
	zassert_equal((int32_t)errno_opt, 0, "errno option expected 0, got %d", (int32_t)errno_opt);

	uint32_t ct = 0;
	zassert_true(find_option_int(&cpkt, COAP_OPTION_CONTENT_FORMAT, &ct), "CT option missing");
	zassert_equal(ct, ZEPHLET_COAP_CT_NANOPB, "CT=%u", ct);

	uint16_t payload_len = 0;
	const uint8_t *payload = coap_packet_get_payload(&cpkt, &payload_len);
	zassert_not_null(payload, "payload pointer null");
	zassert_true(payload_len > 0, "payload empty");

	test_resp_t decoded = {0};
	pb_istream_t istream = pb_istream_from_buffer(payload, payload_len);
	zassert_true(pb_decode(&istream, &test_resp_t_msg, &decoded), "round-trip decode failed");
	zassert_equal(decoded.v, 99, "round-trip v=%u", decoded.v);
}

ZTEST(zephlet_coap_translate, test_encode_success_empty_resp)
{
	uint8_t pkt_buf[64];
	struct zephlet_call call = {
		.method_id = method_empty_req.method_id,
		.return_code = 0,
		.resp_desc = NULL,
		.resp = NULL,
	};
	struct coap_packet cpkt;
	init_response_packet(&cpkt, pkt_buf, sizeof(pkt_buf));

	int err = zephlet_coap_encode_response(&call, &cpkt, NULL, 0);
	zassert_equal(err, 0, "encode err=%d", err);

	zassert_equal(coap_header_get_code(&cpkt), COAP_RESPONSE_CODE_CHANGED, "expected 2.04");

	uint32_t errno_opt = 0xDEADBEEFu;
	zassert_true(find_option_int(&cpkt, ZEPHLET_COAP_OPT_ERRNO, &errno_opt),
		     "errno option missing");
	zassert_equal((int32_t)errno_opt, 0, "errno option expected 0");

	uint32_t ct = 0;
	zassert_false(find_option_int(&cpkt, COAP_OPTION_CONTENT_FORMAT, &ct),
		      "CT option present on empty response");

	uint16_t payload_len = 0xFFFF;
	(void)coap_packet_get_payload(&cpkt, &payload_len);
	zassert_equal(payload_len, 0, "expected no payload, got %u bytes", payload_len);
}

ZTEST(zephlet_coap_translate, test_encode_resp_default_value_no_payload)
{
	/* TestResp with v == 0: proto3 default-value omission produces a
	 * zero-byte nanopb encoding. The encoder must downgrade the code
	 * from 2.05 to 2.04 even though resp_desc is set. */
	uint8_t pkt_buf[64];
	uint8_t scratch[TEST_RESP_SIZE];
	test_resp_t resp = {.v = 0};

	struct zephlet_call call = {
		.method_id = method_req_resp.method_id,
		.return_code = 0,
		.resp_desc = &test_resp_t_msg,
		.resp = &resp,
	};
	struct coap_packet cpkt;
	init_response_packet(&cpkt, pkt_buf, sizeof(pkt_buf));

	int err = zephlet_coap_encode_response(&call, &cpkt, scratch, sizeof(scratch));
	zassert_equal(err, 0, "encode err=%d", err);
	zassert_equal(coap_header_get_code(&cpkt), COAP_RESPONSE_CODE_CHANGED,
		      "expected 2.04 for empty-encoded resp");

	uint32_t ct = 0;
	zassert_false(find_option_int(&cpkt, COAP_OPTION_CONTENT_FORMAT, &ct),
		      "CT option present when payload empty");
}

ZTEST(zephlet_coap_translate, test_encode_errno_verbatim)
{
	uint8_t pkt_buf[64];
	struct zephlet_call call = {
		.method_id = method_req_resp.method_id,
		.return_code = -EINVAL,
		.resp_desc = NULL,
		.resp = NULL,
	};
	struct coap_packet cpkt;
	init_response_packet(&cpkt, pkt_buf, sizeof(pkt_buf));

	int err = zephlet_coap_encode_response(&call, &cpkt, NULL, 0);
	zassert_equal(err, 0, "encode err=%d", err);
	zassert_equal(coap_header_get_code(&cpkt), COAP_RESPONSE_CODE_BAD_REQUEST,
		      "expected 4.00 for -EINVAL");

	uint32_t raw = 0;
	zassert_true(find_option_int(&cpkt, ZEPHLET_COAP_OPT_ERRNO, &raw), "errno option missing");
	zassert_equal((int32_t)raw, -EINVAL, "errno verbatim mismatch: got %d expected %d",
		      (int32_t)raw, -EINVAL);
}

ZTEST(zephlet_coap_translate, test_encode_errno_verbatim_emsgsize)
{
	/* The 4.13 path: oversize body would have produced -EMSGSIZE on
	 * the dispatcher side; the response must carry both the mapped
	 * class byte and the verbatim raw errno. */
	uint8_t pkt_buf[64];
	struct zephlet_call call = {
		.method_id = method_req_resp.method_id,
		.return_code = -EMSGSIZE,
	};
	struct coap_packet cpkt;
	init_response_packet(&cpkt, pkt_buf, sizeof(pkt_buf));

	int err = zephlet_coap_encode_response(&call, &cpkt, NULL, 0);
	zassert_equal(err, 0, "encode err=%d", err);
	zassert_equal(coap_header_get_code(&cpkt), COAP_RESPONSE_CODE_REQUEST_TOO_LARGE,
		      "expected 4.13 for -EMSGSIZE");

	uint32_t raw = 0;
	zassert_true(find_option_int(&cpkt, ZEPHLET_COAP_OPT_ERRNO, &raw), "errno option missing");
	zassert_equal((int32_t)raw, -EMSGSIZE, "errno verbatim mismatch");
}

ZTEST_SUITE(zephlet_coap_translate, NULL, NULL, NULL, NULL, NULL);
