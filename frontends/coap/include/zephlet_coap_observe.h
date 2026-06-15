#ifndef ZEPHLET_FRONTENDS_COAP_OBSERVE_H
#define ZEPHLET_FRONTENDS_COAP_OBSERVE_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/net/coap.h>
#include <zephyr/net/coap_service.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/slist.h>

#include "zephlet.h"

/**
 * @file
 * @brief CoAP Observe (RFC 7641) event bridge for opted-in zephlets.
 *
 * Delivers a zephlet instance's `events` channel to subscribed CoAP
 * clients as NON notifications. The codegen-emitted per-type interface
 * owns one `struct zephlet_coap_instance_state` per instance (a section
 * iterable) and calls the helpers below; this translation unit owns the
 * global subscriber pool and the notification fan-out.
 *
 * Subscriber records are allocated from a `k_mem_slab`
 * (`CONFIG_ZEPHLETS_COAP_MAX_OBSERVERS` blocks) and chained onto the
 * owning instance's `observers` list. The per-event sequence counter is
 * the loss signal: it advances once per event, so a gap in the values a
 * subscriber receives marks a missed notification.
 */

/**
 * @brief Per-instance Observe bookkeeping.
 *
 * One section-iterable record is emitted per CoAP-opted-in
 * `ZEPHLET_NEW(...)` instance by the codegen hook. The dispatcher and
 * the event callback locate it by walking
 * `STRUCT_SECTION_FOREACH(zephlet_coap_instance_state, s)` and matching
 * `s->z`.
 *
 * `observe_seq` advances once per emitted event (masked to the 24-bit
 * CoAP Observe range); subscribers detect loss as gaps in the values
 * they receive. `observers` chains the active `struct zlet_observe_sub`
 * records for this instance.
 */
struct zephlet_coap_instance_state {
	const struct zephlet *z;
	atomic_t observe_seq;
	sys_slist_t observers;
};

/**
 * @brief Locate the Observe state record owning @p z.
 *
 * @param z Instance to look up.
 *
 * @return The matching record, or NULL if @p z is not CoAP-opted-in.
 */
struct zephlet_coap_instance_state *zephlet_coap_state_for(const struct zephlet *z);

/**
 * @brief Handle a GET that carries the Observe option for an instance's
 *        `/zlet/{type}/{instance}/events` resource.
 *
 * Observe value 0 registers (or refreshes) a subscriber and answers 2.05
 * Content carrying the current sequence value; value 1 deregisters the
 * matching subscriber and answers a bare 2.05 with no Observe option. A
 * register that finds no free slab block is answered 5.03.
 *
 * @param res       Matched `coap_resource`.
 * @param req       Inbound request packet.
 * @param addr      Inbound peer address.
 * @param addr_len  Length of @p addr.
 * @param z         Resolved instance the resource addresses.
 *
 * @return 0 on a sent response; `-ENOENT` when @p req carries no Observe
 *         option (the caller decides how to answer a plain GET);
 *         negative errno from the CoAP primitives on send failure.
 */
int zephlet_coap_observe_handle_get(struct coap_resource *res, struct coap_packet *req,
				    struct sockaddr *addr, socklen_t addr_len,
				    const struct zephlet *z);

/**
 * @brief Fan an encoded event out to @p z's current subscribers.
 *
 * Advances @p z's sequence counter once, then sends one NON 2.05
 * notification per subscriber carrying the Observe option and @p payload.
 * Runs from the events-channel async listener (system workqueue). Send
 * failures are logged; the sequence gap they leave is the client-visible
 * loss signal.
 *
 * @param res          The type's `coap_resource` (notifications are sent
 *                     through it).
 * @param z            Instance whose event fired.
 * @param payload      Encoded event bytes (the per-type nanopb message).
 * @param payload_len  Number of payload bytes (0 for an empty event).
 */
void zephlet_coap_observe_notify(struct coap_resource *res, const struct zephlet *z,
				 const uint8_t *payload, size_t payload_len);

#endif /* ZEPHLET_FRONTENDS_COAP_OBSERVE_H */
