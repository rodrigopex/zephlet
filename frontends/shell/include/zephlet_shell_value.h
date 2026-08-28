/* Typed value parse/print helpers for the `zlet` shell frontend. */

#ifndef ZEPHLET_SHELL_VALUE_H_
#define ZEPHLET_SHELL_VALUE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct shell;

/**
 * @file
 * @brief One function per nanopb scalar "family", not per exact token.
 *
 * The exact-token dispatch (which family a field's nanopb ltype belongs
 * to, and the C type to cast the parsed scratch value into) happens in
 * zephlet_shell_macros.h via typeof(). These functions never see the
 * field's real type — they only produce/consume a family-wide scratch
 * type (uint64_t, int64_t, double).
 */

/**
 * @brief Parse an unsigned integer token: decimal, or `h<hex>` (the
 * field's numeric value in hex, e.g. "h1F4" == 500).
 *
 * @retval 0 on success.
 * @retval -EINVAL malformed token (empty, non-digit, trailing garbage).
 */
int zlet_shell_parse_uint(const char *tok, uint64_t *out);

/**
 * @brief Parse a signed integer token: decimal (leading '-' allowed),
 * or `h<hex>` (two's-complement bit pattern, e.g. "hFFFFFFFF" == -1
 * once truncated to the field's real width).
 *
 * @retval 0 on success.
 * @retval -EINVAL malformed token.
 */
int zlet_shell_parse_int(const char *tok, int64_t *out);

/** @brief Parse a float/double token via strtod(). -EINVAL if malformed. */
int zlet_shell_parse_float(const char *tok, double *out);

/** @brief Parse "true"/"false"/"1"/"0". -EINVAL otherwise. */
int zlet_shell_parse_bool(const char *tok, bool *out);

/**
 * @brief Decode an `h<hex>` token into raw bytes.
 *
 * Requires the "h" prefix and an even number of hex digits after it —
 * odd length or a non-hex character is a parse error, not a
 * best-effort decode.
 *
 * @param max_len Capacity of `out` (the field's own declared size).
 * @param out_len Set to the decoded byte count on success.
 *
 * @retval 0 on success.
 * @retval -EINVAL malformed token (missing prefix, odd length, bad hex).
 * @retval -ENOSPC decoded length would exceed max_len.
 */
int zlet_shell_parse_hexbytes(const char *tok, uint8_t *out, size_t max_len, size_t *out_len);

/**
 * @brief Bounded copy of `tok` into a fixed `char[cap]` string field.
 *
 * @param cap Capacity of `out` including the NUL terminator (matches
 * nanopb's STRING field sizing).
 *
 * @retval 0 on success.
 * @retval -ENOSPC `tok` (plus NUL) does not fit in `cap`.
 */
int zlet_shell_parse_string(const char *tok, char *out, size_t cap);

/** @brief Print helpers, one per storage family. */
void zlet_shell_print_uint(const struct shell *sh, const char *name, uint64_t v);
void zlet_shell_print_int(const struct shell *sh, const char *name, int64_t v);
void zlet_shell_print_float(const struct shell *sh, const char *name, double v);
void zlet_shell_print_bool(const struct shell *sh, const char *name, bool v);
void zlet_shell_print_hexbytes(const struct shell *sh, const char *name, const uint8_t *data,
			       size_t len);
void zlet_shell_print_string(const struct shell *sh, const char *name, const char *s);

#endif /* ZEPHLET_SHELL_VALUE_H_ */
