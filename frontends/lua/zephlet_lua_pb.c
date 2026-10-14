/*
 * nanopb struct <-> Lua table, driven by nanopb-textformat descriptors.
 *
 * The text-format descriptors already carry what nanopb's own drops: field
 * names, the exact value type (float vs fixed32, enum vs int32) and the
 * repetition rule (optional vs singular). Each field is located in the struct
 * with nanopb's pb_field_iter, the same way the text-format printer does it.
 *
 * Lua is built with LUA_32BITS, so a Lua integer is 32-bit and a Lua number is
 * a float. A value that does not fit a Lua integer is delivered as a number,
 * which may round; see the frontend README.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include <nanopb_textformat.h>
#include <pb_common.h>

#include <zephyr/sys/util.h>

#include "zephlet_lua_priv.h"

/* Deeper nesting than any hand-written proto; stops a runaway walk. */
#define ZLET_LUA_PB_MAX_DEPTH 8

static bool field_supported(const struct pb_tf_field *f)
{
	if (PB_TF_AT(f->rules) != PB_TF_AT_STATIC) {
		return false;
	}
	if (PB_TF_HT(f->rules) == PB_TF_HT_FIXARRAY) {
		return false;
	}

	return f->vtype != PB_TF_VT_EXTENSION;
}

static bool vtype_is_msg(uint8_t vtype)
{
	return (vtype == PB_TF_VT_MESSAGE) || (vtype == PB_TF_VT_MSG_W_CB);
}

static bool vtype_is_signed(uint8_t vtype)
{
	switch (vtype) {
	case PB_TF_VT_INT32:
	case PB_TF_VT_SINT32:
	case PB_TF_VT_SFIXED32:
	case PB_TF_VT_INT64:
	case PB_TF_VT_SINT64:
	case PB_TF_VT_SFIXED64:
	case PB_TF_VT_ENUM:
		return true;
	default:
		return false;
	}
}

static bool vtype_is_unsigned(uint8_t vtype)
{
	switch (vtype) {
	case PB_TF_VT_UINT32:
	case PB_TF_VT_FIXED32:
	case PB_TF_VT_UINT64:
	case PB_TF_VT_FIXED64:
	case PB_TF_VT_UENUM:
		return true;
	default:
		return false;
	}
}

static bool locate(const struct pb_tf_msg *tf, const void *msg, const struct pb_tf_field *f,
		   pb_field_iter_t *it)
{
	/* pb_field_iter_begin() reports false for a message with no fields. */
	return pb_field_iter_begin(it, tf->desc, (void *)msg) && pb_field_iter_find(it, f->tag);
}

/* ----- C -> Lua --------------------------------------------------------- */

/* strnlen() without relying on the libc exposing it. */
static size_t bounded_strlen(const char *s, size_t max)
{
	size_t n = 0U;

	while ((n < max) && (s[n] != '\0')) {
		n++;
	}

	return n;
}

static int64_t load_signed(const void *src, pb_size_t size)
{
	int64_t v = 0;

	if (size == sizeof(int8_t)) {
		int8_t x;

		memcpy(&x, src, sizeof(x));
		v = x;
	} else if (size == sizeof(int16_t)) {
		int16_t x;

		memcpy(&x, src, sizeof(x));
		v = x;
	} else if (size == sizeof(int32_t)) {
		int32_t x;

		memcpy(&x, src, sizeof(x));
		v = x;
	} else if (size == sizeof(int64_t)) {
		memcpy(&v, src, sizeof(v));
	}

	return v;
}

static uint64_t load_unsigned(const void *src, pb_size_t size)
{
	uint64_t v = 0U;

	if (size == sizeof(uint8_t)) {
		uint8_t x;

		memcpy(&x, src, sizeof(x));
		v = x;
	} else if (size == sizeof(uint16_t)) {
		uint16_t x;

		memcpy(&x, src, sizeof(x));
		v = x;
	} else if (size == sizeof(uint32_t)) {
		uint32_t x;

		memcpy(&x, src, sizeof(x));
		v = x;
	} else if (size == sizeof(uint64_t)) {
		memcpy(&v, src, sizeof(v));
	}

	return v;
}

static void push_signed(lua_State *L, int64_t v)
{
	if ((v >= (int64_t)LUA_MININTEGER) && (v <= (int64_t)LUA_MAXINTEGER)) {
		lua_pushinteger(L, (lua_Integer)v);
	} else {
		lua_pushnumber(L, (lua_Number)v);
	}
}

static void push_unsigned(lua_State *L, uint64_t v)
{
	if (v <= (uint64_t)LUA_MAXINTEGER) {
		lua_pushinteger(L, (lua_Integer)v);
	} else {
		lua_pushnumber(L, (lua_Number)v);
	}
}

static void push_msg_at(lua_State *L, const struct pb_tf_msg *tf, const void *msg, int depth);

static void push_value(lua_State *L, const struct pb_tf_field *f, const void *src, pb_size_t size,
		       int depth)
{
	if (vtype_is_msg(f->vtype)) {
		push_msg_at(L, f->sub, src, depth + 1);
		return;
	}
	if (vtype_is_signed(f->vtype)) {
		push_signed(L, load_signed(src, size));
		return;
	}
	if (vtype_is_unsigned(f->vtype)) {
		push_unsigned(L, load_unsigned(src, size));
		return;
	}

	switch (f->vtype) {
	case PB_TF_VT_BOOL:
		lua_pushboolean(L, *(const bool *)src ? 1 : 0);
		break;
	case PB_TF_VT_FLOAT:
	case PB_TF_VT_DOUBLE:
		/* nanopb maps double to float on targets without a 64-bit double. */
		if (size == sizeof(double)) {
			double d;

			memcpy(&d, src, sizeof(d));
			lua_pushnumber(L, (lua_Number)d);
		} else {
			float x;

			memcpy(&x, src, sizeof(x));
			lua_pushnumber(L, (lua_Number)x);
		}
		break;
	case PB_TF_VT_STRING:
		lua_pushlstring(L, (const char *)src, bounded_strlen((const char *)src, size));
		break;
	case PB_TF_VT_BYTES: {
		const pb_bytes_array_t *b = src;
		size_t cap = (size_t)size - offsetof(pb_bytes_array_t, bytes);

		lua_pushlstring(L, (const char *)b->bytes, MIN((size_t)b->size, cap));
		break;
	}
	case PB_TF_VT_FIXED_LENGTH_BYTES:
		lua_pushlstring(L, (const char *)src, size);
		break;
	default:
		lua_pushnil(L);
		break;
	}
}

static void push_msg_at(lua_State *L, const struct pb_tf_msg *tf, const void *msg, int depth)
{
	if (depth > ZLET_LUA_PB_MAX_DEPTH) {
		luaL_error(L, "zephlets: message nested deeper than %d", ZLET_LUA_PB_MAX_DEPTH);
	}
	luaL_checkstack(L, 4, "zephlets: message");

	lua_createtable(L, 0, (tf != NULL) ? tf->count : 0);
	if (tf == NULL) {
		return;
	}

	for (uint16_t i = 0U; i < tf->count; i++) {
		const struct pb_tf_field *f = &tf->fields[i];
		pb_field_iter_t it;

		if (!field_supported(f) || !locate(tf, msg, f, &it)) {
			continue;
		}

		switch (PB_TF_HT(f->rules)) {
		case PB_TF_HT_REQUIRED:
		case PB_TF_HT_SINGULAR:
			push_value(L, f, it.pData, it.data_size, depth);
			break;
		case PB_TF_HT_OPTIONAL:
			if ((it.pSize == NULL) || !*(const bool *)it.pSize) {
				continue;
			}
			push_value(L, f, it.pData, it.data_size, depth);
			break;
		case PB_TF_HT_ONEOF:
			if ((it.pSize == NULL) ||
			    (*(const pb_size_t *)it.pSize != (pb_size_t)f->tag)) {
				continue;
			}
			push_value(L, f, it.pData, it.data_size, depth);
			break;
		case PB_TF_HT_REPEATED: {
			pb_size_t n = (it.pSize != NULL) ? *(const pb_size_t *)it.pSize : 0U;

			n = MIN(n, it.array_size);
			lua_createtable(L, (int)n, 0);
			for (pb_size_t e = 0U; e < n; e++) {
				push_value(L, f,
					   (const char *)it.pData + ((size_t)e * it.data_size),
					   it.data_size, depth);
				lua_rawseti(L, -2, (lua_Integer)e + 1);
			}
			break;
		}
		default:
			continue;
		}
		lua_setfield(L, -2, f->name);
	}
}

void zlet_lua_push_msg(lua_State *L, const struct pb_tf_msg *tf, const void *msg)
{
	push_msg_at(L, tf, msg, 0);
}

/* ----- Lua -> C --------------------------------------------------------- */

static const struct pb_tf_field *find_field(const struct pb_tf_msg *tf, const char *name)
{
	if (tf == NULL) {
		return NULL;
	}

	for (uint16_t i = 0U; i < tf->count; i++) {
		const struct pb_tf_field *f = &tf->fields[i];

		if ((f->name != NULL) && (strcmp(f->name, name) == 0)) {
			return f;
		}
	}

	return NULL;
}

/* 2^63 and 2^64, exact in any lua_Number: the bounds a float must be checked
 * against before it is cast, since casting one out of range is undefined. */
#define TWO_POW_63 ((lua_Number)9223372036854775808.0)
#define TWO_POW_64 ((lua_Number)18446744073709551616.0)

/* The number argument at idx, raising unless it is one. A Lua integer is only
 * 32-bit, so larger values arrive as floats; an integral one is accepted. */
static lua_Number check_number(lua_State *L, int idx, const char *name)
{
	if (lua_type(L, idx) != LUA_TNUMBER) {
		luaL_error(L, "field '%s': expected integer, got %s", name, luaL_typename(L, idx));
	}

	return lua_tonumber(L, idx);
}

static void non_integral(lua_State *L, const char *name)
{
	luaL_error(L, "field '%s': expected integer, got non-integral number", name);
}

/* A signed integer argument, from -2^63 up to 2^63 - 1. */
static int64_t check_signed(lua_State *L, int idx, const char *name)
{
	lua_Number n = check_number(L, idx, name);
	int64_t v;

	if (lua_isinteger(L, idx)) {
		return (int64_t)lua_tointeger(L, idx);
	}
	if (n != n) {
		non_integral(L, name);
	}
	if (!((n >= -TWO_POW_63) && (n < TWO_POW_63))) {
		luaL_error(L, "field '%s': value out of range", name);
	}
	v = (int64_t)n;
	if ((lua_Number)v != n) {
		non_integral(L, name);
	}

	return v;
}

/* An unsigned integer argument, from 0 up to 2^64 - 1. */
static uint64_t check_unsigned(lua_State *L, int idx, const char *name)
{
	lua_Number n = check_number(L, idx, name);
	uint64_t v;

	if (lua_isinteger(L, idx)) {
		lua_Integer i = lua_tointeger(L, idx);

		if (i < 0) {
			luaL_error(L, "field '%s': value out of range", name);
		}
		return (uint64_t)i;
	}
	if (n != n) {
		non_integral(L, name);
	}
	/* Above -1, a cast truncates to a representable value; 0 > n > -1 is
	 * then caught as non-integral. */
	if (!((n > (lua_Number)-1) && (n < TWO_POW_64))) {
		luaL_error(L, "field '%s': value out of range", name);
	}
	v = (uint64_t)n;
	if ((lua_Number)v != n) {
		non_integral(L, name);
	}

	return v;
}

static void store_signed(lua_State *L, const char *name, void *dst, pb_size_t size, int64_t v)
{
	int64_t lo;
	int64_t hi;

	switch (size) {
	case sizeof(int8_t):
		lo = INT8_MIN;
		hi = INT8_MAX;
		break;
	case sizeof(int16_t):
		lo = INT16_MIN;
		hi = INT16_MAX;
		break;
	case sizeof(int32_t):
		lo = INT32_MIN;
		hi = INT32_MAX;
		break;
	default:
		lo = INT64_MIN;
		hi = INT64_MAX;
		break;
	}
	if ((v < lo) || (v > hi)) {
		luaL_error(L, "field '%s': value out of range", name);
	}

	if (size == sizeof(int8_t)) {
		int8_t x = (int8_t)v;

		memcpy(dst, &x, sizeof(x));
	} else if (size == sizeof(int16_t)) {
		int16_t x = (int16_t)v;

		memcpy(dst, &x, sizeof(x));
	} else if (size == sizeof(int32_t)) {
		int32_t x = (int32_t)v;

		memcpy(dst, &x, sizeof(x));
	} else {
		memcpy(dst, &v, sizeof(v));
	}
}

static void store_unsigned(lua_State *L, const char *name, void *dst, pb_size_t size, uint64_t v)
{
	uint64_t hi;

	switch (size) {
	case sizeof(uint8_t):
		hi = UINT8_MAX;
		break;
	case sizeof(uint16_t):
		hi = UINT16_MAX;
		break;
	case sizeof(uint32_t):
		hi = UINT32_MAX;
		break;
	default:
		hi = UINT64_MAX;
		break;
	}
	if (v > hi) {
		luaL_error(L, "field '%s': value out of range", name);
	}

	if (size == sizeof(uint8_t)) {
		uint8_t x = (uint8_t)v;

		memcpy(dst, &x, sizeof(x));
	} else if (size == sizeof(uint16_t)) {
		uint16_t x = (uint16_t)v;

		memcpy(dst, &x, sizeof(x));
	} else if (size == sizeof(uint32_t)) {
		uint32_t x = (uint32_t)v;

		memcpy(dst, &x, sizeof(x));
	} else {
		memcpy(dst, &v, sizeof(v));
	}
}

static const char *check_string(lua_State *L, int idx, const char *name, size_t *len)
{
	if (lua_type(L, idx) != LUA_TSTRING) {
		luaL_error(L, "field '%s': expected string, got %s", name, luaL_typename(L, idx));
	}

	return lua_tolstring(L, idx, len);
}

static void read_msg_at(lua_State *L, int idx, const struct pb_tf_msg *tf, void *msg, int depth);

static void read_value(lua_State *L, int idx, const struct pb_tf_field *f, void *dst,
		       pb_size_t size, int depth)
{
	size_t len;
	const char *s;

	if (vtype_is_msg(f->vtype)) {
		if (lua_type(L, idx) != LUA_TTABLE) {
			luaL_error(L, "field '%s': expected table, got %s", f->name,
				   luaL_typename(L, idx));
		}
		read_msg_at(L, idx, f->sub, dst, depth + 1);
		return;
	}
	if (vtype_is_signed(f->vtype)) {
		store_signed(L, f->name, dst, size, check_signed(L, idx, f->name));
		return;
	}
	if (vtype_is_unsigned(f->vtype)) {
		store_unsigned(L, f->name, dst, size, check_unsigned(L, idx, f->name));
		return;
	}

	switch (f->vtype) {
	case PB_TF_VT_BOOL:
		if (lua_type(L, idx) != LUA_TBOOLEAN) {
			luaL_error(L, "field '%s': expected boolean, got %s", f->name,
				   luaL_typename(L, idx));
		}
		*(bool *)dst = lua_toboolean(L, idx) != 0;
		break;
	case PB_TF_VT_FLOAT:
	case PB_TF_VT_DOUBLE: {
		if (lua_type(L, idx) != LUA_TNUMBER) {
			luaL_error(L, "field '%s': expected number, got %s", f->name,
				   luaL_typename(L, idx));
		}
		lua_Number n = lua_tonumber(L, idx);

		if (size == sizeof(double)) {
			double d = (double)n;

			memcpy(dst, &d, sizeof(d));
		} else {
			float x = (float)n;

			memcpy(dst, &x, sizeof(x));
		}
		break;
	}
	case PB_TF_VT_STRING:
		s = check_string(L, idx, f->name, &len);
		/* One byte stays for the terminator nanopb expects. */
		if (len >= size) {
			luaL_error(L, "field '%s': string longer than %d bytes", f->name,
				   (int)size - 1);
		}
		memcpy(dst, s, len);
		((char *)dst)[len] = '\0';
		break;
	case PB_TF_VT_BYTES: {
		pb_bytes_array_t *b = dst;
		size_t cap = (size_t)size - offsetof(pb_bytes_array_t, bytes);

		s = check_string(L, idx, f->name, &len);
		if (len > cap) {
			luaL_error(L, "field '%s': bytes longer than %d", f->name, (int)cap);
		}
		memcpy(b->bytes, s, len);
		b->size = (pb_size_t)len;
		break;
	}
	case PB_TF_VT_FIXED_LENGTH_BYTES:
		s = check_string(L, idx, f->name, &len);
		if (len != size) {
			luaL_error(L, "field '%s': expected exactly %d bytes", f->name, (int)size);
		}
		memcpy(dst, s, len);
		break;
	default:
		luaL_error(L, "field '%s' is not supported", f->name);
		break;
	}
}

static void read_repeated(lua_State *L, int idx, const struct pb_tf_field *f, pb_field_iter_t *it,
			  int depth)
{
	lua_Integer n;

	if (lua_type(L, idx) != LUA_TTABLE) {
		luaL_error(L, "field '%s': expected array, got %s", f->name, luaL_typename(L, idx));
	}
	n = (lua_Integer)lua_rawlen(L, idx);
	if (n > (lua_Integer)it->array_size) {
		luaL_error(L, "field '%s': more than %d elements", f->name, (int)it->array_size);
	}

	for (lua_Integer e = 1; e <= n; e++) {
		lua_rawgeti(L, idx, e);
		read_value(L, lua_gettop(L), f,
			   (char *)it->pData + ((size_t)(e - 1) * it->data_size), it->data_size,
			   depth);
		lua_pop(L, 1);
	}
	*(pb_size_t *)it->pSize = (pb_size_t)n;
}

static void read_msg_at(lua_State *L, int idx, const struct pb_tf_msg *tf, void *msg, int depth)
{
	idx = lua_absindex(L, idx);
	if (depth > ZLET_LUA_PB_MAX_DEPTH) {
		luaL_error(L, "zephlets: message nested deeper than %d", ZLET_LUA_PB_MAX_DEPTH);
	}
	luaL_checkstack(L, 4, "zephlets: message");

	lua_pushnil(L);
	while (lua_next(L, idx) != 0) {
		const struct pb_tf_field *f;
		const char *name;
		pb_field_iter_t it;
		int vidx = lua_gettop(L);

		if (lua_type(L, -2) != LUA_TSTRING) {
			luaL_error(L, "zephlets: message field names must be strings");
		}
		name = lua_tostring(L, -2);
		f = find_field(tf, name);
		if (f == NULL) {
			luaL_error(L, "unknown field '%s'", name);
		}
		if (!field_supported(f) || !locate(tf, msg, f, &it)) {
			luaL_error(L, "field '%s' is not supported", name);
		}

		switch (PB_TF_HT(f->rules)) {
		case PB_TF_HT_REQUIRED:
		case PB_TF_HT_SINGULAR:
			read_value(L, vidx, f, it.pData, it.data_size, depth);
			break;
		case PB_TF_HT_OPTIONAL:
			read_value(L, vidx, f, it.pData, it.data_size, depth);
			*(bool *)it.pSize = true;
			break;
		case PB_TF_HT_ONEOF: {
			pb_size_t *which = it.pSize;

			if ((*which != 0U) && (*which != (pb_size_t)f->tag)) {
				luaL_error(L,
					   "field '%s': another member of its oneof is already set",
					   name);
			}
			memset(it.pData, 0, it.data_size);
			read_value(L, vidx, f, it.pData, it.data_size, depth);
			*which = (pb_size_t)f->tag;
			break;
		}
		case PB_TF_HT_REPEATED:
			read_repeated(L, vidx, f, &it, depth);
			break;
		default:
			luaL_error(L, "field '%s' is not supported", name);
			break;
		}
		lua_pop(L, 1);
	}
}

void zlet_lua_read_msg(lua_State *L, int idx, const struct pb_tf_msg *tf, void *msg)
{
	read_msg_at(L, idx, tf, msg, 0);
}
