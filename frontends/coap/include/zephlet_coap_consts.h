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

/** Content-Format ID for `application/link-format` (RFC 6690 §7.2).
 *  Carried on responses from the `/.well-known/core` discovery resource. */
#define ZEPHLET_COAP_CT_LINK_FORMAT 40

/** Content-Format ID for `text/plain;charset=utf-8` (RFC 7252 §12.3).
 *  Carried on the shared `/zlet/apis` resource whose body is a
 *  newline-separated list of base lifecycle method names. */
#define ZEPHLET_COAP_CT_TEXT_PLAIN 0

/** Custom CoAP option number carrying the raw POSIX errno verbatim
 *  (signed 32-bit).
 *
 *  Properties (per RFC 7252 §5.4.6 — encoded in the option number's
 *  low bits, NOT free parameters):
 *    - bit 0 = 0 → Elective (recipients that don't recognize it MUST
 *      silently ignore the option, not reject the message).
 *    - bit 1 = 0 → Safe-to-Forward (proxies may forward).
 *    - bits 2-4 = `111` → NoCacheKey (option excluded from the
 *      proxy cache key).
 *
 *  `65052 = 0xFE1C`, low 5 bits = `11100`, matches the spec above.
 *  Errno carry is best-effort debug info; Critical/Unsafe would force
 *  the recipient to reject the whole response on unknown-option, which
 *  defeats the purpose. */
#define ZEPHLET_COAP_OPT_ERRNO 65052

/** Base URI segment for all zephlet resources: `/zlet/...`. */
#define ZEPHLET_COAP_BASE_PATH "zlet"

/** URI suffix segment for an instance's event stream:
 *  `/zlet/{type}/{instance}/events`. */
#define ZEPHLET_COAP_EVENTS_SUFFIX "events"

/** URI suffix segments for an instance's stats resource:
 *  `/zlet/{type}/{instance}/events/stats`. */
#define ZEPHLET_COAP_STATS_SUFFIX "events/stats"

/** `rt=` link attribute for an RPC resource exposed by a discoverable
 *  type (per custom method, emitted by `/zlet/{type}/apis`). Base
 *  lifecycle methods are advertised separately via the shared
 *  `/zlet/apis` resource and do not appear in per-type `/apis`. */
#define ZEPHLET_COAP_RT_RPC "zlet.rpc"

/** `rt=` link attribute for an events resource (per instance). */
#define ZEPHLET_COAP_RT_EVENT "zlet.event"

/** `rt=` link attribute for a stats resource (per instance). */
#define ZEPHLET_COAP_RT_STATS "zlet.stats"

#endif /* ZEPHLET_FRONTENDS_COAP_CONSTS_H */
