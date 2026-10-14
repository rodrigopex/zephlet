/*
 * The `zephlets` Lua module.
 *
 *   z.<instance>                    instance object, looked up in the
 *                                   zephlet_lua_instance section on first use
 *   inst:<rpc>([req,] timeout_ms)   command, through the generated typed wrapper
 *   inst:add_policy(fn)             handler for the instance's events; only
 *                                   before the seal
 *   inst:await([timeout_ms])        next event, from a zephyr.async task
 *   inst.<Enum>.<VALUE>             proto enum constants
 *   z.await_any(insts[, timeout_ms]) next event of any of @p insts
 *   z.stats(), z.FOREVER
 *
 * await and await_any live in the prelude (Lua): they park the task.
 *
 * The module and instance objects are read-only. No table mirrors the instance
 * registry: lookups walk the section, and only the instances a script actually
 * names are cached.
 */

#include <errno.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>

#include "zephlet_lua_priv.h"

LOG_MODULE_DECLARE(zlet_lua);

#define INSTANCE_MT_PREFIX "zephlets.instance."

/* Registry keys. */
static const char cache_key;    /* name -> instance object */
static const char handlers_key; /* instance index + 1 -> array of functions */

/* Command request/response storage. The Lua state has one owner thread and a
 * command completes before the next can start, so one pair is enough. */
static uint8_t req_buf[CONFIG_ZEPHLETS_LUA_SCRATCH_SIZE] __aligned(8);
static uint8_t resp_buf[CONFIG_ZEPHLETS_LUA_SCRATCH_SIZE] __aligned(8);

k_tid_t zlet_lua_owner;

struct instance_ud {
	struct zephlet_lua_instance *inst;
};

void zlet_lua_check_owner(lua_State *L)
{
	if (k_current_get() != zlet_lua_owner) {
		luaL_error(L, "zephlets: called from a thread that does not own the Lua state");
	}
}

size_t zlet_lua_instance_count(void)
{
	int n;

	STRUCT_SECTION_COUNT(zephlet_lua_instance, &n);

	return (size_t)n;
}

struct zephlet_lua_instance *zlet_lua_instance_get(size_t idx)
{
	struct zephlet_lua_instance *inst;

	STRUCT_SECTION_GET(zephlet_lua_instance, idx, &inst);

	return inst;
}

size_t zlet_lua_instance_index(const struct zephlet_lua_instance *inst)
{
	return (size_t)(inst - zlet_lua_instance_get(0));
}

static int l_readonly(lua_State *L)
{
	return luaL_error(L, "zephlets: '%s' is read-only", luaL_tolstring(L, 2, NULL));
}

static const char *errno_name(int rc)
{
	switch (-rc) {
	case EPERM:
		return "EPERM";
	case ENOENT:
		return "ENOENT";
	case EIO:
		return "EIO";
	case EAGAIN:
		return "EAGAIN";
	case ENOMEM:
		return "ENOMEM";
	case EACCES:
		return "EACCES";
	case EFAULT:
		return "EFAULT";
	case EBUSY:
		return "EBUSY";
	case ENODEV:
		return "ENODEV";
	case EINVAL:
		return "EINVAL";
	case ENOSPC:
		return "ENOSPC";
	case ERANGE:
		return "ERANGE";
	case ENOSYS:
		return "ENOSYS";
	case ENOMSG:
		return "ENOMSG";
	case ENODATA:
		return "ENODATA";
	case ETIME:
		return "ETIME";
	case ENOBUFS:
		return "ENOBUFS";
	case ENOTSUP:
		return "ENOTSUP";
	case ETIMEDOUT:
		return "ETIMEDOUT";
	case EALREADY:
		return "EALREADY";
	case EINPROGRESS:
		return "EINPROGRESS";
	case ECANCELED:
		return "ECANCELED";
	case EMSGSIZE:
		return "EMSGSIZE";
	case EBADMSG:
		return "EBADMSG";
	default:
		return "EUNKNOWN";
	}
}

/* ----- Commands --------------------------------------------------------- */

static k_timeout_t check_timeout(lua_State *L, int idx, const char *name)
{
	int isint = 0;
	lua_Integer ms = 0;

	if (lua_type(L, idx) == LUA_TNUMBER) {
		ms = lua_tointegerx(L, idx, &isint);
	}
	if (!isint || (ms < ZEPHLET_LUA_FOREVER)) {
		luaL_error(L, "%s: timeout_ms must be an integer >= 0, or zephlets.FOREVER", name);
	}

	if (ms == ZEPHLET_LUA_FOREVER) {
		return K_FOREVER;
	}
	if (ms == 0) {
		return K_NO_WAIT;
	}

	return K_MSEC(ms);
}

/* Set while a command runs, req_buf and resp_buf in use. */
static bool in_command;

/*
 * The command proper, under the protected call l_call() makes: in_command is
 * cleared however it ends. Arguments: method (light userdata), the type's
 * instance metatable, then the caller's own.
 */
static int call_protected(lua_State *L)
{
	const struct zephlet_lua_method *m = lua_touserdata(L, 1);
	bool takes_req = m->req_size > 0U;
	int nargs = takes_req ? 3 : 2;
	struct instance_ud *self;
	k_timeout_t timeout;
	int rc;

	self = lua_touserdata(L, 3);
	if ((self == NULL) || !lua_getmetatable(L, 3) || !lua_rawequal(L, -1, 2)) {
		return luaL_error(L, "%s: call it as a method, inst:%s(...)", m->name, m->name);
	}
	lua_pop(L, 1);
	lua_remove(L, 1);
	lua_remove(L, 1);

	if (lua_gettop(L) != nargs) {
		return luaL_error(L, "%s:%s: expected %s, got %d argument(s)", self->inst->z->name,
				  m->name, takes_req ? "(req, timeout_ms)" : "(timeout_ms)",
				  lua_gettop(L) - 1);
	}
	timeout = check_timeout(L, nargs, m->name);

	if (takes_req) {
		luaL_checktype(L, 2, LUA_TTABLE);
		memset(req_buf, 0, m->req_size);
		zlet_lua_read_msg(L, 2, m->req_tf, req_buf);
	}
	if (m->resp_size > 0U) {
		memset(resp_buf, 0, m->resp_size);
	}

	rc = m->invoke(self->inst->z, req_buf, resp_buf, timeout);
	if (rc != 0) {
		lua_pushnil(L);
		lua_pushstring(L, errno_name(rc));
		lua_pushinteger(L, rc);
		return 3;
	}

	if (m->resp_size > 0U) {
		zlet_lua_push_msg(L, m->resp_tf, resp_buf);
	} else {
		lua_pushboolean(L, 1);
	}

	return 1;
}

/* Upvalues: 1 = method (light userdata), 2 = the type's instance metatable. */
static int l_call(lua_State *L)
{
	const struct zephlet_lua_method *m = lua_touserdata(L, lua_upvalueindex(1));
	int rc;

	zlet_lua_check_owner(L);
	/* Building a request or response allocates, and an allocation can run a
	 * __gc metamethod: one that called a command would overwrite the buffers
	 * of the command under way. */
	if (in_command) {
		return luaL_error(L,
				  "%s: a command cannot run while another is converting "
				  "its request or response (from a __gc metamethod?)",
				  m->name);
	}

	lua_pushcfunction(L, call_protected);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushvalue(L, lua_upvalueindex(2));
	lua_rotate(L, 1, 3);
	in_command = true;
	rc = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
	in_command = false;
	if (rc != LUA_OK) {
		return lua_error(L);
	}

	return lua_gettop(L);
}

/* ----- Events ----------------------------------------------------------- */

void zlet_lua_push_handlers(lua_State *L, size_t idx)
{
	lua_rawgetp(L, LUA_REGISTRYINDEX, &handlers_key);
	lua_rawgeti(L, -1, (lua_Integer)idx + 1);
	lua_remove(L, -2);
}

/* Upvalue 1: the type's instance metatable. */
static int l_add_policy(lua_State *L)
{
	struct instance_ud *self = lua_touserdata(L, 1);
	size_t idx;

	zlet_lua_check_owner(L);
	if ((self == NULL) || !lua_getmetatable(L, 1) ||
	    !lua_rawequal(L, -1, lua_upvalueindex(1))) {
		return luaL_error(L, "add_policy: call it as a method, inst:add_policy(fn)");
	}
	lua_pop(L, 1);

	if (atomic_get(&zlet_lua_sealed) != 0) {
		return luaL_error(L,
				  "%s:add_policy: policies are fixed once the script's main chunk "
				  "has returned",
				  self->inst->z->name);
	}
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (lua_gettop(L) != 2) {
		return luaL_error(L, "%s:add_policy: expected one function", self->inst->z->name);
	}

	idx = zlet_lua_instance_index(self->inst);
	lua_rawgetp(L, LUA_REGISTRYINDEX, &handlers_key);
	if (lua_rawgeti(L, -1, (lua_Integer)idx + 1) == LUA_TNIL) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_rawseti(L, -3, (lua_Integer)idx + 1);
	}
	lua_pushvalue(L, 2);
	lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);

	return 0;
}

/* ----- Instances -------------------------------------------------------- */

/* Push the prelude's function @p name. */
static void push_prelude_fn(lua_State *L, const char *name)
{
	lua_rawgetp(L, LUA_REGISTRYINDEX, &zlet_lua_prelude_key);
	lua_getfield(L, -1, name);
	lua_remove(L, -2);
}

/* Upvalue 1: the type's member table (methods and enums). */
static int l_instance_index(lua_State *L)
{
	struct instance_ud *self = lua_touserdata(L, 1);
	const char *key = lua_tostring(L, 2);

	lua_pushvalue(L, 2);
	if (lua_rawget(L, lua_upvalueindex(1)) != LUA_TNIL) {
		return 1;
	}
	lua_pop(L, 1);

	return luaL_error(L, "zephlets: %s (%s) has no field '%s'", self->inst->z->name,
			  self->inst->type->name, (key != NULL) ? key : "?");
}

static int l_instance_tostring(lua_State *L)
{
	struct instance_ud *self = lua_touserdata(L, 1);

	lua_pushfstring(L, "zephlet %s (%s)", self->inst->z->name, self->inst->type->name);

	return 1;
}

/* Read-only view of a freshly built table. */
static void push_readonly(lua_State *L)
{
	lua_newtable(L);
	lua_createtable(L, 0, 3);
	lua_pushvalue(L, -3);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, l_readonly);
	lua_setfield(L, -2, "__newindex");
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");
	lua_setmetatable(L, -2);
	lua_remove(L, -2);
}

/* Push the per-type instance metatable, building it on first use. */
static void push_type_metatable(lua_State *L, const struct zephlet_lua_type *type)
{
	lua_pushfstring(L, INSTANCE_MT_PREFIX "%s", type->name);
	if (!luaL_newmetatable(L, lua_tostring(L, -1))) {
		lua_remove(L, -2);
		return;
	}
	lua_remove(L, -2);

	/* Member table, shared by every instance of the type. */
	lua_createtable(L, 0, 2 + type->num_methods + type->num_enums);
	lua_pushvalue(L, -2); /* the metatable */
	lua_pushcclosure(L, l_add_policy, 1);
	lua_setfield(L, -2, "add_policy");
	push_prelude_fn(L, "await");
	lua_setfield(L, -2, "await");
	for (uint16_t i = 0U; i < type->num_methods; i++) {
		lua_pushlightuserdata(L, (void *)&type->methods[i]);
		lua_pushvalue(L, -3); /* the metatable */
		lua_pushcclosure(L, l_call, 2);
		lua_setfield(L, -2, type->methods[i].name);
	}
	for (uint16_t i = 0U; i < type->num_enums; i++) {
		const struct zephlet_lua_enum *e = &type->enums[i];

		lua_createtable(L, 0, e->count);
		for (uint16_t v = 0U; v < e->count; v++) {
			lua_pushinteger(L, (lua_Integer)e->values[v].value);
			lua_setfield(L, -2, e->values[v].name);
		}
		push_readonly(L);
		lua_setfield(L, -2, e->name);
	}
	lua_pushcclosure(L, l_instance_index, 1);
	lua_setfield(L, -2, "__index");

	lua_pushcfunction(L, l_readonly);
	lua_setfield(L, -2, "__newindex");
	lua_pushcfunction(L, l_instance_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");
}

struct zephlet_lua_instance *zlet_lua_to_instance(lua_State *L, int idx)
{
	struct instance_ud *self = lua_touserdata(L, idx);
	bool ok = false;

	if ((self != NULL) && lua_getmetatable(L, idx)) {
		if (lua_getfield(L, -1, "__name") == LUA_TSTRING) {
			ok = strncmp(lua_tostring(L, -1), INSTANCE_MT_PREFIX,
				     sizeof(INSTANCE_MT_PREFIX) - 1U) == 0;
		}
		lua_pop(L, 2);
	}

	return ok ? self->inst : NULL;
}

void zlet_lua_push_instance(lua_State *L, struct zephlet_lua_instance *inst)
{
	lua_rawgetp(L, LUA_REGISTRYINDEX, &cache_key);
	if (lua_getfield(L, -1, inst->z->name) != LUA_TNIL) {
		lua_remove(L, -2);
		return;
	}
	lua_pop(L, 1);

	struct instance_ud *self = lua_newuserdatauv(L, sizeof(*self), 0);

	self->inst = inst;
	push_type_metatable(L, inst->type);
	lua_setmetatable(L, -2);

	lua_pushvalue(L, -1);
	lua_setfield(L, -3, inst->z->name);
	lua_remove(L, -2);
}

/* ----- Module ----------------------------------------------------------- */

/* A counter as a Lua integer, which is 32-bit here: it saturates rather than
 * turning negative. */
static void push_counter(lua_State *L, const atomic_t *counter)
{
	unsigned long v = (unsigned long)atomic_get(counter);

	lua_pushinteger(L, (v > (unsigned long)LUA_MAXINTEGER) ? LUA_MAXINTEGER : (lua_Integer)v);
}

static int l_stats(lua_State *L)
{
	size_t n = zlet_lua_instance_count();

	lua_createtable(L, 0, (int)n);
	for (size_t i = 0U; i < n; i++) {
		struct zephlet_lua_instance *inst = zlet_lua_instance_get(i);
		const struct zephlet_lua_stats *s = &inst->stats;

		lua_createtable(L, 0, 7);
		push_counter(L, &s->emitted);
		lua_setfield(L, -2, "emitted");
		push_counter(L, &s->delivered);
		lua_setfield(L, -2, "delivered");
		push_counter(L, &s->staged_drop);
		lua_setfield(L, -2, "staged_drop");
		push_counter(L, &s->fifo_drop);
		lua_setfield(L, -2, "fifo_drop");
		push_counter(L, &s->doorbell_fail);
		lua_setfield(L, -2, "doorbell_fail");
		push_counter(L, &s->handler_err);
		lua_setfield(L, -2, "handler_err");
		push_counter(L, &s->await_drop);
		lua_setfield(L, -2, "await_drop");
		lua_setfield(L, -2, inst->z->name);
	}

	return 1;
}

/* The module's own fields, which shadow an instance of the same name. */
static const char *const module_keys[] = {"stats", "FOREVER", "await_any"};

static int l_module_index(lua_State *L)
{
	const char *key = luaL_checkstring(L, 2);

	if (strcmp(key, "stats") == 0) {
		lua_pushcfunction(L, l_stats);
		return 1;
	}
	if (strcmp(key, "FOREVER") == 0) {
		lua_pushinteger(L, ZEPHLET_LUA_FOREVER);
		return 1;
	}
	if (strcmp(key, "await_any") == 0) {
		push_prelude_fn(L, "await_any");
		return 1;
	}

	/* An instance already named once is cached: skip the section walk. */
	lua_rawgetp(L, LUA_REGISTRYINDEX, &cache_key);
	if (lua_getfield(L, -1, key) != LUA_TNIL) {
		return 1;
	}
	lua_pop(L, 2);

	STRUCT_SECTION_FOREACH(zephlet_lua_instance, inst) {
		if (strcmp(inst->z->name, key) == 0) {
			zlet_lua_push_instance(L, inst);
			return 1;
		}
	}

	return luaL_error(L, "zephlets: no instance named '%s'", key);
}

static int luaopen_zephlets(lua_State *L)
{
	zlet_lua_check_owner(L);

	lua_newtable(L);
	lua_createtable(L, 0, 3);
	lua_pushcfunction(L, l_module_index);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, l_readonly);
	lua_setfield(L, -2, "__newindex");
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");
	lua_setmetatable(L, -2);

	return 1;
}

void zlet_lua_register(lua_State *L)
{
	luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
	lua_pushcfunction(L, luaopen_zephlets);
	lua_setfield(L, -2, "zephlets");
	lua_pop(L, 1);

	/* Created here rather than in luaopen, so the seal and the dispatcher find
	 * them even when the script never requires the module. */
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &handlers_key);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &cache_key);

	zlet_lua_event_register(L);

	/* The module's own fields come first, and codegen never sees instance
	 * names, so say it here when one cannot be reached. */
	STRUCT_SECTION_FOREACH(zephlet_lua_instance, inst) {
		for (size_t i = 0U; i < ARRAY_SIZE(module_keys); i++) {
			if (strcmp(inst->z->name, module_keys[i]) == 0) {
				LOG_WRN("instance '%s' is unreachable from Lua: zephlets.%s is "
					"the module's own",
					inst->z->name, module_keys[i]);
			}
		}
	}
}

void zlet_lua_seal(lua_State *L)
{
	size_t n = zlet_lua_instance_count();

	lua_rawgetp(L, LUA_REGISTRYINDEX, &handlers_key);
	for (size_t i = 0U; i < n; i++) {
		if (lua_rawgeti(L, -1, (lua_Integer)i + 1) == LUA_TTABLE) {
			(void)atomic_or(&zlet_lua_instance_get(i)->flags,
					ZEPHLET_LUA_F_CONNECTED | ZEPHLET_LUA_F_POLICY);
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	atomic_set(&zlet_lua_sealed, 1);
	zlet_lua_fold_preseal_drops();
}
