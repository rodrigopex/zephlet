#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_NET_CONNECTION_MANAGER)
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#endif

#include "zlet_silent.h"
#include "zlet_tick.h"
#include "zlet_ui.h"

LOG_MODULE_REGISTER(zlet_coap_functional, LOG_LEVEL_INF);

/**
 * @file
 * @brief Host app for the CoAP functional twister target.
 *
 * Instantiates one `tick`, one `ui`, and one `silent` zephlet so the
 * codegen-emitted per-type CoAP resources have live targets. `tick` and
 * `ui` opt into discovery; `silent` exposes RPCs but stays absent from
 * `/.well-known/core`, which the discovery test asserts. The CoAP
 * service + resources are registered automatically via the frontend's
 * `COAP_SERVICE_DEFINE` and each opted-in zephlet's
 * `COAP_RESOURCE_DEFINE` — there is no inline CoAP setup here.
 */

static struct tick_config tick_fast_cfg = {
	.duration_ms = 100,
	.period_ms = 100,
};
static struct tick_data tick_fast_data;
ZEPHLET_NEW(tick, tick_fast, &tick_fast_cfg, &tick_fast_data, tick_init_fn);

/* Second `tick` instance — exercises the multi-instance case of the
 * `/zlet/tick/instances` runtime walk. */
static struct tick_config tick_slow_cfg = {
	.duration_ms = 500,
	.period_ms = 500,
};
static struct tick_data tick_slow_data;
ZEPHLET_NEW(tick, tick_slow, &tick_slow_cfg, &tick_slow_data, tick_init_fn);

static struct ui_config ui_main_cfg = {
	.blink_period_ms = 250,
};
static struct ui_data ui_main_data;
ZEPHLET_NEW(ui, ui_main, &ui_main_cfg, &ui_main_data, ui_init_fn);

static struct silent_config silent_inst_cfg = {
	.nothing = 0,
};
static struct silent_data silent_inst_data;
ZEPHLET_NEW(silent, silent_inst, &silent_inst_cfg, &silent_inst_data, silent_init_fn);

int main(void)
{
	LOG_INF("zephlet coap functional host up on UDP/5683");

#if defined(CONFIG_NET_CONNECTION_MANAGER)
	/*
	 * The offloaded-sockets iface never runs DHCP — it forwards socket
	 * calls straight to the host BSD stack. But conn_mgr's L4_CONNECTED
	 * still requires the *Zephyr-side* iface to have L3 readiness (an
	 * IPv4/IPv6 address of its own), same as upstream's conn_mgr_nsos
	 * test: a manual address is enough to let `net cm connect` produce
	 * a real L4_CONNECTED once the connectivity-sim binding clears
	 * dormant.
	 */
	struct net_if *iface = net_if_get_default();
	struct net_in_addr addr;

	net_addr_pton(NET_AF_INET, "192.0.2.1", &addr);
	net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
#endif

	return 0;
}
