/*
 * zephlet_lua_coordinate(): the frontend's entry point.
 *
 * Order, all on the calling thread:
 *   1. Lua state on the frontend's own heap; `zephlets` in package preload.
 *   2. The prelude, which defines the internal tasks and `await`.
 *   3. The script's main chunk, synchronously: it adds policies and may call
 *      commands. zephyr.async is not available yet.
 *   4. Seal: policies freeze. An instance with none stops staging until a
 *      task awaits it.
 *   5. zephyr.async loop with the dispatcher and pump tasks, plus the function
 *      the chunk returned, if any. Its tasks may await instances' events.
 *
 * Zephlet instances are initialised before any of this, by the zephlet
 * SYS_INIT walker, and their events are staged from boot.
 */

#include <errno.h>

#include <lauxlib.h>
#include <lua.h>

#include <luaz_async.h>
#include <luaz_utils.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/zbus/zbus.h>

#include "zephlet_lua_priv.h"
/* zephlet_lua_prelude.lua, precompiled by luaz_add_bytecode_file(). */
#include "zephlet_lua_prelude_lua_bytecode.h"

LOG_MODULE_REGISTER(zlet_lua, CONFIG_ZEPHLET_LOG_LEVEL);

/* The dispatcher, and the pump unless it is configured out. */
#define INTERNAL_TASKS (IS_ENABLED(CONFIG_ZEPHLETS_LUA_PUMP) ? 2 : 1)

BUILD_ASSERT(CONFIG_LUA_ASYNC_MAX_TASKS >= INTERNAL_TASKS + 1,
	     "the Lua frontend needs its own scheduler tasks, plus 1 reserved for the script's "
	     "async main: a policies-only script pays for it too, as the build cannot see what "
	     "the chunk returns");

/* A small-only sys_heap fails its init assert past this size, at boot. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_SYS_HEAP_SMALL_ONLY) || CONFIG_ZEPHLETS_LUA_HEAP_SIZE <= 262136,
	     "CONFIG_ZEPHLETS_LUA_HEAP_SIZE exceeds what CONFIG_SYS_HEAP_SMALL_ONLY supports "
	     "(262136 bytes): lower it, or select CONFIG_SYS_HEAP_AUTO");

K_HEAP_DEFINE(heap, CONFIG_ZEPHLETS_LUA_HEAP_SIZE);

ZBUS_MSG_SUBSCRIBER_DEFINE(zlet_lua_sub);

static atomic_t started;

const char zlet_lua_prelude_key;

static int traceback(lua_State *L)
{
	const char *msg = lua_tostring(L, 1);

	luaL_traceback(L, L, (msg != NULL) ? msg : "(error object is not a string)", 1);

	return 1;
}

/* Run the prelude, and keep the table it returns for the module and run(). */
static int load_prelude(lua_State *L, int tb)
{
	int rc;

	rc = luaL_loadbufferx(L, (const char *)zephlet_lua_prelude_lua_bytecode,
			      zephlet_lua_prelude_lua_bytecode_len, "=zephlets", "b");
	if (rc != LUA_OK) {
		LOG_ERR("prelude: %s", lua_tostring(L, -1));
		return (rc == LUA_ERRMEM) ? -ENOMEM : -EFAULT;
	}
	zlet_lua_push_core(L);
	rc = lua_pcall(L, 1, 1, tb);
	if (rc != LUA_OK) {
		LOG_ERR("prelude: %s", lua_tostring(L, -1));
		return (rc == LUA_ERRMEM) ? -ENOMEM : -EFAULT;
	}
	lua_rawsetp(L, LUA_REGISTRYINDEX, &zlet_lua_prelude_key);

	return 0;
}

static int run(lua_State *L, const char *buf, size_t len, const char *chunkname)
{
	int tb;
	int ntasks;
	bool has_main;
	int rc;

	lua_pushcfunction(L, traceback);
	tb = lua_gettop(L);

	rc = load_prelude(L, tb);
	if (rc != 0) {
		return rc;
	}

	rc = luaL_loadbufferx(L, buf, len, chunkname, NULL);
	if (rc != LUA_OK) {
		LOG_ERR("%s", lua_tostring(L, -1));
		return (rc == LUA_ERRMEM) ? -ENOMEM : -ENOEXEC;
	}
	rc = lua_pcall(L, 0, 1, tb);
	if (rc != LUA_OK) {
		LOG_ERR("%s", lua_tostring(L, -1));
		return (rc == LUA_ERRMEM) ? -ENOMEM : -EINVAL;
	}
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
	} else if (!lua_isfunction(L, -1)) {
		LOG_ERR("%s: the main chunk must return nil or a function, not a %s", chunkname,
			luaL_typename(L, -1));
		return -EINVAL;
	}
	has_main = lua_gettop(L) > tb;
	ntasks = (has_main ? 1 : 0) + INTERNAL_TASKS;

	zlet_lua_seal(L);

	lua_rawgetp(L, LUA_REGISTRYINDEX, &zlet_lua_prelude_key);
	if (IS_ENABLED(CONFIG_ZEPHLETS_LUA_PUMP)) {
		lua_getfield(L, -1, "pump");
		lua_insert(L, -2);
	}
	lua_getfield(L, -1, "dispatch");
	lua_remove(L, -2);
	/* [tb, main?, pump?, dispatch]: the script's task, if any, goes last. */
	if (has_main) {
		lua_rotate(L, tb + 1, -1);
	}

	rc = luaz_async_run(L, &zlet_lua_sub, ntasks);

	return (rc == 0) ? 0 : -EFAULT;
}

struct start_args {
	const char *buf;
	size_t len;
	const char *chunkname;
	int rc;
};

/*
 * Everything after lua_newstate(), under the protected call
 * zephlet_lua_coordinate() makes: the libraries, the module and run()'s own
 * bookkeeping allocate outside any pcall of their own, and on a heap too
 * small for them an unprotected error would abort.
 */
static int start(lua_State *L)
{
	struct start_args *args = lua_touserdata(L, 1);

	lua_settop(L, 0);
	luaz_openlibs(L);
	zlet_lua_register(L);
	args->rc = run(L, args->buf, args->len, args->chunkname);

	return 0;
}

int zephlet_lua_coordinate(const char *buf, size_t len, const char *chunkname)
{
	struct start_args args = {.buf = buf, .len = len, .chunkname = chunkname};
	lua_State *L;
	int rc;

	if (!atomic_cas(&started, 0, 1)) {
		return -EALREADY;
	}
	zlet_lua_owner = k_current_get();

	L = lua_newstate(lua_zephyr_allocator, &heap.heap, 0U);
	if (L == NULL) {
		/* As on every other way out: nothing will empty staging. */
		zlet_lua_shutdown();
		return -ENOMEM;
	}
	(void)lua_gc(L, LUA_GCPARAM, LUA_GCPPAUSE, CONFIG_ZEPHLETS_LUA_GC_PAUSE);

	lua_pushcfunction(L, start);
	lua_pushlightuserdata(L, &args);
	rc = lua_pcall(L, 1, 0, 0);
	if (rc != LUA_OK) {
		LOG_ERR("start: %s", lua_tostring(L, -1));
		rc = (rc == LUA_ERRMEM) ? -ENOMEM : -EFAULT;
	} else {
		rc = args.rc;
	}
	/* Whatever stopped it, nothing will empty staging again. */
	zlet_lua_shutdown();
	lua_close(L);

	return rc;
}
