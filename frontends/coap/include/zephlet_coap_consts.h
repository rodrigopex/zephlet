#ifndef ZEPHLET_FRONTENDS_COAP_CONSTS_H
#define ZEPHLET_FRONTENDS_COAP_CONSTS_H

/**
 * @file
 * @brief Pinned wire-level constants for the CoAP frontend.
 *
 * Single source of truth for Content-Format ID, custom option numbers,
 * URI base path, suffixes, and `rt=` strings. Every CoAP frontend TU
 * pulls names from here — no magic literals in templates or runtime.
 */

/** Content-Format ID for nanopb-encoded zephlet payloads (RFC 7252 §12.3
 *  experimental range, 65000-65535). */
#define ZEPHLET_COAP_CT_NANOPB 65001

/** Custom CoAP option number carrying the raw POSIX errno verbatim
 *  (signed 32-bit). Critical=0, Unsafe=0, NoCacheKey=1, Repeatable=0. */
#define ZEPHLET_COAP_OPT_ERRNO 65003

/** Base URI segment for all zephlet resources: `/zlet/...`. */
#define ZEPHLET_COAP_BASE_PATH "zlet"

/** URI suffix segment for an instance's event stream:
 *  `/zlet/{type}/{instance}/events`. */
#define ZEPHLET_COAP_EVENTS_SUFFIX "events"

/** URI suffix segments for an instance's stats resource:
 *  `/zlet/{type}/{instance}/events/stats`. */
#define ZEPHLET_COAP_STATS_SUFFIX "events/stats"

/** `rt=` link attribute for an RPC resource (per opted-in method). */
#define ZEPHLET_COAP_RT_RPC "zlet.rpc"

/** `rt=` link attribute for an events resource (per instance). */
#define ZEPHLET_COAP_RT_EVENT "zlet.event"

/** `rt=` link attribute for a stats resource (per instance). */
#define ZEPHLET_COAP_RT_STATS "zlet.stats"

#endif /* ZEPHLET_FRONTENDS_COAP_CONSTS_H */
