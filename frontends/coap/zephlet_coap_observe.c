#include "zephlet_coap_observe.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_service.h>
#include <zephyr/sys/util.h>

#include "zephlet_coap_consts.h"
#include "zephlet_coap_send.h"

LOG_MODULE_DECLARE(zlet_coap);

/**
 * @file
 * @brief CoAP Observe subscriber pool and notification fan-out.
 *
 * Subscriber records live in a single `k_mem_slab` shared across every
 * opted-in instance; each is chained onto its instance's `observers`
 * list. A `k_spinlock` guards the (short) list mutations and the snapshot
 * walk — the blocking send runs after the snapshot, outside the lock.
 *
 * Dropped notifications are not counted: a send failure (or any loss
 * downstream) simply leaves a gap in the per-event sequence values the
 * subscriber receives, which is the client-visible loss signal. Retry
 * and explicit drop accounting are intentionally left for a follow-up.
 */

/** One CoAP Observe subscriber: peer endpoint + token, slab-allocated. */
struct zlet_observe_sub {
	sys_snode_t node;
	struct net_sockaddr_storage addr;
	socklen_t addr_len;
	uint8_t token[COAP_TOKEN_MAX_LEN];
	uint8_t tkl;
};

#define ZLET_OBSERVE_SUB_ALIGN sizeof(void *)

K_MEM_SLAB_DEFINE(zlet_observe_slab,
		  ROUND_UP(sizeof(struct zlet_observe_sub), ZLET_OBSERVE_SUB_ALIGN),
		  CONFIG_ZEPHLETS_COAP_MAX_OBSERVERS, ZLET_OBSERVE_SUB_ALIGN);

static struct k_spinlock zlet_observe_lock;

/*
 * Notification scratch reused across sends. The events-channel async
 * listener runs on the system workqueue, which executes work items one at
 * a time, so a notify() call never overlaps another — these file-static
 * buffers need no lock and stay off the (smaller) workqueue stack.
 */
static uint8_t zlet_notify_buf[CONFIG_COAP_SERVER_MESSAGE_SIZE];

struct zlet_observe_target {
	struct net_sockaddr_storage addr;
	socklen_t addr_len;
	uint8_t token[COAP_TOKEN_MAX_LEN];
	uint8_t tkl;
};

static struct zlet_observe_target zlet_notify_targets[CONFIG_ZEPHLETS_COAP_MAX_OBSERVERS];

struct zephlet_coap_instance_state *zephlet_coap_state_for(const struct zephlet *z)
{
	STRUCT_SECTION_FOREACH(zephlet_coap_instance_state, s) {
		if (s->z == z) {
			return s;
		}
	}
	return NULL;
}

/** Find a subscriber of @p state matching @p addr + @p token. Caller
 *  holds `zlet_observe_lock`. */
static struct zlet_observe_sub *zlet_find_sub(struct zephlet_coap_instance_state *state,
					      const struct sockaddr *addr, socklen_t addr_len,
					      const uint8_t *token, uint8_t tkl)
{
	struct zlet_observe_sub *sub;

	SYS_SLIST_FOR_EACH_CONTAINER(&state->observers, sub, node) {
		if (sub->addr_len == addr_len && sub->tkl == tkl &&
		    memcmp(&sub->addr, addr, addr_len) == 0 &&
		    memcmp(sub->token, token, tkl) == 0) {
			return sub;
		}
	}
	return NULL;
}

/** Build + send a 2.05 Content response (empty body), optionally carrying
 *  the Observe option. Runs on the CoAP server thread. */
static int zlet_send_2_05(struct coap_resource *res, struct coap_packet *req, struct sockaddr *addr,
			  socklen_t addr_len, bool with_observe, uint32_t seq)
{
	uint8_t buf[CONFIG_COAP_SERVER_MESSAGE_SIZE];
	struct coap_packet response;
	uint8_t token[COAP_TOKEN_MAX_LEN];
	uint8_t tkl = coap_header_get_token(req, token);
	uint16_t id = coap_header_get_id(req);
	uint8_t req_type = coap_header_get_type(req);
	uint8_t resp_type = (req_type == COAP_TYPE_CON) ? COAP_TYPE_ACK : COAP_TYPE_NON_CON;

	int err = coap_packet_init(&response, buf, sizeof(buf), COAP_VERSION_1, resp_type, tkl,
				   token, COAP_RESPONSE_CODE_CONTENT, id);
	if (err < 0) {
		return err;
	}

	if (with_observe) {
		err = coap_append_option_int(&response, COAP_OPTION_OBSERVE, seq);
		if (err < 0) {
			return err;
		}
	}

	return coap_resource_send(res, &response, addr, addr_len, NULL);
}

int zephlet_coap_observe_handle_get(struct coap_resource *res, struct coap_packet *req,
				    struct sockaddr *addr, socklen_t addr_len,
				    const struct zephlet *z)
{
	int observe = coap_get_option_int(req, COAP_OPTION_OBSERVE);
	if (observe < 0) {
		/* Plain GET with no Observe option: the caller decides. */
		return -ENOENT;
	}

	struct zephlet_coap_instance_state *state = zephlet_coap_state_for(z);
	if (state == NULL) {
		return zephlet_coap_send_error(res, req, addr, addr_len, -ENODEV);
	}

	uint8_t token[COAP_TOKEN_MAX_LEN];
	uint8_t tkl = coap_header_get_token(req, token);

	if (observe == 0) {
		/* Register, or refresh an already-registered subscriber. */
		k_spinlock_key_t key = k_spin_lock(&zlet_observe_lock);
		struct zlet_observe_sub *sub = zlet_find_sub(state, addr, addr_len, token, tkl);

		if (sub == NULL) {
			void *block;

			if (k_mem_slab_alloc(&zlet_observe_slab, &block, K_NO_WAIT) != 0) {
				k_spin_unlock(&zlet_observe_lock, key);
				LOG_WRN("%s: observe pool full", z->name);
				return zephlet_coap_send_error(res, req, addr, addr_len, -EBUSY);
			}

			sub = block;
			memcpy(&sub->addr, addr, addr_len);
			sub->addr_len = addr_len;
			memcpy(sub->token, token, tkl);
			sub->tkl = tkl;
			sys_slist_append(&state->observers, &sub->node);
		}

		uint32_t seq = (uint32_t)atomic_get(&state->observe_seq) & 0xFFFFFF;
		k_spin_unlock(&zlet_observe_lock, key);

		return zlet_send_2_05(res, req, addr, addr_len, true, seq);
	}

	/* Any non-zero Observe value deregisters (RFC 7641 §3.6 uses 1). */
	k_spinlock_key_t key = k_spin_lock(&zlet_observe_lock);
	struct zlet_observe_sub *sub = zlet_find_sub(state, addr, addr_len, token, tkl);
	if (sub != NULL) {
		sys_slist_find_and_remove(&state->observers, &sub->node);
	}
	k_spin_unlock(&zlet_observe_lock, key);

	if (sub != NULL) {
		k_mem_slab_free(&zlet_observe_slab, sub);
	}

	return zlet_send_2_05(res, req, addr, addr_len, false, 0);
}

void zephlet_coap_observe_notify(struct coap_resource *res, const struct zephlet *z,
				 const uint8_t *payload, size_t payload_len)
{
	struct zephlet_coap_instance_state *state = zephlet_coap_state_for(z);
	if (state == NULL) {
		return;
	}

	/* One sequence value per event, shared by all of this instance's
	 * subscribers. Post-increment so the value is strictly greater than
	 * the one handed out in the subscribe response. */
	uint32_t seq = (uint32_t)(atomic_inc(&state->observe_seq) + 1) & 0xFFFFFF;

	size_t count = 0;
	k_spinlock_key_t key = k_spin_lock(&zlet_observe_lock);
	struct zlet_observe_sub *sub;
	SYS_SLIST_FOR_EACH_CONTAINER(&state->observers, sub, node) {
		if (count >= ARRAY_SIZE(zlet_notify_targets)) {
			break;
		}
		memcpy(&zlet_notify_targets[count].addr, &sub->addr, sub->addr_len);
		zlet_notify_targets[count].addr_len = sub->addr_len;
		memcpy(zlet_notify_targets[count].token, sub->token, sub->tkl);
		zlet_notify_targets[count].tkl = sub->tkl;
		count++;
	}
	k_spin_unlock(&zlet_observe_lock, key);

	for (size_t i = 0; i < count; i++) {
		struct zlet_observe_target *t = &zlet_notify_targets[i];
		struct coap_packet pkt;
		int err = coap_packet_init(&pkt, zlet_notify_buf, sizeof(zlet_notify_buf),
					   COAP_VERSION_1, COAP_TYPE_NON_CON, t->tkl, t->token,
					   COAP_RESPONSE_CODE_CONTENT, coap_next_id());
		if (err < 0) {
			LOG_WRN("%s: observe packet init failed (%d)", z->name, err);
			continue;
		}

		err = coap_append_option_int(&pkt, COAP_OPTION_OBSERVE, seq);
		if (err < 0) {
			LOG_WRN("%s: observe option failed (%d)", z->name, err);
			continue;
		}

		err = coap_append_option_int(&pkt, COAP_OPTION_CONTENT_FORMAT,
					     ZEPHLET_COAP_CT_NANOPB);
		if (err < 0) {
			LOG_WRN("%s: content-format option failed (%d)", z->name, err);
			continue;
		}

		if (payload_len > 0) {
			err = coap_packet_append_payload_marker(&pkt);
			if (err < 0) {
				LOG_WRN("%s: payload marker failed (%d)", z->name, err);
				continue;
			}

			err = coap_packet_append_payload(&pkt, (uint8_t *)payload,
							 (uint16_t)payload_len);
			if (err < 0) {
				LOG_WRN("%s: payload append failed (%d)", z->name, err);
				continue;
			}
		}

		err = coap_resource_send(res, &pkt, (struct sockaddr *)&t->addr, t->addr_len, NULL);
		if (err < 0) {
			LOG_WRN("%s: observe send drop (%d)", z->name, err);
		}
	}
}
