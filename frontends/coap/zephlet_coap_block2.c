#include "zephlet_coap_block2.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "zephlet_coap_consts.h"

LOG_MODULE_DECLARE(zlet_coap);

/**
 * @file
 * @brief Block-wise responses (RFC 7959 Block2).
 *
 * The kept rpc responses live in a fixed table of
 * `CONFIG_ZEPHLETS_COAP_BLOCK2_ENTRIES` entries. An entry expires
 * `CONFIG_ZEPHLETS_COAP_BLOCK2_LIFETIME_MS` after its block 0; a new
 * response takes an expired entry, else the one that expires first.
 */

#define BLOCK_SIZE CONFIG_ZEPHLETS_COAP_BLOCK2_SIZE

BUILD_ASSERT(IS_POWER_OF_TWO(BLOCK_SIZE) && (BLOCK_SIZE >= 16) && (BLOCK_SIZE <= 1024),
	     "CONFIG_ZEPHLETS_COAP_BLOCK2_SIZE must be 16, 32, 64, 128, 256, 512 or 1024");

/* Header, token, ETag, Content-Format, Block2, Size2, errno option and the
 * payload marker, with room to spare. */
#define BLOCK_HEADROOM 48

struct kept_response {
	bool used;
	struct sockaddr_storage peer;
	socklen_t peer_len;
	uint32_t key;
	uint32_t etag;
	uint32_t ct_id;
	int64_t expires;
	size_t len;
	uint8_t body[CONFIG_ZEPHLETS_COAP_BLOCK2_MAX_SIZE];
};

static struct kept_response kept[CONFIG_ZEPHLETS_COAP_BLOCK2_ENTRIES];
static uint32_t next_etag;
static K_MUTEX_DEFINE(kept_lock);

/* The block the request asks for: number and size, @p size capped at
 * BLOCK_SIZE. No Block2 option asks for block 0 at BLOCK_SIZE. */
static bool requested_block(const struct coap_packet *req, uint32_t *num, size_t *size)
{
	bool more;
	int bytes = coap_get_block2_option(req, &more, num);

	if (bytes < 0) {
		*num = 0;
		*size = BLOCK_SIZE;
		return false;
	}
	*size = MIN((size_t)bytes, (size_t)BLOCK_SIZE);
	return true;
}

/* CRC-32 of the URI path, the URI query and the payload: what makes two
 * requests ask for the same response. */
static uint32_t request_key(const struct coap_packet *req)
{
	static const uint16_t numbers[] = {COAP_OPTION_URI_PATH, COAP_OPTION_URI_QUERY};
	struct coap_option opts[CONFIG_ZEPHLETS_COAP_MAX_URI_SEGMENTS];
	uint32_t key = 0;
	uint16_t len;
	const uint8_t *payload;

	ARRAY_FOR_EACH(numbers, i) {
		int n = coap_find_options(req, numbers[i], opts, ARRAY_SIZE(opts));

		for (int j = 0; j < n; j++) {
			key = crc32_ieee_update(key, opts[j].value, opts[j].len);
			key = crc32_ieee_update(key, (const uint8_t *)"/", 1);
		}
		key = crc32_ieee_update(key, (const uint8_t *)"?", 1);
	}
	payload = coap_packet_get_payload(req, &len);
	if (payload != NULL) {
		key = crc32_ieee_update(key, payload, len);
	}
	return key;
}

static bool same_peer(const struct kept_response *k, const struct sockaddr *addr,
		      socklen_t addr_len)
{
	return (k->peer_len == addr_len) && (memcmp(&k->peer, addr, addr_len) == 0);
}

static int init_response(const struct coap_packet *req, uint8_t *buf, size_t size, uint8_t code,
			 struct coap_packet *out)
{
	uint8_t token[COAP_TOKEN_MAX_LEN];
	uint8_t tkl = coap_header_get_token(req, token);
	uint8_t type =
		(coap_header_get_type(req) == COAP_TYPE_CON) ? COAP_TYPE_ACK : COAP_TYPE_NON_CON;

	return coap_packet_init(out, buf, size, COAP_VERSION_1, type, tkl, token, code,
				coap_header_get_id(req));
}

static int send_code(struct coap_resource *res, const struct coap_packet *req,
		     struct sockaddr *addr, socklen_t addr_len, uint8_t code)
{
	uint8_t buf[BLOCK_HEADROOM];
	struct coap_packet response;
	int err = init_response(req, buf, sizeof(buf), code, &response);

	if (err < 0) {
		return err;
	}
	return coap_resource_send(res, &response, addr, addr_len, NULL);
}

/* Block @p num of @p body, @p size bytes each, with ETag, Content-Format,
 * Block2, Size2 and the errno option. */
static int send_block(struct coap_resource *res, const struct coap_packet *req,
		      struct sockaddr *addr, socklen_t addr_len, uint8_t code, int rc,
		      uint32_t ct_id, uint32_t etag, const uint8_t *body, size_t len, uint32_t num,
		      size_t size)
{
	uint8_t buf[BLOCK_SIZE + BLOCK_HEADROOM];
	struct coap_packet response;
	size_t offset = (size_t)num * size;
	size_t chunk;
	bool more;
	uint32_t block2;
	int err;

	if (offset >= len) {
		return send_code(res, req, addr, addr_len, COAP_RESPONSE_CODE_BAD_OPTION);
	}
	chunk = MIN(size, len - offset);
	more = (offset + chunk) < len;
	block2 = (num << 4) | (more ? BIT(3) : 0U) | (uint32_t)coap_bytes_to_block_size(size);

	err = init_response(req, buf, sizeof(buf), code, &response);
	if (err == 0) {
		err = coap_packet_append_option(&response, COAP_OPTION_ETAG, (const uint8_t *)&etag,
						sizeof(etag));
	}
	if (err == 0) {
		err = coap_append_option_int(&response, COAP_OPTION_CONTENT_FORMAT, ct_id);
	}
	if (err == 0) {
		err = coap_append_option_int(&response, COAP_OPTION_BLOCK2, block2);
	}
	if (err == 0) {
		err = coap_append_option_int(&response, COAP_OPTION_SIZE2, (uint32_t)len);
	}
	if (err == 0) {
		err = coap_append_option_int(&response, ZEPHLET_COAP_OPT_ERRNO, (uint32_t)rc);
	}
	if (err == 0) {
		err = coap_packet_append_payload_marker(&response);
	}
	if (err == 0) {
		err = coap_packet_append_payload(&response, (uint8_t *)body + offset,
						 (uint16_t)chunk);
	}
	if (err < 0) {
		LOG_WRN("block %u: %d", num, err);
		return err;
	}
	return coap_resource_send(res, &response, addr, addr_len, NULL);
}

/* The entry a new response takes: the one of the same client and request,
 * so a block after block 0 finds the new response; else an unused or
 * expired one; else the one that expires first. */
static struct kept_response *take_entry(int64_t now, uint32_t key, const struct sockaddr *addr,
					socklen_t addr_len)
{
	struct kept_response *pick = NULL;

	ARRAY_FOR_EACH_PTR(kept, k) {
		if (k->used && (k->key == key) && same_peer(k, addr, addr_len)) {
			return k;
		}
	}
	ARRAY_FOR_EACH_PTR(kept, k) {
		if (!k->used || (k->expires <= now)) {
			return k;
		}
		if ((pick == NULL) || (k->expires < pick->expires)) {
			pick = k;
		}
	}
	return pick;
}

int zephlet_coap_block2_continue(struct coap_resource *res, struct coap_packet *req,
				 struct sockaddr *addr, socklen_t addr_len)
{
	uint32_t num;
	size_t size;
	uint32_t key;
	int64_t now = k_uptime_get();
	int ret;

	if (coap_header_get_code(req) != COAP_METHOD_POST) {
		return 0;
	}
	if (!requested_block(req, &num, &size) || (num == 0)) {
		return 0;
	}

	key = request_key(req);
	k_mutex_lock(&kept_lock, K_FOREVER);
	ARRAY_FOR_EACH_PTR(kept, k) {
		if (k->used && (k->expires > now) && (k->key == key) &&
		    same_peer(k, addr, addr_len)) {
			ret = send_block(res, req, addr, addr_len, COAP_RESPONSE_CODE_CONTENT, 0,
					 k->ct_id, k->etag, k->body, k->len, num, size);
			k_mutex_unlock(&kept_lock);
			return (ret < 0) ? ret : 1;
		}
	}
	k_mutex_unlock(&kept_lock);

	/* The kept response expired or was taken: the client starts over */
	ret = send_code(res, req, addr, addr_len, COAP_RESPONSE_CODE_INCOMPLETE);
	return (ret < 0) ? ret : 1;
}

int zephlet_coap_block2_send(struct coap_resource *res, struct coap_packet *req,
			     struct sockaddr *addr, socklen_t addr_len, uint8_t code, int rc,
			     uint32_t ct_id, const uint8_t *payload, size_t len)
{
	uint32_t num;
	size_t size;
	int ret;

	(void)requested_block(req, &num, &size);
	if ((num == 0) && (len <= size)) {
		return 0;
	}

	if (coap_header_get_code(req) != COAP_METHOD_POST) {
		/* GET: the same body every time, the ETag its CRC */
		ret = send_block(res, req, addr, addr_len, code, rc, ct_id,
				 crc32_ieee(payload, len), payload, len, num, size);
		return (ret < 0) ? ret : 1;
	}

	if (len > CONFIG_ZEPHLETS_COAP_BLOCK2_MAX_SIZE) {
		LOG_WRN("response of %zu bytes over CONFIG_ZEPHLETS_COAP_BLOCK2_MAX_SIZE", len);
		return -EMSGSIZE;
	}

	uint32_t key = request_key(req);

	k_mutex_lock(&kept_lock, K_FOREVER);
	struct kept_response *k = take_entry(k_uptime_get(), key, addr, addr_len);

	k->used = true;
	memcpy(&k->peer, addr, MIN((size_t)addr_len, sizeof(k->peer)));
	k->peer_len = addr_len;
	k->key = key;
	k->etag = ++next_etag;
	k->ct_id = ct_id;
	k->expires = k_uptime_get() + CONFIG_ZEPHLETS_COAP_BLOCK2_LIFETIME_MS;
	k->len = len;
	memcpy(k->body, payload, len);
	ret = send_block(res, req, addr, addr_len, code, rc, ct_id, k->etag, k->body, len, num,
			 size);
	k_mutex_unlock(&kept_lock);
	return (ret < 0) ? ret : 1;
}
