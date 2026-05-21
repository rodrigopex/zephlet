#ifndef ZEPHLET_FRONTENDS_COAP_TRANSLATE_H
#define ZEPHLET_FRONTENDS_COAP_TRANSLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/net/coap.h>

#include "zephlet.h"
#include "zephlet_coap_types.h"

/**
 * @file
 * @brief Pure envelope ↔ CoAP payload translation for the CoAP frontend.
 *
 * Transport-free helpers used by the Phase 3 catch-all dispatcher and the
 * Phase 5 events bridge. None of these functions touch `coap_service`,
 * sockets, or zbus — they operate on raw buffers and pre-initialised
 * `struct coap_packet` instances, so they are unit-testable in isolation.
 *
 * Wire format: request and response bodies are nanopb-encoded protobuf
 * messages described by the same `pb_msgdesc_t` descriptors that
 * `<prefix>_interface.c` references for local dispatch. Content-Format
 * `ZEPHLET_COAP_CT_NANOPB` (65001) tags outbound payloads.
 */

/**
 * @brief Map a zephlet handler return code to a CoAP response code byte.
 *
 * The mapping table is the authoritative one pinned in the CoAP frontend
 * adoption plan:
 *
 *   rc ==  0, has_payload  → 2.05 Content
 *   rc ==  0, no payload   → 2.04 Changed
 *   rc == -EINVAL          → 4.00 Bad Request
 *   rc == -ENODEV          → 4.04 Not Found
 *   rc == -ENOSYS          → 4.05 Method Not Allowed
 *   rc == -EALREADY        → 4.09 Conflict
 *   rc == -EMSGSIZE        → 4.13 Request Entity Too Large
 *   rc == -EBUSY/-EAGAIN   → 5.03 Service Unavailable
 *   rc == -ETIMEDOUT       → 5.04 Gateway Timeout
 *   rc == -ENOMEM          → 5.00 Internal Server Error
 *   any other negative rc  → 5.00 Internal Server Error (default)
 *
 * Positive rc values are not produced by zephlet handlers; they are
 * treated the same as "any other".
 *
 * @param rc           Handler return code (POSIX errno; 0 == success).
 * @param has_payload  Only consulted when @p rc == 0. True selects 2.05;
 *                     false selects 2.04.
 *
 * @return CoAP response code byte (`COAP_MAKE_RESPONSE_CODE(class, detail)`).
 */
uint8_t zephlet_coap_map_return_code(int32_t rc, bool has_payload);

/**
 * @brief Decode a CoAP request body into a `struct zephlet_call`.
 *
 * On success the function fills @p call with `method_id`, `req_desc`,
 * `resp_desc`, and `req = req_storage`. The caller is responsible for
 * `resp` / `resp_desc` storage and for any prior CoAP-side validation
 * (URI segments, Content-Format option).
 *
 *   - `len > m->req_max_size` → `-EMSGSIZE`; dispatcher surfaces 4.13.
 *     The bound is the codegen-emitted `<MSG>_SIZE` from nanopb, so the
 *     decoder enforces the exact per-method envelope (Empty requests
 *     have `req_max_size == 0`, so any inbound body is rejected here).
 *   - nanopb decode failure (truncated, malformed varints, type
 *     mismatch) → `-EINVAL`; dispatcher surfaces 4.00.
 *
 * @param buf          CoAP payload bytes (may be NULL if @p len == 0).
 * @param len          Number of payload bytes.
 * @param m            Method descriptor (matched
 *                     `zephlet_coap_method`). Must be non-NULL.
 * @param call         Envelope to populate. Must be non-NULL.
 * @param req_storage  Caller-owned request struct storage (e.g. stack-
 *                     allocated `struct <Type>Req`). Must be non-NULL
 *                     when `m->req_desc != NULL`.
 *
 * @return 0 on success;
 *         `-EINVAL` on malformed body or NULL-argument violation;
 *         `-EMSGSIZE` on oversized body.
 */
int zephlet_coap_decode_request(const uint8_t *buf, size_t len, const struct zephlet_coap_method *m,
				struct zephlet_call *call, void *req_storage);

/**
 * @brief Encode a post-dispatch envelope into a CoAP response packet.
 *
 * @p cpkt must already have been initialised by the caller via
 * `coap_packet_init` with the appropriate version, type, token, and
 * message ID (matching the request). This helper:
 *
 *   1. Sets the response code from `call->return_code` via
 *      `zephlet_coap_map_return_code`.
 *   2. Always appends the verbatim-errno option
 *      (`ZEPHLET_COAP_OPT_ERRNO = 65052`) carrying `call->return_code`
 *      reinterpreted as `uint32_t`. The receiver re-casts to `int32_t`.
 *   3. When `call->return_code == 0` and `call->resp_desc != NULL`,
 *      encodes the response payload through @p scratch using nanopb. If
 *      the encoded size is greater than zero, the response code is
 *      upgraded to 2.05 Content, a Content-Format option (65001) is
 *      appended, the payload marker is written, and the encoded bytes
 *      are appended.
 *
 * @p scratch is required only when the caller can produce a non-empty
 * payload (success + non-NULL `resp_desc`). Size it from nanopb's
 * compile-time `<msg>_size` macros for the relevant zephlet type's
 * largest opted-in response message.
 *
 * @param call         Envelope to serialise (must be non-NULL).
 * @param cpkt         Pre-initialised CoAP packet (must be non-NULL).
 * @param scratch      Encoder scratch buffer (may be NULL when no
 *                     payload is expected).
 * @param scratch_size Size of @p scratch in bytes.
 *
 * @return 0 on success;
 *         `-ENOMEM` if either @p scratch is insufficient for the encoded
 *         response or the CoAP packet's underlying buffer cannot fit the
 *         options/payload.
 */
int zephlet_coap_encode_response(const struct zephlet_call *call, struct coap_packet *cpkt,
				 uint8_t *scratch, size_t scratch_size);

#endif /* ZEPHLET_FRONTENDS_COAP_TRANSLATE_H */
