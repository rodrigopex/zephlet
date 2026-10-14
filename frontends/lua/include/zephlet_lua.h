#ifndef ZEPHLET_LUA_H_
#define ZEPHLET_LUA_H_

/**
 * @file
 * @brief Lua frontend: the `zephlets` Lua module over every zephlet instance.
 *
 * The application hands a Lua script to zephlet_lua_coordinate() from main().
 * The script reaches instances by name as fields of the module, calls their
 * commands with `:` syntax, and connects handlers to their events:
 *
 * @code{.lua}
 * local z = require("zephlets")
 * z.tick_fast.events:connect(function(ev, meta) z.ui_main:blink(100) end)
 * @endcode
 *
 * Nothing here needs Lua headers: generated code and application code include
 * this file to declare instances and start the frontend, nothing more.
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/iterable_sections.h>

#include "zephlet.h"

struct pb_tf_msg;

/** @brief One command of a zephlet type, as the Lua frontend calls it. */
struct zephlet_lua_method {
	/** RPC name, which is also its Lua method name. */
	const char *name;
	/** Calls the generated typed wrapper; @p req / @p resp are the frontend's storage. */
	int (*invoke)(const struct zephlet *z, const void *req, void *resp, k_timeout_t timeout);
	/** Request descriptor; NULL when the request has no fields. */
	const struct pb_tf_msg *req_tf;
	/** Response descriptor; NULL when the response has no fields. */
	const struct pb_tf_msg *resp_tf;
	/** sizeof() the request struct; 0 for Empty, which takes no argument. */
	uint16_t req_size;
	/** sizeof() the response struct; 0 for Empty, which returns `true`. */
	uint16_t resp_size;
};

/** @brief One named value of a proto enum. */
struct zephlet_lua_enum_value {
	const char *name;
	int32_t value;
};

/** @brief A proto enum, exposed read-only on every instance of its type. */
struct zephlet_lua_enum {
	const char *name;
	const struct zephlet_lua_enum_value *values;
	uint16_t count;
};

/** @brief Everything the Lua frontend knows about one zephlet type. Generated. */
struct zephlet_lua_type {
	const char *name;
	const struct zephlet_lua_method *methods;
	uint16_t num_methods;
	/** Events descriptor; NULL when Events has no fields. */
	const struct pb_tf_msg *events_tf;
	const struct zephlet_lua_enum *enums;
	uint16_t num_enums;
};

/** @brief Per-instance counters, readable from Lua with `z.stats()`. */
struct zephlet_lua_stats {
	/** Events emitted by the instance, dropped ones included; `meta.seq`. */
	atomic_t seq;
	/** Events dropped because every staging slot was taken. */
	atomic_t staged_drop;
	/** Events dropped because the Lua-heap FIFO was full or out of memory. */
	atomic_t fifo_drop;
	/** Doorbells that could not be published; the poll period recovers them. */
	atomic_t doorbell_fail;
	/** Handler calls that raised an error. */
	atomic_t handler_err;
	/** Events handed to the handlers. */
	atomic_t delivered;
};

/**
 * @brief Lua frontend record of one zephlet instance.
 *
 * Emitted by the per-type hook that ZEPHLET_NEW expands, one per instance, so
 * the frontend discovers instances by walking this section. Lives in RAM for
 * the flags and counters.
 */
struct zephlet_lua_instance {
	const struct zephlet *z;
	const struct zephlet_lua_type *type;
	/** ZEPHLET_LUA_F_* */
	atomic_t flags;
	struct zephlet_lua_stats stats;
	/** `seq` of the last event handed to Lua; `meta.dropped` is the gap. */
	uint32_t last_seq;
};

/** Set at seal on an instance with at least one handler. */
#define ZEPHLET_LUA_F_CONNECTED BIT(0)

/** `timeout_ms` value meaning K_FOREVER, also exported to Lua as `z.FOREVER`. */
#define ZEPHLET_LUA_FOREVER (-1)

/**
 * @brief Copy one event into a staging slot. Called by the generated listener.
 *
 * Runs on the system workqueue. Never blocks and never touches Lua: when every
 * slot is taken the event is dropped and counted.
 *
 * @param inst  Source instance.
 * @param ev    Event struct; valid only for the duration of the call.
 * @param size  sizeof() the event struct.
 */
void zephlet_lua_stage(struct zephlet_lua_instance *inst, const void *ev, size_t size);

/**
 * @brief Arguments of zephlet_lua_coordinate() for a script embedded with
 * luaz_add_bytecode_file().
 *
 * `luaz_add_bytecode_file(src/app.lua)` in CMakeLists.txt generates
 * `app_lua_bytecode.h`; include it, then:
 *
 * @code{.c}
 * return zephlet_lua_coordinate(ZEPHLET_LUA_SCRIPT(app));
 * @endcode
 *
 * @param name  The script's file name without `.lua`. Macros are expanded.
 */
#define ZEPHLET_LUA_SCRIPT(name) Z_ZEPHLET_LUA_SCRIPT(name)
#define Z_ZEPHLET_LUA_SCRIPT(name)                                                                 \
	(const char *)name##_lua_bytecode, name##_lua_bytecode_len, "=" #name

/**
 * @brief Run a Lua script against the zephlet instances. Call from main().
 *
 * Creates the Lua state on its own heap, runs the script's main chunk -- which
 * connects handlers and may call commands -- then seals the connections and
 * enters the zephyr.async loop. If the chunk returns a function, that function
 * runs as one more task. Does not return while the script runs.
 *
 * The calling thread owns the Lua state: commands run their handlers on its
 * stack, so size CONFIG_MAIN_STACK_SIZE accordingly.
 *
 * @param buf        Precompiled bytecode (see ZEPHLET_LUA_SCRIPT()), or source
 *                   when the build keeps the Lua parser.
 * @param len        Length of @p buf in bytes.
 * @param chunkname  Name used in Lua error messages.
 *
 * The frontend starts once: after any return, including a failure before the
 * Lua state exists, every later call returns -EALREADY, and the instances stop
 * staging events.
 *
 * @retval -EALREADY  The frontend has already been started.
 * @retval -ENOMEM    The heap is too small to start: for the Lua state, the
 *                    libraries, the frontend's own prelude or the main chunk.
 * @retval -ENOEXEC   The script did not load: not bytecode for this Lua, or a
 *                    syntax error in source.
 * @retval -EINVAL    The main chunk raised an error other than running out of
 *                    memory, or returned something
 *                    other than nil or a function.
 * @retval -EFAULT    The scheduler stopped on a fault, or the frontend's own
 *                    prelude failed to load or run: a build that mixes Lua
 *                    versions.
 */
int zephlet_lua_coordinate(const char *buf, size_t len, const char *chunkname);

#endif /* ZEPHLET_LUA_H_ */
