#include "zephlet_shell_value.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

/* Cap on how many bytes a hexbytes print will render before truncating
 * with "..." — a display-only bound, unrelated to any field's actual
 * declared size (which the parse side already enforces exactly). */
#define ZLET_SHELL_PRINT_HEX_MAX_BYTES 64

int zlet_shell_parse_uint(const char *tok, uint64_t *out)
{
	char *end;
	unsigned long long v;

	if (tok == NULL || tok[0] == '\0' || tok[0] == '-') {
		/* strtoull() accepts a leading '-' and silently wraps the
		 * negated magnitude into an unsigned value; reject it
		 * outright since an unsigned field has no such syntax. */
		return -EINVAL;
	}

	errno = 0;
	if (tok[0] == 'h') {
		if (tok[1] == '\0') {
			return -EINVAL;
		}
		v = strtoull(&tok[1], &end, 16);
	} else {
		v = strtoull(tok, &end, 10);
	}

	if (errno != 0 || *end != '\0') {
		return -EINVAL;
	}

	*out = (uint64_t)v;
	return 0;
}

int zlet_shell_parse_int(const char *tok, int64_t *out)
{
	char *end;

	if (tok == NULL || tok[0] == '\0') {
		return -EINVAL;
	}

	if (tok[0] == 'h') {
		unsigned long long bits;

		if (tok[1] == '\0') {
			return -EINVAL;
		}
		errno = 0;
		bits = strtoull(&tok[1], &end, 16);
		if (errno != 0 || *end != '\0') {
			return -EINVAL;
		}
		*out = (int64_t)bits;
		return 0;
	}

	errno = 0;
	long long v = strtoll(tok, &end, 10);

	if (errno != 0 || *end != '\0') {
		return -EINVAL;
	}

	*out = (int64_t)v;
	return 0;
}

int zlet_shell_parse_float(const char *tok, double *out)
{
	char *end;
	double v;

	if (tok == NULL || tok[0] == '\0') {
		return -EINVAL;
	}

	errno = 0;
	v = strtod(tok, &end);
	if (errno != 0 || end == tok || *end != '\0') {
		return -EINVAL;
	}

	*out = v;
	return 0;
}

int zlet_shell_parse_bool(const char *tok, bool *out)
{
	if (tok == NULL) {
		return -EINVAL;
	}

	if (strcmp(tok, "true") == 0 || strcmp(tok, "1") == 0) {
		*out = true;
		return 0;
	}

	if (strcmp(tok, "false") == 0 || strcmp(tok, "0") == 0) {
		*out = false;
		return 0;
	}

	return -EINVAL;
}

int zlet_shell_parse_hexbytes(const char *tok, uint8_t *out, size_t max_len, size_t *out_len)
{
	size_t hexlen;
	size_t nbytes;

	if (tok == NULL || tok[0] != 'h') {
		return -EINVAL;
	}

	hexlen = strlen(&tok[1]);
	if (hexlen == 0 || (hexlen % 2) != 0) {
		return -EINVAL;
	}

	nbytes = hexlen / 2;
	if (nbytes > max_len) {
		return -ENOSPC;
	}

	if (hex2bin(&tok[1], hexlen, out, max_len) != nbytes) {
		return -EINVAL;
	}

	*out_len = nbytes;
	return 0;
}

int zlet_shell_parse_string(const char *tok, char *out, size_t cap)
{
	size_t len;

	if (tok == NULL || cap == 0) {
		return -ENOSPC;
	}

	len = strlen(tok);
	if (len + 1 > cap) {
		return -ENOSPC;
	}

	memcpy(out, tok, len + 1);
	return 0;
}

void zlet_shell_print_uint(const struct shell *sh, const char *name, uint64_t v)
{
	shell_print(sh, "%s = %llu", name, (unsigned long long)v);
}

void zlet_shell_print_int(const struct shell *sh, const char *name, int64_t v)
{
	shell_print(sh, "%s = %lld", name, (long long)v);
}

void zlet_shell_print_float(const struct shell *sh, const char *name, double v)
{
	shell_print(sh, "%s = %g", name, v);
}

void zlet_shell_print_bool(const struct shell *sh, const char *name, bool v)
{
	shell_print(sh, "%s = %s", name, v ? "true" : "false");
}

void zlet_shell_print_hexbytes(const struct shell *sh, const char *name, const uint8_t *data,
			       size_t len)
{
	char buf[2 * ZLET_SHELL_PRINT_HEX_MAX_BYTES + 1];
	size_t print_len = MIN(len, ZLET_SHELL_PRINT_HEX_MAX_BYTES);

	bin2hex(data, print_len, buf, sizeof(buf));
	shell_print(sh, "%s = h\"%s%s\"", name, buf, (print_len < len) ? "..." : "");
}

void zlet_shell_print_string(const struct shell *sh, const char *name, const char *s)
{
	shell_print(sh, "%s = \"%s\"", name, s);
}
