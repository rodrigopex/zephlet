#include "zephlet_coap_translate.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "zephlet.h"
#include "zephlet_coap_consts.h"
#include "zephlet_coap_types.h"

/**
 * @file
 * @brief Pure CoAP ↔ zephlet envelope translation.
 *
 * Implementation notes:
 *   - Compiled only when `CONFIG_ZEPHLETS_COAP=y` (CMake gate).
 *   - No dependency on `coap_service`, sockets, or zbus. All inputs are
 *     buffers or pre-initialised `struct coap_packet` instances; all
 *     outputs are byte-level CoAP options and payloads.
 */

uint8_t zephlet_coap_map_return_code(int32_t rc, bool has_payload)
{
	if (rc == 0) {
		if (has_payload) {
			return COAP_RESPONSE_CODE_CONTENT;
		}
		return COAP_RESPONSE_CODE_CHANGED;
	}

	switch (rc) {
	case -EINVAL:
		return COAP_RESPONSE_CODE_BAD_REQUEST;
	case -ENODEV:
		return COAP_RESPONSE_CODE_NOT_FOUND;
	case -ENOSYS:
		return COAP_RESPONSE_CODE_NOT_ALLOWED;
	case -EALREADY:
		return COAP_RESPONSE_CODE_CONFLICT;
	case -EMSGSIZE:
	case -E2BIG:
		return COAP_RESPONSE_CODE_REQUEST_TOO_LARGE;
	case -EBUSY:
	case -EAGAIN:
		return COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE;
	case -ETIMEDOUT:
		return COAP_RESPONSE_CODE_GATEWAY_TIMEOUT;
	case -ENOMEM:
		return COAP_RESPONSE_CODE_INTERNAL_ERROR;
	default:
		return COAP_RESPONSE_CODE_INTERNAL_ERROR;
	}
}

int zephlet_coap_decode_request(const uint8_t *buf, size_t len, const struct zephlet_coap_method *m,
				struct zephlet_call *call, void *req_storage)
{
	if (m == NULL || call == NULL) {
		return -EINVAL;
	}

	/* Per-method bound emitted by codegen from the proto's nanopb
	 * `<MSG>_SIZE` constant. A request body larger than the method's
	 * own message envelope is a malformed wire frame; surface 4.13. */
	if (len > m->req_max_size) {
		return -EMSGSIZE;
	}

	call->method_id = m->method_id;
	call->req_desc = m->req_desc;
	call->resp_desc = m->resp_desc;
	call->req = NULL;
	call->return_code = 0;
	/* resp / resp_desc storage is the caller's responsibility (Phase 3
	 * dispatcher allocates the response struct on its stack). */

	if (m->req_desc == NULL) {
		/* Empty request: req_max_size is also 0 so the bound check
		 * above already rejected any inbound bytes. */
		return 0;
	}

	if (req_storage == NULL) {
		return -EINVAL;
	}

	pb_istream_t stream = pb_istream_from_buffer(buf, len);
	if (!pb_decode(&stream, m->req_desc, req_storage)) {
		return -EINVAL;
	}

	call->req = req_storage;
	return 0;
}

int zephlet_coap_encode_response(const struct zephlet_call *call, struct coap_packet *cpkt,
				 uint8_t *scratch, size_t scratch_size)
{
	if (call == NULL || cpkt == NULL) {
		return -EINVAL;
	}

	size_t payload_len = 0;
	bool has_payload = false;

	/* On success with a response descriptor, attempt to encode the
	 * payload into @p scratch up front so we know whether to mark the
	 * code as 2.04 or 2.05. */
	if (call->return_code == 0 && call->resp_desc != NULL && call->resp != NULL) {
		if (scratch == NULL || scratch_size == 0) {
			return -ENOMEM;
		}
		pb_ostream_t os = pb_ostream_from_buffer(scratch, scratch_size);
		if (!pb_encode(&os, call->resp_desc, call->resp)) {
			return -ENOMEM;
		}
		payload_len = os.bytes_written;
		has_payload = (payload_len > 0);
	}

	int err = coap_header_set_code(
		cpkt, zephlet_coap_map_return_code(call->return_code, has_payload));
	if (err < 0) {
		return -ENOMEM;
	}

	/* Verbatim raw errno is carried on every response so the (lossy)
	 * class mapping does not erase the precise handler return code. The
	 * signed value is reinterpreted as unsigned for the wire; receivers
	 * cast back to int32_t. */
	err = coap_append_option_int(cpkt, ZEPHLET_COAP_OPT_ERRNO, (uint32_t)call->return_code);
	if (err < 0) {
		return -ENOMEM;
	}

	if (!has_payload) {
		return 0;
	}

	err = coap_append_option_int(cpkt, COAP_OPTION_CONTENT_FORMAT, ZEPHLET_COAP_CT_NANOPB);
	if (err < 0) {
		return -ENOMEM;
	}

	err = coap_packet_append_payload_marker(cpkt);
	if (err < 0) {
		return -ENOMEM;
	}

	err = coap_packet_append_payload(cpkt, scratch, (uint16_t)payload_len);
	if (err < 0) {
		return -ENOMEM;
	}

	return 0;
}
