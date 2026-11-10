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
 * @brief One dependency slot a zephlet type declares with `(zephlet.requires)`.
 *
 * Generated per type. The init walker reads the provider bound to this slot
 * at `offset` in an instance's dependency record (`struct <type>_dependencies`)
 * and checks that it is an instance of `api`.
 */
struct zephlet_dep_slot {
	/** Slot name: the field of `struct <type>_dependencies`. */
	const char *name;
	/** Required provider type, for diagnostics. */
	const char *type;
	/** The required provider type's `<type>_api`. */
	const struct zephlet_api *api;
	/** Offset of the slot's `const struct zephlet *` in the record. */
	size_t offset;
};

/**
 * @brief Per-type method dispatch table.
 */
struct zephlet_api {
	const struct zephlet_method *methods;
	size_t num_methods;
	/** Dependency slots, or NULL for a type that requires nothing. */
	const struct zephlet_dep_slot *deps;
	size_t num_deps;
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
	/** Provider instances this instance calls: the type's generated
	 *  `struct <type>_dependencies`, or NULL for a type that requires
	 *  nothing. Immutable; the providers keep their own state. */
	const void *dependencies;
	int (*init_fn)(const struct zephlet *self);
	/** SYS_INIT walker runs `init_fn`s in ascending order, every provider
	 *  before its consumers. Default 0. */
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
		const struct zephlet *z = (const struct zephlet *)zbus_chan_user_data(chan);       \
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
 * @brief How the SYS_INIT walker left @p z.
 *
 * @retval 0        `init_fn` returned 0, or the instance has none.
 * @retval -EAGAIN  The walker has not reached the instance yet.
 * @retval -ENODEV  Not initialized: a dependency slot is unbound or bound to
 *                  an instance of the wrong type, or a provider did not
 *                  initialize.
 * @retval -ELOOP   Not initialized: the instance is on a dependency cycle.
 * @retval -ENOENT  @p z is NULL or beyond `CONFIG_ZEPHLET_MAX_INSTANCES`.
 * @return Otherwise the nonzero value `init_fn` returned.
 */
int zephlet_init_result(const struct zephlet *z);

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
 *
 * A type that declares `(zephlet.requires)` fails to build here: define its
 * instances with `ZEPHLET_NEW_WITH_DEPS` instead.
 */
#define ZEPHLET_NEW_PRIO(_type, _name, _cfg, _data, _init, _prio)                                  \
	BUILD_ASSERT(_ZLET_HAS_DEPS_##_type == 0,                                                  \
		     "zephlet type '" #_type "' declares (zephlet.requires): define '" #_name    \
		     "' with ZEPHLET_NEW_WITH_DEPS");                                               \
	_ZEPHLET_DEFINE(_type, _name, _cfg, _data, NULL, _init, _prio)

/**
 * ZEPHLET_NEW_WITH_DEPS(type, name, cfg, data, deps, init)
 *
 * Same as `ZEPHLET_NEW`, for a type that declares `(zephlet.requires)`.
 * `_deps` binds this instance's providers:
 *
 *   static const struct climate_dependencies room_a_deps = {
 *           .temperature = &room_a_sensor,
 *           .fan = &room_a_fan,
 *   };
 *   ZEPHLET_NEW_WITH_DEPS(climate, room_a, &room_a_cfg, &room_a_data,
 *                         &room_a_deps, climate_init_fn);
 *
 * Fails to build when `_deps` is not a `struct <type>_dependencies *` or the
 * type requires nothing. At boot the SYS_INIT walker skips the instance,
 * with an error, when a slot is NULL or bound to an instance of another
 * type, and runs every provider's `init_fn` before this one's; see
 * zephlet_init_result(). Providers may be shared between consumers.
 *
 * @param _deps  `const struct <type>_dependencies *`, typically to a
 *               `static const` record.
 */
#define ZEPHLET_NEW_WITH_DEPS(_type, _name, _cfg, _data, _deps, _init)                             \
	ZEPHLET_NEW_PRIO_WITH_DEPS(_type, _name, _cfg, _data, _deps, _init, 0)

/**
 * ZEPHLET_NEW_PRIO_WITH_DEPS(type, name, cfg, data, deps, init, prio)
 *
 * Same as `ZEPHLET_NEW_WITH_DEPS`, with an explicit `init_priority`. A
 * provider is initialized before its consumers whatever their priorities;
 * the priority orders instances that do not depend on each other.
 */
#define ZEPHLET_NEW_PRIO_WITH_DEPS(_type, _name, _cfg, _data, _deps, _init, _prio)                 \
	BUILD_ASSERT(_ZLET_HAS_DEPS_##_type == 1,                                                  \
		     "zephlet type '" #_type "' declares no (zephlet.requires): define '" #_name \
		     "' with ZEPHLET_NEW");                                                          \
	_ZEPHLET_DEFINE(_type, _name, _cfg, _data,                                                 \
			_Generic((_deps), const struct _type##_dependencies *: (_deps),            \
				 struct _type##_dependencies *: (_deps)),                           \
			_init, _prio)

/* Common body of the instance macros. Not public: use ZEPHLET_NEW*. */
#define _ZEPHLET_DEFINE(_type, _name, _cfg, _data, _deps, _init, _prio)                            \
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
		.dependencies = (_deps),                                                           \
		.init_fn = (_init),                                                                \
		.init_priority = (_prio),                                                          \
	};                                                                                         \
	_ZLET_FRONTEND_HOOKS_##_type(_name)

/**
 * ZEPHLET_INST_DECLARE(type, id)
 *
 * Forward-declares an instance defined elsewhere with
 * `ZEPHLET_INST_DEFINE` / `ZEPHLET_PRIO_INST_DEFINE`, for consumers
 * (policies, adapters) that pass `&<type><id>` to its RPC wrappers.
 *
 * @param _zephlet_name  Zephlet type symbol.
 * @param _instance_id   Instance id token used at the definition site.
 */
#define ZEPHLET_INST_DECLARE(_zephlet_name, _instance_id)                                          \
	extern const struct zephlet _zephlet_name##_instance_id

/**
 * ZEPHLET_PRIO_INST_DEFINE(type, id, init, prio)
 *
 * Defines a zephlet instance with auto-allocated config and data,
 * sparing the caller the per-instance storage boilerplate of
 * `ZEPHLET_NEW_PRIO`.
 *
 *  - Defines `static struct <type>_config <type><id>_cfg` (zeroed).
 *  - Defines `static struct <type>_data   <type><id>_data` (zeroed).
 *  - Expands `ZEPHLET_NEW_PRIO(<type>, <type><id>, &<type><id>_cfg,
 *    &<type><id>_data, <init>, <prio>)`.
 *
 * Reach for `ZEPHLET_NEW` / `ZEPHLET_NEW_PRIO` directly when an
 * instance needs externally-owned or shared storage, or config primed
 * with non-zero literals.
 *
 * @param _zephlet_name  Zephlet type symbol (e.g. `aws_mqtt`). Used to
 *                       resolve `<type>_api`, `struct <type>_config`,
 *                       `struct <type>_data`, `struct <type>_events`.
 * @param _instance_id   Token appended to the type to form the instance
 *                       and storage symbols; unique per type per TU.
 * @param _init_fn       `int (*)(const struct zephlet *)` or NULL.
 * @param _prio          `init_priority`; the SYS_INIT walker runs
 *                       `init_fn`s in ascending priority.
 */
#define ZEPHLET_PRIO_INST_DEFINE(_zephlet_name, _instance_id, _init_fn, _prio)                     \
	static struct _zephlet_name##_config _zephlet_name##_instance_id##_cfg;                    \
	static struct _zephlet_name##_data _zephlet_name##_instance_id##_data;                     \
                                                                                                   \
	ZEPHLET_NEW_PRIO(_zephlet_name, _zephlet_name##_instance_id,                               \
			 &_zephlet_name##_instance_id##_cfg, &_zephlet_name##_instance_id##_data,  \
			 _init_fn, _prio)

/**
 * ZEPHLET_INST_DEFINE(type, id, init)
 *
 * Same as `ZEPHLET_PRIO_INST_DEFINE`, with `init_priority == 0`.
 */
#define ZEPHLET_INST_DEFINE(_zephlet_name, _instance_id, _init_fn)                                 \
	ZEPHLET_PRIO_INST_DEFINE(_zephlet_name, _instance_id, _init_fn, 0)

#endif /* MODULES_ZEPHLETS_SHARED_ZEPHLET_H */
