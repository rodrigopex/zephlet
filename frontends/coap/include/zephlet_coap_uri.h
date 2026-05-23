#ifndef ZEPHLET_FRONTENDS_COAP_URI_H
#define ZEPHLET_FRONTENDS_COAP_URI_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/net/coap.h>

/**
 * @file
 * @brief URI-path helpers for codegen-emitted zephlet CoAP handlers.
 *
 * Allocation-free; works against caller-provided `struct coap_option`
 * storage. Segment values are NUL-terminated into caller stack buffers
 * so the handler body can `strcmp` directly.
 */

/**
 * @brief Decode a request's URI-path into a caller-provided option array.
 *
 * Thin wrapper around `coap_find_options(COAP_OPTION_URI_PATH, ...)`
 * with bounds-checking. The caller sizes @p opts at compile time;
 * `CONFIG_ZEPHLETS_COAP_MAX_URI_SEGMENTS` is the standard ceiling.
 *
 * @param req   Inbound request packet.
 * @param opts  Caller-provided option storage.
 * @param max   Capacity of @p opts.
 * @param count Set to the number of segments found (only when the
 *              function returns 0).
 *
 * @return 0 on success;
 *         `-E2BIG` if @p req carries more URI segments than @p max;
 *         negative errno on transport failure.
 */
int zephlet_coap_parse_uri_path(struct coap_packet *req, struct coap_option *opts, size_t max,
				size_t *count);

/**
 * @brief Copy a `coap_option` value into a NUL-terminated C-string buffer.
 *
 * @param opt       Parsed URI-path option.
 * @param buf       Destination buffer.
 * @param buf_size  Size of @p buf in bytes (must accommodate
 *                  `opt->len + 1`).
 *
 * @return 0 on success;
 *         `-EINVAL` on NULL inputs or zero-sized buffer;
 *         `-ENOBUFS` if `opt->len + 1 > buf_size` (caller surfaces 4.04).
 */
int zephlet_coap_option_to_cstr(const struct coap_option *opt, char *buf, size_t buf_size);

#endif /* ZEPHLET_FRONTENDS_COAP_URI_H */
