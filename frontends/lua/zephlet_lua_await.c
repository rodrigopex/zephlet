/*
 * inst:await() and zephlets.await_any(): per-task event queues.
 *
 * A task's first await on an instance gives it a queue for that instance: a
 * userdata with the sequence numbers, whose uservalue holds the event tables.
 * From then on the drain hands every event of the instance to each of its
 * queues; a full queue drops the newest and counts it.
 *
 * Parking is the prelude's: only Lua code can wait on the doorbell under
 * zephyr.async. Its loop is core.prepare() once, then core.take() and
 * bell:await() until take() returns an event or a timeout.
 */

#include <stdint.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "zephlet_lua_priv.h"

#define DEPTH CONFIG_ZEPHLETS_LUA_AWAIT_DEPTH

struct zlet_lua_queue {
	uint16_t idx;
	uint16_t head;
	uint16_t len;
	/** `seq` of the last event taken, or of the instance at the first await. */
	uint32_t last;
	uint32_t seq[DEPTH];
	/** Arrival order across instances, for await_any. */
	uint32_t serial[DEPTH];
};

/* Registry key: instance index + 1 -> {[task thread] = queue}, weak keys, so a
 * finished task's queues are collected with it. */
static const char queues_key;

static uint32_t serial;

void zlet_lua_await_route(lua_State *L, size_t idx, uint32_t seq)
{
	struct zephlet_lua_instance *inst = zlet_lua_instance_get(idx);
	int ev = lua_gettop(L);

	lua_rawgetp(L, LUA_REGISTRYINDEX, &queues_key);
	if (lua_rawgeti(L, -1, (lua_Integer)idx + 1) != LUA_TTABLE) {
		lua_pop(L, 2);
		return;
	}
	serial++;
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		struct zlet_lua_queue *q = lua_touserdata(L, -1);

		if (q->len == DEPTH) {
			zlet_lua_count_drop(inst, &inst->stats.await_drop, "await queue full");
		} else {
			uint16_t tail = (q->head + q->len) % DEPTH;

			/* The array part is preallocated: storing does not allocate. */
			lua_getiuservalue(L, -1, 1);
			lua_pushvalue(L, ev);
			lua_rawseti(L, -2, (lua_Integer)tail + 1);
			lua_pop(L, 1);
			q->seq[tail] = seq;
			q->serial[tail] = serial;
			q->len++;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 2);
}

/* Push the calling task's queue for @p inst, creating it on its first await. */
static void push_queue(lua_State *L, struct zephlet_lua_instance *inst)
{
	size_t idx = zlet_lua_instance_index(inst);
	struct zlet_lua_queue *q;

	lua_rawgetp(L, LUA_REGISTRYINDEX, &queues_key);
	if (lua_rawgeti(L, -1, (lua_Integer)idx + 1) != LUA_TTABLE) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_createtable(L, 0, 1);
		lua_pushliteral(L, "k");
		lua_setfield(L, -2, "__mode");
		lua_setmetatable(L, -2);
		lua_pushvalue(L, -1);
		lua_rawseti(L, -3, (lua_Integer)idx + 1);
	}
	lua_remove(L, -2);

	(void)lua_pushthread(L);
	if (lua_rawget(L, -2) == LUA_TUSERDATA) {
		lua_remove(L, -2);
		return;
	}
	lua_pop(L, 1);

	/* Hand the events staged so far to their current consumers first: the new
	 * queue starts with the next one. */
	zlet_lua_drain(L);
	(void)atomic_or(&inst->flags, ZEPHLET_LUA_F_CONNECTED | ZEPHLET_LUA_F_AWAITED);

	q = lua_newuserdatauv(L, sizeof(*q), 1);
	memset(q, 0, sizeof(*q));
	q->idx = (uint16_t)idx;
	q->last = (uint32_t)atomic_get(&inst->stats.seq);
	lua_createtable(L, DEPTH, 0);
	lua_setiuservalue(L, -2, 1);

	(void)lua_pushthread(L);
	lua_pushvalue(L, -2);
	lua_rawset(L, -4);
	lua_remove(L, -2);
}

static struct zephlet_lua_instance *check_instance(lua_State *L, int idx, const char *what)
{
	struct zephlet_lua_instance *inst = zlet_lua_to_instance(L, idx);

	if (inst == NULL) {
		luaL_error(L, "%s: expected a zephlet instance, got %s", what,
			   luaL_typename(L, idx));
	}

	return inst;
}

/*
 * core.prepare(x, timeout_ms, any) -> deadline, queue...
 *
 * @p x is one instance, or with @p any an array of them. deadline is nil for
 * no limit, or the uptime in ms at which take() reports a timeout.
 */
static int l_prepare(lua_State *L)
{
	bool any = lua_toboolean(L, 3);
	const char *what = any ? "await_any" : "await";
	lua_Integer ms = ZEPHLET_LUA_FOREVER;
	int n = 1;
	int base;

	zlet_lua_check_owner(L);
	if (!lua_isnoneornil(L, 2)) {
		int isint = 0;

		if (lua_type(L, 2) == LUA_TNUMBER) {
			ms = lua_tointegerx(L, 2, &isint);
		}
		if (!isint || (ms < ZEPHLET_LUA_FOREVER) || (ms > INT32_MAX)) {
			return luaL_error(L,
					  "%s: timeout_ms must be nil, an integer >= 0, or "
					  "zephlets.FOREVER",
					  what);
		}
	}

	if (any) {
		if (!lua_istable(L, 1) || (lua_rawlen(L, 1) == 0U)) {
			return luaL_error(L, "await_any: expected a non-empty array of instances");
		}
		n = (int)lua_rawlen(L, 1);
		for (int i = 1; i <= n; i++) {
			lua_rawgeti(L, 1, i);
			(void)check_instance(L, -1, what);
			lua_pop(L, 1);
		}
	} else {
		(void)check_instance(L, 1, what);
	}
	if (atomic_get(&zlet_lua_sealed) == 0) {
		return luaL_error(L,
				  "%s: only from a zephyr.async task, once the script's main "
				  "chunk has returned",
				  what);
	}

	luaL_checkstack(L, n + 4, "await_any: too many instances");
	base = lua_gettop(L);
	if (ms == ZEPHLET_LUA_FOREVER) {
		lua_pushnil(L);
	} else {
		lua_pushinteger(L, (lua_Integer)(int32_t)(k_uptime_get_32() + (uint32_t)ms));
	}
	for (int i = 1; i <= n; i++) {
		if (any) {
			lua_rawgeti(L, 1, i);
			push_queue(L, zlet_lua_to_instance(L, -1));
			lua_remove(L, -2);
		} else {
			push_queue(L, zlet_lua_to_instance(L, 1));
		}
	}

	return lua_gettop(L) - base;
}

/*
 * core.take(deadline, queue...) -> ev, meta | nil, "timeout" | nil, nil, park
 *
 * Drains staging, then returns the oldest event of the queues. Otherwise says
 * how long to park on the doorbell: park is nil for as long as it takes.
 */
static int l_take(lua_State *L)
{
	struct zlet_lua_queue *best = NULL;
	int best_at = 0;
	int32_t park = CONFIG_ZEPHLETS_LUA_POLL_MS;

	zlet_lua_drain(L);
	for (int i = 2; i <= lua_gettop(L); i++) {
		struct zlet_lua_queue *q = lua_touserdata(L, i);

		if ((q->len > 0U) &&
		    ((best == NULL) ||
		     ((int32_t)(q->serial[q->head] - best->serial[best->head]) < 0))) {
			best = q;
			best_at = i;
		}
	}

	if (best != NULL) {
		struct zephlet_lua_instance *inst = zlet_lua_instance_get(best->idx);
		uint16_t h = best->head;
		uint32_t seq = best->seq[h];
		int32_t gap = (int32_t)(seq - best->last - 1U);

		lua_getiuservalue(L, best_at, 1);
		lua_rawgeti(L, -1, (lua_Integer)h + 1);

		/* meta first: if it cannot be allocated, the event stays queued. */
		lua_createtable(L, 0, 4);
		zlet_lua_push_instance(L, inst);
		lua_setfield(L, -2, "source");
		lua_pushstring(L, inst->z->name);
		lua_setfield(L, -2, "name");
		lua_pushinteger(L, (lua_Integer)seq);
		lua_setfield(L, -2, "seq");
		lua_pushinteger(L, (gap > 0) ? gap : 0);
		lua_setfield(L, -2, "dropped");

		lua_pushnil(L);
		lua_rawseti(L, -4, (lua_Integer)h + 1);
		best->last = seq;
		best->head = (h + 1U) % DEPTH;
		best->len--;

		return 2;
	}

	if (!lua_isnil(L, 1)) {
		int32_t left =
			(int32_t)((uint32_t)lua_tointeger(L, 1) - (uint32_t)k_uptime_get_32());

		if (left <= 0) {
			lua_pushnil(L);
			lua_pushliteral(L, "timeout");
			return 2;
		}
		if ((park == 0) || (left < park)) {
			park = left;
		}
	}

	lua_pushnil(L);
	lua_pushnil(L);
	if (park > 0) {
		lua_pushinteger(L, park);
	} else {
		lua_pushnil(L);
	}

	return 3;
}

void zlet_lua_await_core(lua_State *L)
{
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &queues_key);

	lua_pushcfunction(L, l_prepare);
	lua_setfield(L, -2, "prepare");
	lua_pushcfunction(L, l_take);
	lua_setfield(L, -2, "take");
}
