#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/coap_service.h>

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
 */

static uint16_t zlet_coap_port = 5683;

COAP_SERVICE_DEFINE(zlet_coap_service, "0.0.0.0", &zlet_coap_port, COAP_SERVICE_AUTOSTART);
