/*
 * Copyright (c) 2026 Rodrigo Peixoto
 * SPDX-License-Identifier: Apache-2.0
 *
 * typelab Zephlet Module
 */

#include "zlet_typelab.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zlet_typelab, CONFIG_ZEPHLET_TYPELAB_LOG_LEVEL);

/*
 * typelab has no hardware and no domain behavior — its only job is
 * `Typelab.Config`. start/stop just flip the framework-standard
 * lifecycle flags; config/get_config copy the request through
 * unmodified, since every field is a scalar with no invalid
 * combination to reject.
 */

int typelab_start_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	struct typelab_data *d = z->data;

	if (!d->is_ready) {
		return -ENODEV;
	}
	if (d->is_running) {
		return -EALREADY;
	}

	d->is_running = true;

	if (resp != NULL) {
		resp->is_running = true;
		resp->is_ready = true;
	}
	return 0;
}

int typelab_stop_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	struct typelab_data *d = z->data;

	if (!d->is_running) {
		return -EALREADY;
	}

	d->is_running = false;

	if (resp != NULL) {
		resp->is_running = false;
		resp->is_ready = d->is_ready;
	}
	return 0;
}

int typelab_get_status_impl(const struct zephlet *z, struct lifecycle_status *resp)
{
	struct typelab_data *d = z->data;

	if (resp != NULL) {
		resp->is_running = d->is_running;
		resp->is_ready = d->is_ready;
	}
	return 0;
}

int typelab_config_impl(const struct zephlet *z, const struct typelab_config *req,
			struct typelab_config *resp)
{
	struct typelab_config *cfg = z->config;

	*cfg = *req;

	if (resp != NULL) {
		*resp = *cfg;
	}
	return 0;
}

int typelab_get_config_impl(const struct zephlet *z, struct typelab_config *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		*resp = *cfg;
	}
	return 0;
}

/*
 * Per-type set_X/get_X pairs — one per field of `struct typelab_config`
 * above, so each nanopb scalar type can be exercised over shell in
 * isolation (`zlet typelab_bench set_X <value>` / `get_X`) instead of
 * only through the one big `config` call. All read/write the *same*
 * `cfg` as config_impl/get_config_impl — there is exactly one
 * `struct typelab_config` per instance.
 */

int typelab_set_uint32_impl(const struct zephlet *z, const struct typelab_uint32_value *req,
			    struct typelab_uint32_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_uint32 = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_uint32;
	}
	return 0;
}

int typelab_get_uint32_impl(const struct zephlet *z, struct typelab_uint32_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_uint32;
	}
	return 0;
}

int typelab_set_uint64_impl(const struct zephlet *z, const struct typelab_uint64_value *req,
			    struct typelab_uint64_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_uint64 = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_uint64;
	}
	return 0;
}

int typelab_get_uint64_impl(const struct zephlet *z, struct typelab_uint64_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_uint64;
	}
	return 0;
}

int typelab_set_fixed32_impl(const struct zephlet *z, const struct typelab_fixed32_value *req,
			     struct typelab_fixed32_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_fixed32 = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_fixed32;
	}
	return 0;
}

int typelab_get_fixed32_impl(const struct zephlet *z, struct typelab_fixed32_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_fixed32;
	}
	return 0;
}

int typelab_set_fixed64_impl(const struct zephlet *z, const struct typelab_fixed64_value *req,
			     struct typelab_fixed64_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_fixed64 = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_fixed64;
	}
	return 0;
}

int typelab_get_fixed64_impl(const struct zephlet *z, struct typelab_fixed64_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_fixed64;
	}
	return 0;
}

int typelab_set_uenum_impl(const struct zephlet *z, const struct typelab_uenum_value *req,
			   struct typelab_uenum_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_uenum = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_uenum;
	}
	return 0;
}

int typelab_get_uenum_impl(const struct zephlet *z, struct typelab_uenum_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_uenum;
	}
	return 0;
}

int typelab_set_int32_impl(const struct zephlet *z, const struct typelab_int32_value *req,
			   struct typelab_int32_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_int32 = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_int32;
	}
	return 0;
}

int typelab_get_int32_impl(const struct zephlet *z, struct typelab_int32_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_int32;
	}
	return 0;
}

int typelab_set_int64_impl(const struct zephlet *z, const struct typelab_int64_value *req,
			   struct typelab_int64_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_int64 = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_int64;
	}
	return 0;
}

int typelab_get_int64_impl(const struct zephlet *z, struct typelab_int64_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_int64;
	}
	return 0;
}

int typelab_set_sint32_impl(const struct zephlet *z, const struct typelab_sint32_value *req,
			    struct typelab_sint32_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_sint32 = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_sint32;
	}
	return 0;
}

int typelab_get_sint32_impl(const struct zephlet *z, struct typelab_sint32_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_sint32;
	}
	return 0;
}

int typelab_set_sint64_impl(const struct zephlet *z, const struct typelab_sint64_value *req,
			    struct typelab_sint64_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_sint64 = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_sint64;
	}
	return 0;
}

int typelab_get_sint64_impl(const struct zephlet *z, struct typelab_sint64_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_sint64;
	}
	return 0;
}

int typelab_set_sfixed32_impl(const struct zephlet *z, const struct typelab_sfixed32_value *req,
			      struct typelab_sfixed32_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_sfixed32 = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_sfixed32;
	}
	return 0;
}

int typelab_get_sfixed32_impl(const struct zephlet *z, struct typelab_sfixed32_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_sfixed32;
	}
	return 0;
}

int typelab_set_sfixed64_impl(const struct zephlet *z, const struct typelab_sfixed64_value *req,
			      struct typelab_sfixed64_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_sfixed64 = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_sfixed64;
	}
	return 0;
}

int typelab_get_sfixed64_impl(const struct zephlet *z, struct typelab_sfixed64_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_sfixed64;
	}
	return 0;
}

int typelab_set_enum_impl(const struct zephlet *z, const struct typelab_enum_value *req,
			  struct typelab_enum_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_enum = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_enum;
	}
	return 0;
}

int typelab_get_enum_impl(const struct zephlet *z, struct typelab_enum_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_enum;
	}
	return 0;
}

int typelab_set_float_impl(const struct zephlet *z, const struct typelab_float_value *req,
			   struct typelab_float_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_float = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_float;
	}
	return 0;
}

int typelab_get_float_impl(const struct zephlet *z, struct typelab_float_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_float;
	}
	return 0;
}

int typelab_set_double_impl(const struct zephlet *z, const struct typelab_double_value *req,
			    struct typelab_double_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_double = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_double;
	}
	return 0;
}

int typelab_get_double_impl(const struct zephlet *z, struct typelab_double_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_double;
	}
	return 0;
}

int typelab_set_bool_impl(const struct zephlet *z, const struct typelab_bool_value *req,
			  struct typelab_bool_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_bool = req->value;
	if (resp != NULL) {
		resp->value = cfg->f_bool;
	}
	return 0;
}

int typelab_get_bool_impl(const struct zephlet *z, struct typelab_bool_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value = cfg->f_bool;
	}
	return 0;
}

int typelab_set_bytes_impl(const struct zephlet *z, const struct typelab_bytes_value *req,
			   struct typelab_bytes_value *resp)
{
	struct typelab_config *cfg = z->config;

	cfg->f_bytes.size = req->value.size;
	memcpy(cfg->f_bytes.bytes, req->value.bytes, req->value.size);
	if (resp != NULL) {
		resp->value.size = cfg->f_bytes.size;
		memcpy(resp->value.bytes, cfg->f_bytes.bytes, cfg->f_bytes.size);
	}
	return 0;
}

int typelab_get_bytes_impl(const struct zephlet *z, struct typelab_bytes_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		resp->value.size = cfg->f_bytes.size;
		memcpy(resp->value.bytes, cfg->f_bytes.bytes, cfg->f_bytes.size);
	}
	return 0;
}

int typelab_set_fixed_bytes_impl(const struct zephlet *z,
				 const struct typelab_fixed_bytes_value *req,
				 struct typelab_fixed_bytes_value *resp)
{
	struct typelab_config *cfg = z->config;

	memcpy(cfg->f_fixed_bytes, req->value, sizeof(cfg->f_fixed_bytes));
	if (resp != NULL) {
		memcpy(resp->value, cfg->f_fixed_bytes, sizeof(cfg->f_fixed_bytes));
	}
	return 0;
}

int typelab_get_fixed_bytes_impl(const struct zephlet *z, struct typelab_fixed_bytes_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		memcpy(resp->value, cfg->f_fixed_bytes, sizeof(cfg->f_fixed_bytes));
	}
	return 0;
}

int typelab_set_string_impl(const struct zephlet *z, const struct typelab_string_value *req,
			    struct typelab_string_value *resp)
{
	struct typelab_config *cfg = z->config;

	memcpy(cfg->f_string, req->value, sizeof(cfg->f_string));
	if (resp != NULL) {
		memcpy(resp->value, cfg->f_string, sizeof(cfg->f_string));
	}
	return 0;
}

int typelab_get_string_impl(const struct zephlet *z, struct typelab_string_value *resp)
{
	struct typelab_config *cfg = z->config;

	if (resp != NULL) {
		memcpy(resp->value, cfg->f_string, sizeof(cfg->f_string));
	}
	return 0;
}

int typelab_init_fn(const struct zephlet *z)
{
	struct typelab_data *d = z->data;

	d->is_running = false;
	d->is_ready = true;

	LOG_INF("%s: init", z->name);
	return 0;
}
