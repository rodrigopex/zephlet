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
 * Every zephlet owns a file-local `struct k_spinlock` (declared in the
 * generated `_interface.c`) that serializes access to all three storage
 * bags: `status`, `settings`, `events`. Read and write paths — from
 * thread context or ISR context — use the same primitive via
 * `K_SPINLOCK(&<z>_lock) { ... }`.
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
 * @brief Runtime state of a zephlet instance (mutable, lives in RAM).
 *
 * All three storage bags are reachable through this struct. The pointers
 * themselves are fixed at link time (`*const`); the targets they point to
 * are the per-zephlet mutable state in file-local statics declared inside
 * the generated `<z>_interface.c`.
 *
 * There is no separate `zephlet_config` — zephlets don't have
 * compile-time per-instance const data, and treating runtime-mutable
 * state as "config" only muddies the Zephyr-convention split between
 * `config` (flash) and `data` (RAM).
 */
struct zephlet_data {
	struct zephlet_status *const status;
	void                  *const settings;
	const size_t                 settings_size;
	void                  *const events;
	const size_t                 events_size;
};

struct zephlet {
	const char *name;
	struct {
		const struct zbus_channel *invoke;
		const struct zbus_channel *report;
	} channel;
	int (*init_fn)(const struct zephlet *self);
	void                *api;
	struct zephlet_data *data;
};

/*
 * ZEPHLET_DEFINE(name, init_fn, api)
 *
 * Registers the zephlet instance. The generated `_interface.c` owns the
 * mutable state as a file-scope `struct zephlet_data <name>_data = {...}`,
 * so this macro reaches it via token concatenation — you never pass it in.
 */
#define ZEPHLET_DEFINE(_name, _init_fn, _api)                                                      \
	const STRUCT_SECTION_ITERABLE(zephlet, _name) = {                                          \
		.name = #_name,                                                                    \
		.channel =                                                                         \
			{                                                                          \
				.invoke = &CONCAT(chan_, _name, _invoke),                          \
				.report = &CONCAT(chan_, _name, _report),                          \
			},                                                                         \
		.init_fn = (_init_fn),                                                             \
		.api = (_api),                                                                     \
		.data = &CONCAT(_name, _data),                                                     \
	}

#define ZEPHLET_CALL_OK(report) ((report).has_result && (report).result.return_code == 0)

/**
 * @name Generic lifecycle core helpers
 *
 * Pure state-machine primitives. They take `struct zephlet_data *data`
 * and operate on the storage bags reachable through it. They do NOT
 * touch the per-zephlet spinlock — the per-zephlet generated shim wraps
 * calls with `K_SPINLOCK(&<z>_lock) { ... }`.
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
