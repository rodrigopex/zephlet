/* Text-format descriptors for the shared zephlet.proto messages.
 *
 * Available whenever CONFIG_NANOPB_TEXTFORMAT is on, not only under a
 * frontend: printing a proto struct as text format is useful anywhere,
 * including application logging, and it is the library's facility rather
 * than any one frontend's.
 */

#ifndef ZEPHLET_TEXTFORMAT_H_
#define ZEPHLET_TEXTFORMAT_H_

#if defined(CONFIG_NANOPB_TEXTFORMAT)

#include <nanopb_textformat.h>

/**
 * @brief Descriptor for Lifecycle.Status, the response of start/stop/
 *        get_status on every zephlet.
 *
 * Defined once in zephlet_textformat.c. Codegen emits a `PB_TF_DEFINE` per
 * message for each *per-zephlet* proto, but zephlet.proto is shared and
 * codegen runs once per zephlet, so emitting this from there would produce
 * a duplicate symbol for every type. `Empty` has no fields and needs no
 * descriptor.
 *
 * Usage:
 * @code
 * struct lifecycle_status st;
 * char text[64];
 *
 * (void)zephlet_get_status(z, &st, K_MSEC(100));
 * (void)pb_tf_print_buf(&lifecycle_status_t_tf, &st, text, sizeof(text));
 * @endcode
 */
PB_TF_DECLARE(lifecycle_status_t);

#endif /* CONFIG_NANOPB_TEXTFORMAT */

#endif /* ZEPHLET_TEXTFORMAT_H_ */
