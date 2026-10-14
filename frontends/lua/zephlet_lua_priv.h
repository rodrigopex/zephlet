#ifndef ZEPHLET_LUA_PRIV_H_
#define ZEPHLET_LUA_PRIV_H_

/*
 * Internals shared by the Lua frontend's translation units. Everything here
 * runs on the owner thread (the one that called zephlet_lua_coordinate())
 * unless it says otherwise.
 */

#include <lua.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "zephlet_lua.h"

struct pb_tf_msg;

/** Thread that owns the Lua state; set once by zephlet_lua_coordinate(). */
extern k_tid_t zlet_lua_owner;

/** Non-zero once the main chunk has returned and policies are frozen. */
extern atomic_t zlet_lua_sealed;

/** Raise a Lua error unless called on the owner thread. */
void zlet_lua_check_owner(lua_State *L);

/** Number of zephlet_lua_instance records, and the one at @p idx. */
size_t zlet_lua_instance_count(void);
struct zephlet_lua_instance *zlet_lua_instance_get(size_t idx);
size_t zlet_lua_instance_index(const struct zephlet_lua_instance *inst);

/* zephlet_lua_pb.c */

/** Push @p msg as a new table. A NULL @p tf (no fields) pushes `{}`. */
void zlet_lua_push_msg(lua_State *L, const struct pb_tf_msg *tf, const void *msg);

/**
 * Fill the zeroed struct @p msg from the table at @p idx. Raises a Lua error on
 * an unknown field, a value of the wrong type, or one that does not fit.
 */
void zlet_lua_read_msg(lua_State *L, int idx, const struct pb_tf_msg *tf, void *msg);

/* zephlet_lua_module.c */

/** Register `zephlets` in package preload. */
void zlet_lua_register(lua_State *L);

/** Freeze the handler lists and mark which instances have one. */
void zlet_lua_seal(lua_State *L);

/** Push the (cached) Lua object of @p inst. */
void zlet_lua_push_instance(lua_State *L, struct zephlet_lua_instance *inst);

/** Push the handler list of the instance at @p idx, or nil when it has none. */
void zlet_lua_push_handlers(lua_State *L, size_t idx);

/* zephlet_lua_events.c */

/** Push the table of internal functions the prelude's tasks call. */
void zlet_lua_push_core(lua_State *L);

#endif /* ZEPHLET_LUA_PRIV_H_ */
