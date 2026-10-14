/*
 * zephlet_lua_coordinate(): the frontend's entry point.
 *
 * Order, all on the calling thread:
 *   1. Lua state on the frontend's own heap; `zephlets` in package preload.
 *   2. The script's main chunk, synchronously: it connects handlers and may call
 *      commands. zephyr.async is not available yet.
 *   3. Seal: handler lists freeze, and only connected instances keep staging.
 *   4. zephyr.async loop with the pump and dispatcher tasks, plus the function
 *      the chunk returned, if any.
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

BUILD_ASSERT(CONFIG_LUA_ASYNC_MAX_TASKS >= 3,
	     "the Lua frontend needs 2 scheduler tasks, plus 1 for the script's async main");

static uint8_t heap_mem[CONFIG_ZEPHLETS_LUA_HEAP_SIZE] __aligned(8);
static struct sys_heap heap;

ZBUS_MSG_SUBSCRIBER_DEFINE(zlet_lua_sub);

static atomic_t started;

static int traceback(lua_State *L)
{
	const char *msg = lua_tostring(L, 1);

	luaL_traceback(L, L, (msg != NULL) ? msg : "(error object is not a string)", 1);

	return 1;
}

static int run(lua_State *L, const char *buf, size_t len, const char *chunkname)
{
	int tb;
	int ntasks;
	int rc;

	lua_pushcfunction(L, traceback);
	tb = lua_gettop(L);

	if (luaL_loadbufferx(L, buf, len, chunkname, NULL) != LUA_OK) {
		LOG_ERR("%s", lua_tostring(L, -1));
		return -ENOEXEC;
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
	ntasks = lua_gettop(L) - tb + 2;

	zlet_lua_seal(L);

	if (luaL_loadbufferx(L, (const char *)zephlet_lua_prelude_lua_bytecode,
			     zephlet_lua_prelude_lua_bytecode_len, "=zephlets", "b") != LUA_OK) {
		LOG_ERR("prelude: %s", lua_tostring(L, -1));
		return -EFAULT;
	}
	zlet_lua_push_core(L);
	if (lua_pcall(L, 1, 2, tb) != LUA_OK) {
		LOG_ERR("prelude: %s", lua_tostring(L, -1));
		return -EFAULT;
	}
	/* [tb, main?, pump, dispatch]: the script's task, if any, goes last. */
	if (ntasks == 3) {
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

	sys_heap_init(&heap, heap_mem, sizeof(heap_mem));
	L = lua_newstate(lua_zephyr_allocator, &heap, 0U);
	if (L == NULL) {
		return -ENOMEM;
	}
	luaz_openlibs(L);
	zlet_lua_register(L);

	rc = run(L, buf, len, chunkname);
	lua_close(L);

	return rc;
}
