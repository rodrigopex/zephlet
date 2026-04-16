#ifndef MODULES_ZEPHLETS_SHARED_ZEPHLET_H
#define MODULES_ZEPHLETS_SHARED_ZEPHLET_H

#include <stdbool.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "zephlet.pb.h"

/*
 * Concurrency model
 * =================
 *
 * Every zephlet owns a `struct k_spinlock` embedded in its
 * `struct zephlet_data` (`base_data->lock`) that serializes access to
 * all three storage bags: `status`, `settings`, `events`. Read and
 * write paths — from thread context or ISR context — use the same
 * primitive via `K_SPINLOCK(&<z>_data_storage.base.lock) { ... }`.
 *
 *   1. Critical sections are memcpy-scope only.
 *      Take the spinlock, copy data in or out, release. Never hold it
 *      across a `zbus_chan_pub`, a user hook, or any call that might
 *      sleep.
 *
 *   2. Hooks always run unlocked.
 *      The generated shims call weak hooks (`pre_start`, `post_start`,
 *      `validate_settings`, ...) outside the spinlock so hook code can
 *      do anything — including calling internal helpers that also take
 *      the lock.
 *
 *   3. Correlation slot.
 *      The per-zephlet blocking-helper `k_sem` serializes
 *      `self.expected_cid` / `self.sync_report` / `self.deadline` across
 *      blocking callers. It is independent of the storage spinlock.
 *      Direct `chan_<z>_invoke` publishers that do not hold the sem must
 *      set `ivk->has_result = false` so they never claim a correlation
 *      slot.
 *
 * These invariants assume a single-CPU / sync-listener Zephyr build
 * (the current target). SMP support is future work and will need to
 * re-evaluate the sync-listener assumption and the spinlock's
 * cross-CPU semantics.
 */

/**
 * @brief Base runtime state of a zephlet (mutable, lives in RAM).
 *
 * Every generated `struct <z>_data` places this as its first member
 * (`base`), so a pointer to `struct zephlet_data` can be converted to
 * the enclosing per-zephlet struct via CONTAINER_OF.
 *
 * `settings` and `events` are void pointers wired by
 * `<z>_set_implementation()` to typed siblings within the same
 * `struct <z>_data` at runtime.
 */
struct zephlet_data {
	struct k_spinlock lock;
	struct zephlet_status status;
	void *settings;
	size_t settings_size;
	void *events;
	size_t events_size;
};

/* Forward declaration for init_fn signature in zephlet_config. */
struct zephlet;

/**
 * @brief Immutable per-type configuration (lives in flash with the zephlet).
 */
struct zephlet_config {
	const char *name;
	struct {
		const struct zbus_channel *invoke;
		const struct zbus_channel *report;
	} channel;
	int (*init_fn)(const struct zephlet *self);
};

/**
 * @brief Zephlet descriptor — const, stored in flash.
 *
 * All members are either constant values or constant pointers.
 * Mutable state is reached indirectly through `base_data` (framework
 * storage in RAM) and `instance_data` (user-defined private data in RAM).
 */
struct zephlet {
	struct zephlet_config config;
	struct zephlet_data *base_data;
	void *instance_data;
	void *api;
};

/*
 * ZEPHLET_DEFINE(name, init_fn, api, inst_ptr)
 *
 * Registers the zephlet. `base_data` is auto-resolved via token
 * concatenation to `<name>_data_storage.base`. `inst_ptr` is a pointer
 * to user-defined instance data (or NULL if none).
 */
#define ZEPHLET_DEFINE(_name, _init_fn, _api, _inst_ptr)                                           \
	const STRUCT_SECTION_ITERABLE(zephlet, _name) = {                                          \
		.config =                                                                          \
			{                                                                          \
				.name = #_name,                                                    \
				.channel =                                                         \
					{                                                          \
						.invoke = &CONCAT(chan_, _name, _invoke),          \
						.report = &CONCAT(chan_, _name, _report),          \
					},                                                         \
				.init_fn = (_init_fn),                                             \
			},                                                                         \
		.api = (_api),                                                                     \
		.instance_data = (_inst_ptr),                                                      \
		.base_data = &CONCAT(_name, _data_storage).base,                                   \
	}

#define ZEPHLET_CALL_OK(report) ((report).has_result && (report).result.return_code == 0)

/**
 * @name Generic lifecycle core helpers
 *
 * Pure state-machine primitives. They take `struct zephlet_data *data`
 * and operate on the storage bags reachable through it. They do NOT
 * touch the per-zephlet spinlock — the per-zephlet generated shim wraps
 * calls with `K_SPINLOCK(...)`.
 *
 * @{
 */

int zephlet_start_core(struct zephlet_data *data, struct zephlet_status *out_status);
int zephlet_stop_core(struct zephlet_data *data, struct zephlet_status *out_status);
int zephlet_get_status_core(const struct zephlet_data *data, struct zephlet_status *out_status);
int zephlet_get_settings_core(const struct zephlet_data *data, void *out_settings);
int zephlet_update_settings_core(struct zephlet_data *data, const void *in_settings);
int zephlet_get_events_core(const struct zephlet_data *data, void *out_events);

/** @} */

#endif /* MODULES_ZEPHLETS_SHARED_ZEPHLET_H */
