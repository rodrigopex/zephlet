/*
 * Event transport for the Lua frontend.
 *
 *   zephlet emit -> forwarding listener (system workqueue)
 *                -> C staging slot + doorbell on `zlet_lua_bell`
 *                -> pump/dispatcher tasks (Lua thread) drain into the Lua-heap FIFO
 *                -> dispatcher pops and runs the handlers
 *
 * The listener only copies into a fixed slot: nothing on the workqueue touches
 * the Lua state, whose allocator and VM are not thread-safe. The doorbell is
 * the one channel the zephyr.async scheduler watches for us; it carries no
 * data, only "staging is not empty", so a lost or coalesced doorbell loses no
 * event. Both tasks drain, so staging keeps emptying while the dispatcher is
 * parked inside a handler.
 */

#include <errno.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include <luaz_msg_descr.h>
#include <luaz_zbus.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

#include "zephlet_lua_priv.h"

LOG_MODULE_DECLARE(zlet_lua);

/* ----- Doorbell --------------------------------------------------------- */

struct zlet_lua_bell {
	int32_t n;
};

static const struct lua_msg_field_descr bell_fields[] = {
	LUA_MSG_FIELD(struct zlet_lua_bell, n, LUA_MSG_TYPE_INT),
};

ZBUS_CHAN_DEFINE(zlet_lua_bell, struct zlet_lua_bell, NULL,
		 LUA_ZBUS_MSG_DESCR(struct zlet_lua_bell, bell_fields), ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

/* Set while a doorbell is outstanding, so a burst rings it once. */
static atomic_t bell_pending;

/* ----- Staging (shared with the workqueue) ------------------------------ */

struct zlet_lua_slot {
	uint16_t idx;
	uint32_t seq;
	uint8_t payload[CONFIG_ZEPHLETS_LUA_SLOT_SIZE] __aligned(8);
};

static struct zlet_lua_slot staging[CONFIG_ZEPHLETS_LUA_STAGING_DEPTH];
static size_t staging_head;
static size_t staging_len;
static struct k_spinlock staging_lock;

atomic_t zlet_lua_sealed;

static void count_drop(struct zephlet_lua_instance *inst, atomic_t *counter, const char *why)
{
	atomic_val_t n = atomic_inc(counter) + 1;

	/* Powers of two: the first drop and then ever more rarely, never silent. */
	if ((n & (n - 1)) == 0) {
		LOG_WRN("%s: %ld event(s) dropped (%s)", inst->z->name, (long)n, why);
	}
}

void zephlet_lua_stage(struct zephlet_lua_instance *inst, const void *ev, size_t size)
{
	uint32_t seq = (uint32_t)atomic_inc(&inst->stats.seq) + 1U;
	struct zlet_lua_slot *slot;
	k_spinlock_key_t key;

	if ((atomic_get(&zlet_lua_sealed) != 0) &&
	    ((atomic_get(&inst->flags) & ZEPHLET_LUA_F_CONNECTED) == 0)) {
		return;
	}

	key = k_spin_lock(&staging_lock);
	if (staging_len == ARRAY_SIZE(staging)) {
		k_spin_unlock(&staging_lock, key);
		count_drop(inst, &inst->stats.staged_drop, "staging full");
		return;
	}
	slot = &staging[(staging_head + staging_len) % ARRAY_SIZE(staging)];
	slot->idx = (uint16_t)zlet_lua_instance_index(inst);
	slot->seq = seq;
	memcpy(slot->payload, ev, MIN(size, sizeof(slot->payload)));
	staging_len++;
	k_spin_unlock(&staging_lock, key);

	if (atomic_cas(&bell_pending, 0, 1)) {
		struct zlet_lua_bell msg = {.n = 1};

		if (zbus_chan_pub(&zlet_lua_bell, &msg, K_NO_WAIT) != 0) {
			/* The next event rings again; the poll period covers a quiet source. */
			atomic_clear(&bell_pending);
			(void)atomic_inc(&inst->stats.doorbell_fail);
		}
	}
}

/* Copy out the oldest staged event. Owner thread only. */
static bool staging_take(struct zlet_lua_slot *out)
{
	k_spinlock_key_t key = k_spin_lock(&staging_lock);
	bool got = staging_len > 0U;

	if (got) {
		const struct zlet_lua_slot *slot = &staging[staging_head];

		/* Only the event's own bytes: a slot is sized for the largest type. */
		out->idx = slot->idx;
		out->len = slot->len;
		memcpy(out->payload, slot->payload, slot->len);
		staging_head = (staging_head + 1U) % ARRAY_SIZE(staging);
		staging_len--;
	}
	k_spin_unlock(&staging_lock, key);

	return got;
}

void zlet_lua_fold_preseal_drops(void)
{
	size_t n = zlet_lua_instance_count();

	for (size_t i = 0U; i < n; i++) {
		struct zephlet_lua_instance *inst = zlet_lua_instance_get(i);
		atomic_val_t lost = atomic_set(&inst->preseal_drop, 0);

		if ((lost > 0) && ((atomic_get(&inst->flags) & ZEPHLET_LUA_F_CONNECTED) != 0)) {
			(void)atomic_add(&inst->stats.staged_drop, lost);
			LOG_WRN("%s: %ld event(s) dropped before the script started (staging full)",
				inst->z->name, (long)lost);
		}
	}
}

void zlet_lua_shutdown(void)
{
	size_t n = zlet_lua_instance_count();
	k_spinlock_key_t key;

	atomic_set(&zlet_lua_sealed, 1);
	for (size_t i = 0U; i < n; i++) {
		atomic_clear(&zlet_lua_instance_get(i)->flags);
	}
	key = k_spin_lock(&staging_lock);
	staging_head = 0U;
	staging_len = 0U;
	k_spin_unlock(&staging_lock, key);
}

/* ----- Lua-heap FIFO (owner thread only) -------------------------------- */

/* Registry key of the table that holds the queued event tables. */
static const char fifo_key;

static uint16_t fifo_src[CONFIG_ZEPHLETS_LUA_FIFO_DEPTH];
static uint32_t fifo_seq[CONFIG_ZEPHLETS_LUA_FIFO_DEPTH];
static size_t fifo_head;
static size_t fifo_len;

/* Being drained: kept off the stack, since a slot can be several KB. */
static struct zlet_lua_slot drain_slot;

/* Instance whose event core.pop() was building, for core.lost(). */
static int pop_inflight = -1;

static int l_convert(lua_State *L)
{
	const struct zephlet_lua_instance *inst = lua_touserdata(L, 1);

	zlet_lua_push_msg(L, inst->type->events_tf, drain_slot.payload);

	return 1;
}

/* core.drain(): move every staged event into the FIFO. */
static int l_drain(lua_State *L)
{
	zlet_lua_check_owner(L);
	atomic_clear(&bell_pending);

	lua_rawgetp(L, LUA_REGISTRYINDEX, &fifo_key);
	while (staging_take(&drain_slot)) {
		struct zephlet_lua_instance *inst = zlet_lua_instance_get(drain_slot.idx);
		size_t tail;

		if ((atomic_get(&inst->flags) & ZEPHLET_LUA_F_CONNECTED) == 0) {
			/* Staged before the seal by a source nothing listens to. */
			continue;
		}
		if (fifo_len == ARRAY_SIZE(fifo_src)) {
			count_drop(inst, &inst->stats.fifo_drop, "fifo full");
			continue;
		}

		lua_pushcfunction(L, l_convert);
		lua_pushlightuserdata(L, inst);
		if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
			LOG_ERR("%s: event conversion failed: %s", inst->z->name,
				lua_tostring(L, -1));
			lua_pop(L, 1);
			count_drop(inst, &inst->stats.fifo_drop, "out of memory");
			continue;
		}

		tail = (fifo_head + fifo_len) % ARRAY_SIZE(fifo_src);
		lua_rawseti(L, -2, (lua_Integer)tail + 1);
		fifo_src[tail] = drain_slot.idx;
		fifo_seq[tail] = drain_slot.seq;
		fifo_len++;
	}
	lua_pop(L, 1);

	return 0;
}

/* core.pop() -> handlers, event, meta; or nil when the FIFO is empty. */
static int l_pop(lua_State *L)
{
	struct zephlet_lua_instance *inst;
	size_t head = fifo_head;
	uint16_t idx;

	zlet_lua_check_owner(L);
	if (fifo_len == 0U) {
		lua_pushnil(L);
		return 1;
	}

	/* Taken off first: if building meta runs out of memory, the event is lost
	 * (and counted by core.lost()) rather than retried forever. */
	idx = fifo_src[head];
	fifo_head = (fifo_head + 1U) % ARRAY_SIZE(fifo_src);
	fifo_len--;
	inst = zlet_lua_instance_get(idx);
	pop_inflight = idx;

	zlet_lua_push_handlers(L, idx);

	lua_rawgetp(L, LUA_REGISTRYINDEX, &fifo_key);
	lua_rawgeti(L, -1, (lua_Integer)head + 1);
	lua_pushnil(L);
	lua_rawseti(L, -3, (lua_Integer)head + 1);
	lua_remove(L, -2);

	lua_createtable(L, 0, 4);
	zlet_lua_push_instance(L, inst);
	lua_setfield(L, -2, "source");
	lua_pushstring(L, inst->z->name);
	lua_setfield(L, -2, "name");
	lua_pushinteger(L, (lua_Integer)fifo_seq[head]);
	lua_setfield(L, -2, "seq");
	/* Every event of a connected source is staged or counted as dropped, so
	 * the gap in its sequence is exactly what it lost since the last one. */
	lua_pushinteger(L, (lua_Integer)(fifo_seq[head] - inst->last_seq - 1U));
	lua_setfield(L, -2, "dropped");
	inst->last_seq = fifo_seq[head];

	(void)atomic_inc(&inst->stats.delivered);
	pop_inflight = -1;

	return 3;
}

/* core.lost(err): core.pop() raised; count its event as dropped. */
static int l_lost(lua_State *L)
{
	if (pop_inflight >= 0) {
		struct zephlet_lua_instance *inst = zlet_lua_instance_get((size_t)pop_inflight);

		LOG_ERR("%s: event lost: %s", inst->z->name, luaL_optstring(L, 1, "?"));
		count_drop(inst, &inst->stats.fifo_drop, "out of memory");
		pop_inflight = -1;
	}

	return 0;
}

/* core.handler_error(name, err) */
static int l_handler_error(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	const char *err = luaL_optstring(L, 2, "(error object is not a string)");

	for (size_t i = 0U; i < zlet_lua_instance_count(); i++) {
		struct zephlet_lua_instance *inst = zlet_lua_instance_get(i);

		if (strcmp(inst->z->name, name) == 0) {
			(void)atomic_inc(&inst->stats.handler_err);
			break;
		}
	}
	LOG_ERR("%s handler: %s", name, err);

	return 0;
}

/* core.bell(): the doorbell channel, as a zephyr.async-awaitable object. */
static int l_bell(lua_State *L)
{
	const struct zbus_channel **ud = lua_newuserdatauv(L, sizeof(*ud), 0);

	*ud = &zlet_lua_bell;
	/* Created by require("zephyr.async"), which the prelude calls first. */
	luaL_setmetatable(L, ZBUS_CHAN_METATABLE);

	return 1;
}

void zlet_lua_push_core(lua_State *L)
{
	static const luaL_Reg funcs[] = {
		{"drain", l_drain}, {"pop", l_pop},
		{"lost", l_lost},   {"handler_error", l_handler_error},
		{"bell", l_bell},   {NULL, NULL},
	};

	lua_createtable(L, (int)ARRAY_SIZE(fifo_src), 0);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &fifo_key);

	luaL_newlib(L, funcs);
	if (CONFIG_ZEPHLETS_LUA_POLL_MS > 0) {
		lua_pushinteger(L, CONFIG_ZEPHLETS_LUA_POLL_MS);
		lua_setfield(L, -2, "POLL_MS");
	}
}
