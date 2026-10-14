/*
 * Event objects: one userdata per event, holding a copy of its C struct.
 *
 * A field is converted only when the script reads it, through the type's
 * nanopb-textformat descriptor, so an event costs one allocation however many
 * fields it has. Scalars and strings come back as Lua values; a submessage or
 * a repeated field builds a new table on each read. Events are read-only:
 * every consumer of an event gets the same object.
 */

#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include <zephyr/sys/util.h>

#include <nanopb_textformat.h>

#include "zephlet_lua_priv.h"

#define EVENT_MT "zephlets.event"

struct zlet_lua_event {
	const struct zephlet_lua_type *type;
	/* Followed by the events struct, at PAYLOAD_OFFSET. */
};

#define PAYLOAD_OFFSET ROUND_UP(sizeof(struct zlet_lua_event), 8)

static const void *payload(const struct zlet_lua_event *e)
{
	return (const uint8_t *)e + PAYLOAD_OFFSET;
}

void zlet_lua_push_event(lua_State *L, const struct zephlet_lua_type *type, const void *ev)
{
	size_t size = (type->events_tf != NULL) ? type->events_tf->struct_size : 0U;
	struct zlet_lua_event *e = lua_newuserdatauv(L, PAYLOAD_OFFSET + size, 0);

	e->type = type;
	memcpy((uint8_t *)e + PAYLOAD_OFFSET, ev, size);
	luaL_setmetatable(L, EVENT_MT);
}

static int l_event_index(lua_State *L)
{
	struct zlet_lua_event *e = luaL_checkudata(L, 1, EVENT_MT);
	const char *key = lua_tostring(L, 2);
	int rc = -1;

	if (lua_type(L, 2) == LUA_TSTRING) {
		rc = zlet_lua_push_msg_field(L, e->type->events_tf, payload(e), key);
	}
	if (rc < 0) {
		return luaL_error(L, "zephlets: %s event has no field '%s'", e->type->name,
				  (key != NULL) ? key : luaL_typename(L, 2));
	}
	if (rc == 0) {
		lua_pushnil(L);
	}

	return 1;
}

static int l_event_newindex(lua_State *L)
{
	return luaL_error(L, "zephlets: events are read-only");
}

/* The __pairs iterator: the present fields, in tag order. */
static int l_event_next(lua_State *L)
{
	struct zlet_lua_event *e = luaL_checkudata(L, 1, EVENT_MT);
	const struct pb_tf_msg *tf = e->type->events_tf;
	int from = 0;

	if (!lua_isnoneornil(L, 2)) {
		from = zlet_lua_msg_field_index(tf, luaL_checkstring(L, 2));
		if (from < 0) {
			return luaL_error(L, "zephlets: invalid key to 'next'");
		}
		from++;
	}
	if (zlet_lua_next_msg_field(L, tf, payload(e), from) < 0) {
		lua_pushnil(L);
		return 1;
	}

	return 2;
}

static int l_event_pairs(lua_State *L)
{
	(void)luaL_checkudata(L, 1, EVENT_MT);
	lua_pushcfunction(L, l_event_next);
	lua_pushvalue(L, 1);
	lua_pushnil(L);

	return 3;
}

static int l_event_tostring(lua_State *L)
{
	struct zlet_lua_event *e = luaL_checkudata(L, 1, EVENT_MT);

	lua_pushfstring(L, "zephlets event (%s)", e->type->name);

	return 1;
}

void zlet_lua_event_register(lua_State *L)
{
	static const luaL_Reg meta[] = {
		{"__index", l_event_index},
		{"__newindex", l_event_newindex},
		{"__pairs", l_event_pairs},
		{"__tostring", l_event_tostring},
		{NULL, NULL},
	};

	luaL_newmetatable(L, EVENT_MT);
	luaL_setfuncs(L, meta, 0);
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");
	lua_pop(L, 1);
}
