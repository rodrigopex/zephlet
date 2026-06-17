#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/coap_service.h>

#ifdef CONFIG_ZEPHLETS_COAP_DTLS
#include <zephyr/init.h>
#include <zephyr/net/tls_credentials.h>
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

#ifdef CONFIG_ZEPHLETS_COAP_DTLS

static uint16_t zlet_coap_port = CONFIG_ZEPHLETS_COAP_SECURE_PORT;

static const sec_tag_t zlet_coap_sec_tags[] = {
	CONFIG_ZEPHLETS_COAP_DTLS_SEC_TAG,
};

/* The service's sec_tag_list_size is forwarded verbatim as the
 * setsockopt(TLS_SEC_TAG_LIST) option length, which is a byte count — use
 * sizeof, not ARRAY_SIZE, or coap_service_start() rejects it with -EINVAL. */
COAPS_SERVICE_DEFINE(zlet_coap_service, "0.0.0.0", &zlet_coap_port, COAP_SERVICE_AUTOSTART,
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

COAP_SERVICE_DEFINE(zlet_coap_service, "0.0.0.0", &zlet_coap_port, COAP_SERVICE_AUTOSTART);

#endif /* CONFIG_ZEPHLETS_COAP_DTLS */
