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
 * Push field @p name of @p msg. Returns 1 when pushed, 0 when the field is
 * absent (an unset optional or oneof member; nothing pushed), -1 when the
 * message has no such field.
 */
int zlet_lua_push_msg_field(lua_State *L, const struct pb_tf_msg *tf, const void *msg,
			    const char *name);

/**
 * Push the name and value of the first present field at index @p from or
 * after; returns its index, or -1, with nothing pushed, when there is none.
 */
int zlet_lua_next_msg_field(lua_State *L, const struct pb_tf_msg *tf, const void *msg, int from);

/** Index of field @p name in @p tf, or -1. */
int zlet_lua_msg_field_index(const struct pb_tf_msg *tf, const char *name);

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

/** The instance behind the Lua object at @p idx, or NULL if it is not one. */
struct zephlet_lua_instance *zlet_lua_to_instance(lua_State *L, int idx);

/** Push the handler list of the instance at @p idx, or nil when it has none. */
void zlet_lua_push_handlers(lua_State *L, size_t idx);

/* zephlet_lua_events.c */

/** Push the table of internal functions the prelude's tasks call. */
void zlet_lua_push_core(lua_State *L);

/**
 * Move every staged event to its consumers: the policy FIFO and the queues of
 * the tasks awaiting its source.
 */
void zlet_lua_drain(lua_State *L);

/** Fold the drops of staging's start-up overwrites into staged_drop. At seal. */
void zlet_lua_fold_preseal_drops(void);

/**
 * The frontend has stopped: stop staging, and empty it, so the listeners return
 * at once from then on.
 */
void zlet_lua_shutdown(void);

/** Count a dropped event of @p inst in @p counter; logs, rate-limited. */
void zlet_lua_count_drop(struct zephlet_lua_instance *inst, atomic_t *counter, const char *why);

/* zephlet_lua_event.c */

/** Push a read-only event object of @p type, holding a copy of @p ev. */
void zlet_lua_push_event(lua_State *L, const struct zephlet_lua_type *type, const void *ev);

/** Create the event objects' metatable. */
void zlet_lua_event_register(lua_State *L);

/* zephlet_lua_await.c */

/**
 * Queue the event object at the top of the stack, without popping it, for every
 * task awaiting the instance at @p idx. Never allocates.
 */
void zlet_lua_await_route(lua_State *L, size_t idx);

/**
 * Forget the queues of finished tasks awaiting the instance at @p idx. Returns
 * whether a live one is left; when none is, the instance stops being awaited.
 * Never allocates.
 */
bool zlet_lua_await_prune(lua_State *L, size_t idx);

/** Add the await functions to the core table at the top of the stack. */
void zlet_lua_await_core(lua_State *L);

/* zephlet_lua_coordinate.c */

/**
 * Registry key of the table the prelude returned: its tasks, `pump` and
 * `dispatch`, and the Lua functions behind `inst:await()` and
 * `zephlets.await_any()`, which park the calling task.
 */
extern const char zlet_lua_prelude_key;

#endif /* ZEPHLET_LUA_PRIV_H_ */
