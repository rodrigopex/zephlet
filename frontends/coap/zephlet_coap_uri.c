#include "zephlet_coap_uri.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/coap.h>

LOG_MODULE_DECLARE(zlet_coap);

/**
 * @file
 * @brief URI-path helpers for zephlet CoAP handlers.
 */

int zephlet_coap_parse_uri_path(struct coap_packet *req, struct coap_option *opts, size_t max,
				size_t *count)
{
	if (req == NULL || opts == NULL || count == NULL) {
		return -EINVAL;
	}
	if (max == 0 || max > UINT16_MAX) {
		return -EINVAL;
	}

	int n = coap_find_options(req, COAP_OPTION_URI_PATH, opts, (uint16_t)max);
	if (n < 0) {
		return n;
	}
	if ((size_t)n > max) {
		return -E2BIG;
	}

	*count = (size_t)n;
	return 0;
}

int zephlet_coap_option_to_cstr(const struct coap_option *opt, char *buf, size_t buf_size)
{
	if (opt == NULL || buf == NULL || buf_size == 0) {
		return -EINVAL;
	}
	if ((size_t)opt->len + 1U > buf_size) {
		return -ENOBUFS;
	}

	memcpy(buf, opt->value, opt->len);
	buf[opt->len] = '\0';
	return 0;
}
