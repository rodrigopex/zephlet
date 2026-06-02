#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_link_format.h>
#include <zephyr/net/coap_service.h>
#include <zephyr/sys/iterable_sections.h>

#include "zephlet_coap_consts.h"
#include "zephlet_coap_send.h"
#include "zephlet_coap_types.h"

LOG_MODULE_DECLARE(zlet_coap);

/**
 * @file
 * @brief Custom `/.well-known/core` handler that filters out hidden
 *        resources before delegating to Zephyr's standard formatter.
 *
 * Zephyr's stock handler (`CONFIG_COAP_SERVER_WELL_KNOWN_CORE`) iterates
 * every registered resource and emits it. The zephlet frontend marks
 * the per-type RPC wildcard of non-discoverable services with
 * `ZEPHLET_COAP_HIDDEN` via `user_data`; that marker would otherwise
 * leak the type's URI prefix into the enumeration. The handler below
 * copies the visible subset into a stack array and feeds it to
 * `coap_well_known_core_get_len`, so the wire output reuses Zephyr's
 * RFC 6690 formatter unchanged.
 *
 * Under `CONFIG_ZEPHLETS_COAP`, the framework Kconfig also sets
 * `COAP_SERVER_WELL_KNOWN_CORE=n`; the stock handler stays out of the
 * way so this resource wins.
 */

/* Sentinel definition for `ZEPHLET_COAP_HIDDEN`. The value is
 * irrelevant — only the address is used as a tag. */
const char zephlet_coap_hidden_marker;

/* Max resources copied into the filtered stack array. With three
 * resources per discoverable type (`apis`, `instances`, wildcard) plus
 * the wildcards of non-discoverable types and this discovery resource,
 * 16 covers a comfortable upper bound for typical apps. Bumping it
 * costs a few hundred bytes on the handler stack. */
#define ZLET_WELLKNOWN_MAX_VISIBLE 16

extern const struct coap_service zlet_coap_service;

static int zlet_wellknown_core_get(struct coap_resource *res, struct coap_packet *req,
				   struct sockaddr *addr, socklen_t addr_len)
{
	struct coap_resource visible[ZLET_WELLKNOWN_MAX_VISIBLE];
	size_t count = 0;

	COAP_SERVICE_FOREACH_RESOURCE(&zlet_coap_service, r) {
		if (r->user_data == ZEPHLET_COAP_HIDDEN) {
			continue;
		}
		if (count >= ARRAY_SIZE(visible)) {
			LOG_WRN("/.well-known/core: visible resource cap %u reached",
				(unsigned)ARRAY_SIZE(visible));
			break;
		}
		visible[count++] = *r;
	}

	uint8_t buf[CONFIG_COAP_SERVER_MESSAGE_SIZE];
	struct coap_packet response;

	int ret = coap_well_known_core_get_len(visible, count, req, &response, buf, sizeof(buf));
	if (ret < 0) {
		LOG_ERR("/.well-known/core: format failed: %d", ret);
		return ret;
	}

	return coap_resource_send(res, &response, addr, addr_len, NULL);
}

static const char *const zlet_wellknown_core_path[] = {
	".well-known",
	"core",
	NULL,
};

COAP_RESOURCE_DEFINE(zlet_wellknown_core_resource, zlet_coap_service,
		     {
			     .path = zlet_wellknown_core_path,
			     .get = zlet_wellknown_core_get,
		     });

/* /zlet/apis: shared base lifecycle method names, served as
 * `text/plain;charset=utf-8`. One method per line, no trailing newline.
 *
 * The names below MUST stay in lockstep with `LifecycleApi` in
 * zephlet.proto — the Python codegen already asserts (build error) that
 * every discoverable service declares each base name, but the actual
 * wire body here is hand-maintained. Drift between the two is the
 * caller's responsibility to detect; keep this in mind when extending
 * the base contract.
 */
static const char zlet_base_apis_body[] = "start\n"
					  "stop\n"
					  "get_status\n"
					  "config\n"
					  "get_config";

static int zlet_base_apis_get(struct coap_resource *res, struct coap_packet *req,
			      struct sockaddr *addr, socklen_t addr_len)
{
	return zephlet_coap_send_payload_ct(res, req, addr, addr_len, 0, ZEPHLET_COAP_CT_TEXT_PLAIN,
					    (const uint8_t *)zlet_base_apis_body,
					    sizeof(zlet_base_apis_body) - 1);
}

static const char *const zlet_base_apis_path[] = {
	"zlet",
	"apis",
	NULL,
};

COAP_RESOURCE_DEFINE(zlet_base_apis_resource, zlet_coap_service,
		     {
			     .path = zlet_base_apis_path,
			     .get = zlet_base_apis_get,
		     });
