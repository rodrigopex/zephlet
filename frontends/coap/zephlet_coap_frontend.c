#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/coap_service.h>

#ifdef CONFIG_ZEPHLETS_COAP_DTLS
#include <zephyr/init.h>
#include <zephyr/net/tls_credentials.h>
#endif

#if defined(CONFIG_NET_CONNECTION_MANAGER)
#include <errno.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#endif

LOG_MODULE_REGISTER(zlet_coap, CONFIG_ZEPHLET_LOG_LEVEL);

/**
 * @file
 * @brief CoAP frontend runtime — service declaration.
 *
 * Owns the `coap_service` instance every codegen-emitted per-type
 * resource registers against. Each opted-in zephlet type contributes
 * a `COAP_RESOURCE_DEFINE(<type>_coap_resource, zlet_coap_service, ...)`
 * from its `<prefix>_coap_interface.c`; this TU has no per-type
 * knowledge.
 *
 * With `CONFIG_ZEPHLETS_COAP_DTLS=y` the service is bound over DTLS on
 * the secure port and authenticated with a PSK the integrator supplies
 * via Kconfig; otherwise it is plain CoAP over UDP on 5683.
 */

/*
 * With offloaded sockets (e.g. ESP-AT Wi-Fi) the listen socket cannot bind
 * until the link is up, which the connection manager reports asynchronously
 * well after boot — so COAP_SERVICE_AUTOSTART loses the race and the service
 * never comes up. When the connection manager is present, declare the service
 * without autostart and drive start/stop from the L4 connectivity events (see
 * zlet_coap_on_l4_event below); otherwise keep boot-time autostart.
 */
#if defined(CONFIG_NET_CONNECTION_MANAGER)
#define ZLET_COAP_SERVICE_FLAGS 0
#else
#define ZLET_COAP_SERVICE_FLAGS COAP_SERVICE_AUTOSTART
#endif

#ifdef CONFIG_ZEPHLETS_COAP_DTLS

static uint16_t zlet_coap_port = CONFIG_ZEPHLETS_COAP_SECURE_PORT;

static const sec_tag_t zlet_coap_sec_tags[] = {
	CONFIG_ZEPHLETS_COAP_DTLS_SEC_TAG,
};

/* The service's sec_tag_list_size is forwarded verbatim as the
 * setsockopt(TLS_SEC_TAG_LIST) option length, which is a byte count — use
 * sizeof, not ARRAY_SIZE, or coap_service_start() rejects it with -EINVAL. */
COAPS_SERVICE_DEFINE(zlet_coap_service, "0.0.0.0", &zlet_coap_port, ZLET_COAP_SERVICE_FLAGS,
		     zlet_coap_sec_tags, sizeof(zlet_coap_sec_tags));

/**
 * @brief Register the CoAPS pre-shared key before the service binds.
 *
 * No key material ships in the framework: the identity and key come from
 * `CONFIG_ZEPHLETS_COAP_DTLS_PSK_IDENTITY` / `_KEY`, which the integrator
 * provides through an overlay or secret. Runs ahead of the coap_service
 * autostart so the DTLS socket has its credential when it binds.
 */
static int zlet_coap_dtls_psk_init(void)
{
	static const char psk[] = CONFIG_ZEPHLETS_COAP_DTLS_PSK_KEY;
	static const char psk_id[] = CONFIG_ZEPHLETS_COAP_DTLS_PSK_IDENTITY;
	int err;

	/* `sizeof - 1` drops the implicit string terminator: the PSK and its
	 * identity are byte strings, not C strings, on the wire. */
	err = tls_credential_add(CONFIG_ZEPHLETS_COAP_DTLS_SEC_TAG, TLS_CREDENTIAL_PSK, psk,
				 sizeof(psk) - 1);
	if (err < 0) {
		LOG_ERR("CoAPS PSK register failed: %d", err);
		return err;
	}

	err = tls_credential_add(CONFIG_ZEPHLETS_COAP_DTLS_SEC_TAG, TLS_CREDENTIAL_PSK_ID, psk_id,
				 sizeof(psk_id) - 1);
	if (err < 0) {
		LOG_ERR("CoAPS PSK identity register failed: %d", err);
		return err;
	}

	return 0;
}

SYS_INIT(zlet_coap_dtls_psk_init, APPLICATION, 0);

#else

static uint16_t zlet_coap_port = 5683;

COAP_SERVICE_DEFINE(zlet_coap_service, "0.0.0.0", &zlet_coap_port, ZLET_COAP_SERVICE_FLAGS);

#endif /* CONFIG_ZEPHLETS_COAP_DTLS */

#if defined(CONFIG_NET_CONNECTION_MANAGER)
/*
 * Start/stop the service in step with L4 connectivity. zlet_coap_service is
 * declared in both the DTLS and plain branches above, so this is independent
 * of the transport.
 */
static void zlet_coap_on_l4_event(uint64_t mgmt_event, struct net_if *iface, void *info,
				  size_t info_length, void *user_data)
{
	int ret;

	ARG_UNUSED(iface);
	ARG_UNUSED(info);
	ARG_UNUSED(info_length);
	ARG_UNUSED(user_data);

	switch (mgmt_event) {
	case NET_EVENT_L4_CONNECTED:
		ret = coap_service_start(&zlet_coap_service);
		if (ret < 0 && ret != -EALREADY) {
			LOG_ERR("CoAP service start failed on L4 connect: %d", ret);
		}
		break;
	case NET_EVENT_L4_DISCONNECTED:
		(void)coap_service_stop(&zlet_coap_service);
		break;
	}
}

NET_MGMT_REGISTER_EVENT_HANDLER(zlet_coap_l4_handler,
				NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED,
				zlet_coap_on_l4_event, NULL);
#endif /* CONFIG_NET_CONNECTION_MANAGER */
