/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLET_TYPELAB_H_
#define ZLET_TYPELAB_H_

#include <stdbool.h>

#include <zephyr/kernel.h>

#include "zlet_typelab_interface.h"

/*
 * User-owned types for the `typelab` zephlet.
 *
 * The interface header (`zlet_typelab_interface.h`) is
 * codegen output; it only knows the command shape. Per-instance storage
 * (`struct typelab_data`) and the init callback
 * (`typelab_init_fn`) are user concerns and live here.
 */

/**
 * @brief Mutable per-instance data for Typelab.
 *
 * Application (or test) allocates one of these per
 * `ZEPHLET_NEW(typelab, ...)` and passes its
 * address as the `_data` arg.
 */
struct typelab_data {
	/* ----- Framework-standard fields (keep first, in this order) ----- */
	bool is_running;
	bool is_ready;
	/* No custom fields — typelab has no hardware, only Typelab.Config. */
};

/**
 * @brief Default init_fn for Typelab instances.
 *
 * Marks the instance ready. The configuration lives in `*z->config`
 * (writable; the `config` command mutates it in place).
 * Pass as the `_init` arg of ZEPHLET_NEW.
 */
int typelab_init_fn(const struct zephlet *z);

#endif /* ZLET_TYPELAB_H_ */
