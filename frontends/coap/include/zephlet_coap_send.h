#ifndef ZEPHLET_FRONTENDS_COAP_SEND_H
#define ZEPHLET_FRONTENDS_COAP_SEND_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/net/coap.h>
#include <zephyr/net/coap_service.h>

/**
 * @file
 * @brief Shared CoAP response helpers for the zephlet frontend.
 *
 * Type-agnostic helpers used by every codegen-emitted per-type RPC
 * handler. They own the CoAP packet construction (response init,
 * token/id propagation from the request, code mapping, errno option,
 * Content-Format option, payload) so the per-type handler can stay
 * focused on the proto decode/encode and the zbus publish.
 */

/**
 * @brief Build and send a CoAP response carrying just a result code.
 *
 * Maps @p rc to a CoAP response code via `zephlet_coap_map_return_code`,
 * appends the verbatim-errno option (number `ZEPHLET_COAP_OPT_ERRNO`)
 * carrying @p rc as `uint32_t`, and sends. No payload, no
 * Content-Format option.
 *
 * @param res       The matched `coap_resource` from the inbound
 *                  handler signature.
 * @param req       The inbound request packet (token + message id are
 *                  copied into the response).
 * @param addr      Inbound peer address (forwarded to
 *                  `coap_resource_send`).
 * @param addr_len  Length of @p addr.
 * @param rc        Handler return code. `rc == 0` produces 2.04
 *                  Changed; negative errno is mapped per the table in
 *                  `zephlet_coap_map_return_code`.
 *
 * @return 0 on success; negative errno from the underlying CoAP
 *         primitives on failure.
 */
int zephlet_coap_send_error(struct coap_resource *res, struct coap_packet *req,
			    struct sockaddr *addr, socklen_t addr_len, int rc);

/**
 * @brief Build and send a CoAP response carrying a result code plus
 *        payload bytes.
 *
 * @p rc == 0 + @p payload_len > 0 produces 2.05 Content with a
 * Content-Format option set to `ZEPHLET_COAP_CT_NANOPB`. @p rc == 0
 * with @p payload_len == 0 collapses to the no-payload `send_error`
 * equivalent (2.04 Changed). Negative @p rc is treated the same as
 * `send_error`, i.e. the payload is dropped.
 *
 * The verbatim-errno option is always appended.
 *
 * @param res          Matched `coap_resource`.
 * @param req          Inbound request packet.
 * @param addr         Inbound peer address.
 * @param addr_len     Length of @p addr.
 * @param rc           Handler return code.
 * @param payload      Encoded payload bytes (may be NULL when
 *                     @p payload_len == 0).
 * @param payload_len  Number of payload bytes.
 *
 * @return 0 on success; negative errno on failure.
 */
int zephlet_coap_send_response(struct coap_resource *res, struct coap_packet *req,
			       struct sockaddr *addr, socklen_t addr_len, int rc,
			       const uint8_t *payload, size_t payload_len);

/**
 * @brief Build and send a CoAP response with a caller-supplied
 *        Content-Format ID.
 *
 * Mirrors `zephlet_coap_send_response` but takes the Content-Format as a
 * parameter instead of hardcoding `ZEPHLET_COAP_CT_NANOPB`. Intended for
 * resources whose payload is not nanopb-encoded — currently the
 * `/.well-known/core` discovery handler, which returns
 * `application/link-format` (`ZEPHLET_COAP_CT_LINK_FORMAT`).
 *
 * @param ct_id  Content-Format ID written into the Content-Format option.
 *               Only consulted when @p rc == 0 and @p payload_len > 0;
 *               for non-success or empty replies, the call collapses to
 *               `zephlet_coap_send_error`.
 *
 * Other parameters and the return contract match `zephlet_coap_send_response`.
 */
int zephlet_coap_send_payload_ct(struct coap_resource *res, struct coap_packet *req,
				 struct sockaddr *addr, socklen_t addr_len, int rc,
				 uint32_t ct_id, const uint8_t *payload, size_t payload_len);

#endif /* ZEPHLET_FRONTENDS_COAP_SEND_H */
