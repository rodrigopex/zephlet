#include "zephlet_coap_send.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_service.h>

#include "zephlet_coap_consts.h"
#include "zephlet_coap_translate.h"

LOG_MODULE_DECLARE(zlet_coap);

/**
 * @file
 * @brief CoAP response builder shared by every per-type zephlet handler.
 *
 * The two entrypoints construct a response packet on the handler's
 * stack, carry token + message id from the request, map the handler
 * return code to a CoAP code, attach the verbatim-errno option, and
 * optionally append a payload + Content-Format. They then forward the
 * packet to `coap_resource_send`.
 */

/* Response buffer size: matches Zephyr's coap_service convention so
 * the dispatcher's stack frame uses the same allocator-friendly width
 * as samples and other server resources. */
#define ZEPHLET_COAP_SEND_BUF_SIZE CONFIG_COAP_SERVER_MESSAGE_SIZE

static int build_response_header(const struct coap_packet *req, uint8_t *buf, size_t buf_size,
				 uint8_t code, struct coap_packet *out)
{
	uint8_t token[COAP_TOKEN_MAX_LEN];
	uint8_t tkl = coap_header_get_token(req, token);
	uint16_t id = coap_header_get_id(req);
	uint8_t req_type = coap_header_get_type(req);
	uint8_t resp_type = (req_type == COAP_TYPE_CON) ? COAP_TYPE_ACK : COAP_TYPE_NON_CON;

	return coap_packet_init(out, buf, buf_size, COAP_VERSION_1, resp_type, tkl, token, code,
				id);
}

static int append_errno_option(struct coap_packet *cpkt, int rc)
{
	return coap_append_option_int(cpkt, ZEPHLET_COAP_OPT_ERRNO, (uint32_t)rc);
}

int zephlet_coap_send_error(struct coap_resource *res, struct coap_packet *req,
			    struct sockaddr *addr, socklen_t addr_len, int rc)
{
	uint8_t buf[ZEPHLET_COAP_SEND_BUF_SIZE];
	struct coap_packet response;
	uint8_t code = zephlet_coap_map_return_code(rc, false);

	int err = build_response_header(req, buf, sizeof(buf), code, &response);
	if (err < 0) {
		LOG_WRN("coap_packet_init failed: %d", err);
		return err;
	}

	err = append_errno_option(&response, rc);
	if (err < 0) {
		LOG_WRN("append errno option failed: %d", err);
		return err;
	}

	return coap_resource_send(res, &response, addr, addr_len, NULL);
}

int zephlet_coap_send_response(struct coap_resource *res, struct coap_packet *req,
			       struct sockaddr *addr, socklen_t addr_len, int rc,
			       const uint8_t *payload, size_t payload_len)
{
	if (rc != 0 || payload_len == 0) {
		return zephlet_coap_send_error(res, req, addr, addr_len, rc);
	}

	uint8_t buf[ZEPHLET_COAP_SEND_BUF_SIZE];
	struct coap_packet response;
	uint8_t code = zephlet_coap_map_return_code(rc, true);

	int err = build_response_header(req, buf, sizeof(buf), code, &response);
	if (err < 0) {
		LOG_WRN("coap_packet_init failed: %d", err);
		return err;
	}

	err = append_errno_option(&response, rc);
	if (err < 0) {
		LOG_WRN("append errno option failed: %d", err);
		return err;
	}

	err = coap_append_option_int(&response, COAP_OPTION_CONTENT_FORMAT, ZEPHLET_COAP_CT_NANOPB);
	if (err < 0) {
		LOG_WRN("append content-format option failed: %d", err);
		return err;
	}

	err = coap_packet_append_payload_marker(&response);
	if (err < 0) {
		LOG_WRN("append payload marker failed: %d", err);
		return err;
	}

	err = coap_packet_append_payload(&response, (uint8_t *)payload, (uint16_t)payload_len);
	if (err < 0) {
		LOG_WRN("append payload failed: %d", err);
		return err;
	}

	return coap_resource_send(res, &response, addr, addr_len, NULL);
}
