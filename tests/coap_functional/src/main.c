#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_service.h>

LOG_MODULE_REGISTER(zlet_coap_functional, LOG_LEVEL_INF);

/**
 * @file
 * @brief Minimal CoAP server host for the pytest functional harness.
 *
 * Registers a `COAP_SERVICE_DEFINE` on UDP/5683 with a single placeholder
 * resource so Zephyr's per-service iterable section is non-empty (without
 * at least one resource the linker does not emit
 * `_coap_resource_<svc>_list_{start,end}` and the service descriptor fails
 * to link). The pytest smoke test queries an unrelated path and asserts
 * `4.04 Not Found`, proving the aiocoap fixture reaches the Zephyr CoAP
 * stack end-to-end.
 */

static uint16_t coap_port = 5683;

COAP_SERVICE_DEFINE(zlet_coap_test_service, "0.0.0.0", &coap_port,
		    COAP_SERVICE_AUTOSTART);

static int placeholder_get(struct coap_resource *resource,
			   struct coap_packet *request,
			   struct sockaddr *addr,
			   socklen_t addr_len)
{
	ARG_UNUSED(resource);
	ARG_UNUSED(request);
	ARG_UNUSED(addr);
	ARG_UNUSED(addr_len);
	return -ENOENT;
}

static const char *const placeholder_path[] = { "phase0_placeholder", NULL };

COAP_RESOURCE_DEFINE(zlet_coap_placeholder, zlet_coap_test_service, {
	.path = placeholder_path,
	.get = placeholder_get,
});

int main(void)
{
	LOG_INF("zephlet coap functional smoke harness up on UDP/%u", coap_port);
	return 0;
}
