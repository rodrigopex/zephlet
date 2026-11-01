#ifndef ZEPHLET_FRONTENDS_COAP_BLOCK2_H
#define ZEPHLET_FRONTENDS_COAP_BLOCK2_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/net/coap.h>
#include <zephyr/net/coap_service.h>

/**
 * @file
 * @brief Block-wise responses (RFC 7959 Block2) of the zephlet frontend.
 *
 * Compiled with `CONFIG_ZEPHLETS_COAP_BLOCK2=y`. A response longer than
 * the block size goes out as blocks of `CONFIG_ZEPHLETS_COAP_BLOCK2_SIZE`
 * bytes, or of the smaller size the client asks for in its Block2 option.
 *
 * - A POST response (an rpc) is kept for the client, keyed by its address,
 *   URI path, URI query and request payload; a later block comes from that
 *   copy and the rpc does not run again.
 * - A GET response (discovery) is built again for each block and cut.
 * - Every block carries ETag, Block2 and Size2.
 */

/**
 * @brief Serve a later block of a kept rpc response.
 *
 * @param res       The matched `coap_resource`.
 * @param req       The inbound request.
 * @param addr      Peer address.
 * @param addr_len  Length of @p addr.
 *
 * @retval 0  Not a later block of a POST: the handler runs the rpc.
 * @retval 1  Answered: the block, 4.08 when no kept response matches, or
 *            4.02 for a block past the end.
 * @retval <0 The response could not be sent.
 */
int zephlet_coap_block2_continue(struct coap_resource *res, struct coap_packet *req,
				 struct sockaddr *addr, socklen_t addr_len);

/**
 * @brief Send @p payload as one block when it does not fit a block.
 *
 * @param res       The matched `coap_resource`.
 * @param req       The inbound request.
 * @param addr      Peer address.
 * @param addr_len  Length of @p addr.
 * @param code      CoAP response code of a whole response.
 * @param rc        Return code for the errno option.
 * @param ct_id     Content-Format of @p payload.
 * @param payload   The whole response body.
 * @param len       Length of @p payload.
 *
 * @retval 0  The payload fits one block and no later block was asked:
 *            the caller sends it whole.
 * @retval 1  Answered: the block, or 4.02 for a block past the end.
 * @retval <0 The response could not be kept or sent; -EMSGSIZE for a POST
 *            response over `CONFIG_ZEPHLETS_COAP_BLOCK2_MAX_SIZE`.
 */
int zephlet_coap_block2_send(struct coap_resource *res, struct coap_packet *req,
			     struct sockaddr *addr, socklen_t addr_len, uint8_t code, int rc,
			     uint32_t ct_id, const uint8_t *payload, size_t len);

#endif /* ZEPHLET_FRONTENDS_COAP_BLOCK2_H */
