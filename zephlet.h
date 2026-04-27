#ifndef MODULES_ZEPHLETS_SHARED_ZEPHLET_H
#define MODULES_ZEPHLETS_SHARED_ZEPHLET_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include <pb.h>

/**
 * @file
 * @brief Zephlet v0.3 shared contract.
 *
 * Each zephlet instance owns two zbus channels:
 *   - `chan_<name>_command` — pointer channel of `struct zephlet_call *`.
 *     Exactly one listener observer (`lis_<type>`). Synchronous command
 *     via zbus sync-listener semantics: the dispatcher runs in the
 *     caller's thread, mutates the envelope in place, and by the time
 *     `zbus_chan_pub()` returns `call->return_code` and `*call->resp`
 *     are populated.
 *   - `chan_<name>_events`  — zbus value-type channel of
 *     `struct <type>_events`. Asynchronous fan-out; zbus copies the
 *     value into each consumer's queue.
 *
 * `ZEPHLET_NEW` is the only supported creator of the channel
 * pair. Do not add observers to `chan_<name>_command` by any other
 * means; the SYS_INIT walker asserts exactly one listener on the
 * command channel under `CONFIG_ASSERT`.
 */

/* Forward decl so the handler function-pointer can reference it. */
struct zephlet;

/**
 * @brief Command envelope carried on a zephlet's command channel.
 *
 * Callers place a stack-local `struct zephlet_call`, publish its
 * address. The sync-listener dispatcher mutates the envelope in place.
 */
struct zephlet_call {
	/** Command method id. Allocated in declaration order starting at 1.
	 *  0 is reserved for future in-band control signalling. */
	uint16_t method_id;
	/** Dispatcher-populated return code (POSIX errno; 0 on success). */
	int32_t return_code;
	/** nanopb descriptor for the request message, or NULL for Empty. */
	const pb_msgdesc_t *req_desc;
	/** Request payload; must be non-NULL when req_desc != NULL. */
	const void *req;
	/** nanopb descriptor for the response message, or NULL for Empty. */
	const pb_msgdesc_t *resp_desc;
	/** Response storage. NULL signals "caller discards the response". */
	void *resp;
};

/**
 * @brief One row of a zephlet's method table.
 *
 * Indexed by `method_id`. Entry 0 is reserved (handler stays NULL);
 * real commands start at index 1.
 */
struct zephlet_method {
	const pb_msgdesc_t *req_desc;
	const pb_msgdesc_t *resp_desc;
	int (*handler)(const struct zephlet *z, struct zephlet_call *call);
};

/**
 * @brief Per-type method dispatch table.
 */
struct zephlet_api {
	const struct zephlet_method *methods;
	size_t num_methods;
};

/**
 * @brief Zephlet instance descriptor (const, flash-resident).
 *
 * Multiple instances of the same type coexist. Each has its own channel
 * pair, its own config, and its own data pointer. The framework treats
 * both `config` and `data` as opaque.
 */
struct zephlet {
	const char *name;
	const struct zephlet_api *api;
	struct {
		/** Pointer channel, listener-only (sync command). */
		const struct zbus_channel *command;
		/** Value-type channel (async fan-out). */
		const struct zbus_channel *events;
	} channel;
	void *config;
	void *data;
	int (*init_fn)(const struct zephlet *self);
	/** SYS_INIT walker runs `init_fn`s in ascending order. Default 0. */
	int init_priority;
};

/**
 * @brief Dispatch a command call on a zephlet instance.
 *
 * Bounds-checks `call->method_id`, looks up the handler, sets
 * `call->return_code` to either `-ENOSYS` (OOB or NULL handler) or the
 * handler's return value.
 *
 * @return 0 on normal dispatch (including handler errors reported via
 *         `call->return_code`); `-EINVAL` only if `z` or `call` is NULL.
 */
int zephlet_dispatch(const struct zephlet *z, struct zephlet_call *call);

/**
 * ZEPHLET_EVENTS_LISTENER(instance, type, callback)
 *
 * Attach an **asynchronous** callback to an instance's events channel.
 * Wraps zbus's `ZBUS_ASYNC_LISTENER_DEFINE`, which runs the callback
 * from the system workqueue — never from ISR or the publisher's
 * thread. The callback may freely call into other zephlets' RPCs with
 * any timeout.
 *
 * This is the framework's event-observer primitive. Adapters (the
 * ports-and-adapters sense: an origin zephlet's events driving a
 * destination zephlet's command) are one use case — the framework takes
 * no position on what the callback does.
 *
 * Requires `CONFIG_ZBUS_ASYNC_LISTENER=y` (selected by `CONFIG_ZEPHLETS`).
 *
 * The callback receives the owning instance pointer (sourced from
 * `zbus_chan_user_data`) so policies that observe multiple instances
 * of the same zephlet type can disambiguate them. The macro casts the
 * pointer explicitly to `const struct zephlet *`: any mismatch in the
 * user callback's signature produces a per-argument compile-time
 * diagnostic.
 *
 * Usage:
 *     static void on_tick(const struct zephlet *z,
 *                         const struct tick_events *ev) { ... }
 *     ZEPHLET_EVENTS_LISTENER(tick_fast, tick, on_tick);
 *
 * Kconfig / build dependency:
 * The channel symbol `chan_<instance>_events` only exists when the
 * owning zephlet is enabled. If this macro is invoked from a
 * translation unit that might be built with the zephlet disabled,
 * guard the TU at the CMake level (`if(CONFIG_...) zephyr_library_sources(...)`)
 * rather than with preprocessor conditionals inside the source.
 */
#define ZEPHLET_EVENTS_LISTENER(_instance, _type, _callback)                                       \
	extern const struct zbus_channel chan_##_instance##_events;                                \
	static void _zephlet_ev_##_instance##_##_callback##_fn(const struct zbus_channel *chan,    \
							       const void *msg)                    \
	{                                                                                          \
		const struct zephlet *z =                                                          \
			(const struct zephlet *)zbus_chan_user_data(chan);                         \
		_callback(z, (const struct _type##_events *)msg);                                  \
	}                                                                                          \
	ZBUS_ASYNC_LISTENER_DEFINE(_zephlet_ev_##_instance##_##_callback##_lis,                    \
				   _zephlet_ev_##_instance##_##_callback##_fn);                    \
	ZBUS_CHAN_ADD_OBS(chan_##_instance##_events, _zephlet_ev_##_instance##_##_callback##_lis, 3)

/**
 * @brief Find a zephlet instance by name.
 */
const struct zephlet *zephlet_get_by_name(const char *name);

/**
 * ZEPHLET_NEW(type, name, cfg, data, init)
 *
 * Declares a zephlet instance with `init_priority == 0`.
 *
 *  - Creates `chan_<name>_command` : pointer channel, observer = `lis_<type>`.
 *  - Creates `chan_<name>_events`  : value channel of `struct <type>_events`.
 *  - Registers `STRUCT_SECTION_ITERABLE(zephlet, <name>)`.
 *
 * @param _type  Zephlet type symbol (e.g. `tick`). Used to resolve
 *               `<type>_api` and `struct <type>_events`.
 * @param _name  Instance symbol (e.g. `tick_fast`).
 * @param _cfg   Pointer to per-instance config (writable; the config
 *               command mutates it in place).
 * @param _data  Pointer to per-instance mutable data.
 * @param _init  `int (*)(const struct zephlet *)` or NULL.
 */
#define ZEPHLET_NEW(_type, _name, _cfg, _data, _init)                                              \
	ZEPHLET_NEW_PRIO(_type, _name, _cfg, _data, _init, 0)

/**
 * ZEPHLET_NEW_PRIO(type, name, cfg, data, init, prio)
 *
 * Same as `ZEPHLET_NEW`, with an explicit `init_priority`.
 * SYS_INIT walker runs `init_fn`s in ascending priority.
 */
#define ZEPHLET_NEW_PRIO(_type, _name, _cfg, _data, _init, _prio)                                  \
	extern const struct zephlet _name;                                                         \
	ZBUS_CHAN_DEFINE(chan_##_name##_command, struct zephlet_call *, NULL, (void *)&_name,      \
			 ZBUS_OBSERVERS(lis_##_type), NULL);                                       \
	ZBUS_CHAN_DEFINE(chan_##_name##_events, struct _type##_events, NULL, (void *)&_name,       \
			 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));                                  \
	const STRUCT_SECTION_ITERABLE(zephlet, _name) = {                                          \
		.name = #_name,                                                                    \
		.api = &_type##_api,                                                               \
		.channel =                                                                         \
			{                                                                          \
				.command = &chan_##_name##_command,                                \
				.events = &chan_##_name##_events,                                  \
			},                                                                         \
		.config = (_cfg),                                                                  \
		.data = (_data),                                                                   \
		.init_fn = (_init),                                                                \
		.init_priority = (_prio),                                                          \
	}

#endif /* MODULES_ZEPHLETS_SHARED_ZEPHLET_H */
