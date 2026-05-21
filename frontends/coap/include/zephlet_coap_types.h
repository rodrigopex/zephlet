#ifndef ZEPHLET_FRONTENDS_COAP_TYPES_H
#define ZEPHLET_FRONTENDS_COAP_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include <pb.h>

#include "zephlet.h"

/**
 * @file
 * @brief Runtime types populated by the codegen-emitted
 *        `<prefix>_coap_interface.c` for every CoAP-opted-in zephlet type.
 *
 * Section iterables walked by the CoAP frontend dispatcher (Phase 3+):
 *   STRUCT_SECTION_FOREACH(zephlet_coap_type, t) { ... }
 *
 * One `struct zephlet_coap_type` record exists per opted-in zephlet *type*
 * (not per instance). Instances are resolved via `zephlet_get_by_name()`
 * and then validated against the type via `z->api == t->api`.
 */

/**
 * @brief One RPC method exposed over CoAP.
 *
 * `path_segment` is the URL-visible method name (snake_case, as declared
 * in the .proto service block). `method_id` indexes into
 * `zephlet_api.methods` for the dispatch trampoline. `req_desc` /
 * `resp_desc` are the nanopb descriptors used by the Phase 2 envelope
 * decode/encode. `req_max_size` / `resp_max_size` are the nanopb-
 * computed maximum encoded sizes (from `<MSG>_SIZE` in `<prefix>.pb.h`)
 * — zero for `Empty` messages. Phase 2 decoder rejects request bodies
 * larger than `req_max_size` with `-EMSGSIZE`; Phase 3 dispatcher uses
 * `resp_max_size` to size per-type response scratch.
 */
struct zephlet_coap_method {
	const char *path_segment;
	uint16_t method_id;
	const pb_msgdesc_t *req_desc;
	const pb_msgdesc_t *resp_desc;
	size_t req_max_size;
	size_t resp_max_size;
};

/**
 * @brief One zephlet type's CoAP surface.
 *
 * `type_name` is the snake-cased type symbol used as the second URI
 * segment (`/zlet/{type}/...`). `api` is the matching dispatch table —
 * the runtime defensive check `z->api == t->api` rejects type/instance
 * mismatches with `4.04`.
 */
struct zephlet_coap_type {
	const char *type_name;
	const struct zephlet_api *api;
	const struct zephlet_coap_method *methods;
	size_t num_methods;
};

#endif /* ZEPHLET_FRONTENDS_COAP_TYPES_H */
