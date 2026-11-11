/*
 * Composition through declared dependencies: `climate` requires one
 * `temperature` and two `fan` slots (intake, exhaust); each controller
 * instance is bound to its own providers, and both share one exhaust fan.
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include "zlet_climate.h"
#include "zlet_fan.h"
#include "zlet_temperature.h"

#define T K_MSEC(100)

/* ----- Init-order recorder ---------------------------------------------- */

static const struct zephlet *init_seq[16];
static size_t init_len;

static void record(const struct zephlet *z)
{
	if (init_len < ARRAY_SIZE(init_seq)) {
		init_seq[init_len++] = z;
	}
}

static size_t init_pos(const struct zephlet *z)
{
	for (size_t i = 0; i < init_len; i++) {
		if (init_seq[i] == z) {
			return i;
		}
	}
	return SIZE_MAX;
}

static int sensor_init(const struct zephlet *z)
{
	record(z);
	return temperature_init_fn(z);
}

static int broken_sensor_init(const struct zephlet *z)
{
	record(z);
	return -EIO;
}

static int fan_init(const struct zephlet *z)
{
	record(z);
	return fan_init_fn(z);
}

static int controller_init(const struct zephlet *z)
{
	record(z);
	return climate_init_fn(z);
}

/* An init_fn that fails with its own error code. */
static int eloop_fan_init(const struct zephlet *z)
{
	record(z);
	return -ELOOP;
}

/* What room_probe's init_fn saw when it called its provider and itself. */
static struct {
	int set_rc;
	int get_rc;
	uint32_t percent;
	uint32_t sets;
	int self_rc;
	bool self_ready;
} probe_at_init = {.set_rc = 1, .get_rc = 1, .percent = 0x12345678, .self_rc = 1};

/* A consumer that calls its provider, and its own RPC, from its init_fn. */
static int probe_init(const struct zephlet *z)
{
	const struct climate_dependencies *deps = z->dependencies;
	struct fan_speed speed = {.percent = 0x12345678};

	record(z);
	probe_at_init.set_rc = fan_set_speed(deps->intake, &(struct fan_speed){.percent = 7}, T);
	probe_at_init.get_rc = fan_get_speed(deps->intake, &speed, T);
	probe_at_init.percent = speed.percent;
	probe_at_init.sets = ((const struct fan_data *)deps->intake->data)->sets;

	int err = climate_init_fn(z);
	struct lifecycle_status st = {0};

	probe_at_init.self_rc = climate_get_status(z, &st, T);
	probe_at_init.self_ready = st.is_ready;
	return err;
}

/* ----- Providers: plain ZEPHLET_NEW, unchanged ---------------------------- */

static struct temperature_config sensor_a_cfg;
static struct temperature_data sensor_a_data;
ZEPHLET_NEW(temperature, sensor_a, &sensor_a_cfg, &sensor_a_data, sensor_init);

static struct temperature_config sensor_b_cfg = {.offset_c = 1};
static struct temperature_data sensor_b_data;
ZEPHLET_NEW(temperature, sensor_b, &sensor_b_cfg, &sensor_b_data, sensor_init);

static struct temperature_config sensor_broken_cfg;
static struct temperature_data sensor_broken_data;
ZEPHLET_NEW(temperature, sensor_broken, &sensor_broken_cfg, &sensor_broken_data,
	    broken_sensor_init);

static struct fan_config fan_a_cfg = {.max_percent = 100};
static struct fan_data fan_a_data;
ZEPHLET_NEW(fan, fan_a, &fan_a_cfg, &fan_a_data, fan_init);

/* The last sub-priority of its rank: still before every consumer, whose
 * type ranks above. */
static struct fan_config fan_b_cfg = {.max_percent = 100};
static struct fan_data fan_b_data;
ZEPHLET_NEW_PRIO(fan, fan_b, &fan_b_cfg, &fan_b_data, fan_init, 9);

static struct fan_config fan_shared_cfg = {.max_percent = 80};
static struct fan_data fan_shared_data;
ZEPHLET_NEW(fan, fan_shared, &fan_shared_cfg, &fan_shared_data, fan_init);

static struct fan_config fan_probe_cfg = {.max_percent = 100};
static struct fan_data fan_probe_data;
ZEPHLET_NEW(fan, fan_probe, &fan_probe_cfg, &fan_probe_data, fan_init);

static struct fan_config fan_eloop_cfg = {.max_percent = 100};
static struct fan_data fan_eloop_data;
ZEPHLET_NEW(fan, fan_eloop, &fan_eloop_cfg, &fan_eloop_data, eloop_fan_init);

/* ----- Consumers ---------------------------------------------------------- */

static const struct climate_dependencies room_a_deps = {
	.temperature = ZEPHLET_DEP(temperature, sensor_a),
	.intake = ZEPHLET_DEP(fan, fan_a),
	.exhaust = ZEPHLET_DEP(fan, fan_shared),
};
static struct climate_config room_a_cfg = {.setpoint_c = 20};
static struct climate_data room_a_data;
/* A lower sub-priority than fan_b, yet after it: the type rank wins. */
ZEPHLET_NEW_PRIO_WITH_DEPS(climate, room_a, &room_a_cfg, &room_a_data, &room_a_deps,
			   controller_init, 0);

static const struct climate_dependencies room_b_deps = {
	.temperature = ZEPHLET_DEP(temperature, sensor_b),
	.intake = ZEPHLET_DEP(fan, fan_b),
	.exhaust = ZEPHLET_DEP(fan, fan_shared),
};
static struct climate_config room_b_cfg = {.setpoint_c = 25};
static struct climate_data room_b_data;
ZEPHLET_NEW_WITH_DEPS(climate, room_b, &room_b_cfg, &room_b_data, &room_b_deps, controller_init);

/* A provider whose init_fn fails. */
static const struct climate_dependencies room_broken_deps = {
	.temperature = ZEPHLET_DEP(temperature, sensor_broken),
	.intake = ZEPHLET_DEP(fan, fan_a),
	.exhaust = ZEPHLET_DEP(fan, fan_shared),
};
static struct climate_config room_broken_cfg;
static struct climate_data room_broken_data;
ZEPHLET_NEW_WITH_DEPS(climate, room_broken, &room_broken_cfg, &room_broken_data, &room_broken_deps,
		      controller_init);

/* A slot left unbound. */
static const struct climate_dependencies room_unbound_deps = {
	.temperature = ZEPHLET_DEP(temperature, sensor_a),
	.intake = ZEPHLET_DEP(fan, fan_a),
};
static struct climate_config room_unbound_cfg;
static struct climate_data room_unbound_data;
ZEPHLET_NEW_WITH_DEPS(climate, room_unbound, &room_unbound_cfg, &room_unbound_data,
		      &room_unbound_deps, controller_init);

/* A slot bound to an instance of another type. ZEPHLET_DEP would not build;
 * a plain & does, and reaches zephlet_init_one()'s own check. */
static const struct climate_dependencies room_mistyped_deps = {
	.temperature = ZEPHLET_DEP(temperature, sensor_a),
	.intake = &sensor_b,
	.exhaust = ZEPHLET_DEP(fan, fan_shared),
};
static struct climate_config room_mistyped_cfg;
static struct climate_data room_mistyped_data;
ZEPHLET_NEW_WITH_DEPS(climate, room_mistyped, &room_mistyped_cfg, &room_mistyped_data,
		      &room_mistyped_deps, controller_init);

/* Calls fan_probe from its init_fn. */
static const struct climate_dependencies room_probe_deps = {
	.temperature = ZEPHLET_DEP(temperature, sensor_a),
	.intake = ZEPHLET_DEP(fan, fan_probe),
	.exhaust = ZEPHLET_DEP(fan, fan_probe),
};
static struct climate_config room_probe_cfg;
static struct climate_data room_probe_data;
ZEPHLET_NEW_WITH_DEPS(climate, room_probe, &room_probe_cfg, &room_probe_data, &room_probe_deps,
		      probe_init);

/* Bound to a provider whose init_fn fails. */
static const struct climate_dependencies room_eloop_deps = {
	.temperature = ZEPHLET_DEP(temperature, sensor_a),
	.intake = ZEPHLET_DEP(fan, fan_eloop),
	.exhaust = ZEPHLET_DEP(fan, fan_shared),
};
static struct climate_config room_eloop_cfg;
static struct climate_data room_eloop_data;
ZEPHLET_NEW_WITH_DEPS(climate, room_eloop, &room_eloop_cfg, &room_eloop_data, &room_eloop_deps,
		      controller_init);

/* Auto-allocated config and data (setpoint 0). */
static const struct climate_dependencies auto_deps = {
	.temperature = ZEPHLET_DEP(temperature, sensor_b),
	.intake = ZEPHLET_DEP(fan, fan_b),
	.exhaust = ZEPHLET_DEP(fan, fan_b),
};
ZEPHLET_INST_DEFINE_WITH_DEPS(climate, _auto, &auto_deps, controller_init);

/* ----- Tests -------------------------------------------------------------- */

ZTEST(deps, test_providers_initialize_before_consumers)
{
	zassert_equal(zephlet_init_result(&room_a), 0);
	zassert_equal(zephlet_init_result(&room_b), 0);

	zassert_true(init_pos(&sensor_a) < init_pos(&room_a));
	zassert_true(init_pos(&fan_a) < init_pos(&room_a));
	zassert_true(init_pos(&fan_shared) < init_pos(&room_a));
	zassert_true(init_pos(&sensor_b) < init_pos(&room_b));
	zassert_true(init_pos(&fan_b) < init_pos(&room_b));
}

ZTEST(deps, test_init_order_is_rank_then_sub_priority)
{
	/*
	 * Rank 0 (temperature, fan), then rank 1 (climate). Within rank 0,
	 * sub-priority 0 before fan_b's 9; equal ones in an unspecified order.
	 * Skipped instances never run.
	 */
	const struct zephlet *rank0[] = {
		&fan_a, &fan_eloop, &fan_probe, &fan_shared, &sensor_a, &sensor_b, &sensor_broken,
	};
	const struct zephlet *rank1[] = {&room_a, &room_b, &room_probe, &climate_auto};

	zassert_equal(init_len, ARRAY_SIZE(rank0) + 1 + ARRAY_SIZE(rank1));
	for (size_t i = 0; i < ARRAY_SIZE(rank0); i++) {
		zassert_true(init_pos(rank0[i]) < init_pos(&fan_b), "%s", rank0[i]->name);
	}
	for (size_t i = 0; i < ARRAY_SIZE(rank1); i++) {
		zassert_true(init_pos(&fan_b) < init_pos(rank1[i]), "%s", rank1[i]->name);
	}
}

/* App SYS_INIT entries interleave with the instances' by priority. */
static int seen_before = 1;
static int seen_after = 1;

static int app_before_zephlets(void)
{
	seen_before = zephlet_init_result(&fan_a);
	return 0;
}

static int app_after_zephlets(void)
{
	seen_after = zephlet_init_result(&room_b);
	return 0;
}

BUILD_ASSERT(9 < CONFIG_ZEPHLET_INIT_PRIORITY && 99 > CONFIG_ZEPHLET_INIT_PRIORITY + 1);
SYS_INIT(app_before_zephlets, APPLICATION, 9);
SYS_INIT(app_after_zephlets, APPLICATION, 99);

ZTEST(deps, test_instances_are_ordinary_sys_init_entries)
{
	zassert_equal(seen_before, -EAGAIN, "a lower priority runs first");
	zassert_equal(seen_after, 0, "a higher priority runs after every rank");
}

ZTEST(deps, test_instance_holds_the_binding)
{
	const struct climate_dependencies *b = room_b.dependencies;

	zassert_equal_ptr(room_a.dependencies, &room_a_deps);
	zassert_equal_ptr(b->exhaust, &fan_shared);
	zassert_is_null(fan_a.dependencies, "a provider without requires has no record");
}

ZTEST(deps, test_regulate_calls_bound_providers)
{
	struct climate_decision decision;
	struct temperature_reading t = {.celsius = 23};
	struct fan_speed speed;

	zassert_ok(temperature_set(&sensor_a, &t, T));
	t.celsius = 10;
	zassert_ok(temperature_set(&sensor_b, &t, T));

	zassert_ok(climate_regulate(&room_a, &decision, T));
	zassert_equal(decision.celsius, 23);
	zassert_equal(decision.percent, 30);
	zassert_ok(fan_get_speed(&fan_a, &speed, T));
	zassert_equal(speed.percent, 30);
	zassert_ok(fan_get_speed(&fan_shared, &speed, T));
	zassert_equal(speed.percent, 30);
	zassert_ok(fan_get_speed(&fan_b, &speed, T));
	zassert_equal(speed.percent, 0, "room_a never touches room_b's intake");

	/* room_b: 10 °C + offset 1 is below its setpoint, so both its fans stop,
	 * including the exhaust it shares with room_a. */
	zassert_ok(climate_regulate(&room_b, &decision, T));
	zassert_equal(decision.celsius, 11);
	zassert_equal(decision.percent, 0);
	zassert_ok(fan_get_speed(&fan_shared, &speed, T));
	zassert_equal(speed.percent, 0);
}

ZTEST(deps, test_shared_provider_keeps_its_own_state)
{
	struct temperature_reading t = {.celsius = 40};
	struct fan_speed speed;
	uint32_t sets = fan_shared_data.sets;

	zassert_ok(temperature_set(&sensor_a, &t, T));
	zassert_ok(climate_regulate(&room_a, NULL, T));
	zassert_ok(climate_regulate(&room_b, NULL, T));
	zassert_equal(fan_shared_data.sets, sets + 2, "both consumers reached one fan");

	/* Its own config still caps it, whoever calls. */
	zassert_ok(climate_regulate(&room_a, NULL, T));
	zassert_ok(fan_get_speed(&fan_shared, &speed, T));
	zassert_equal(speed.percent, 80);
}

ZTEST(deps, test_failed_provider_skips_consumer)
{
	zassert_equal(zephlet_init_result(&sensor_broken), -EIO);
	zassert_equal(zephlet_init_result(&room_broken), -ENODEV);
	zassert_equal(init_pos(&room_broken), SIZE_MAX, "init_fn never ran");
	zassert_false(room_broken_data.is_ready);
}

ZTEST(deps, test_init_fn_may_call_providers)
{
	/* Init entries run after zbus: the calls reached fan_probe's listener. */
	zassert_ok(zephlet_init_result(&room_probe));
	zassert_equal(probe_at_init.set_rc, 0);
	zassert_equal(probe_at_init.sets, 1, "the set ran during init");
	zassert_equal(probe_at_init.get_rc, 0);
	zassert_equal(probe_at_init.percent, 7, "the response was written during init");
}

ZTEST(deps, test_init_fn_may_call_its_own_rpcs)
{
	/* A consumer with dependencies is served while its init_fn runs. */
	zassert_equal(probe_at_init.self_rc, 0);
	zassert_true(probe_at_init.self_ready, "its own handler ran during init");
}

ZTEST(deps, test_init_fn_error_is_its_result)
{
	zassert_equal(zephlet_init_result(&fan_eloop), -ELOOP);
	zassert_equal(fan_get_speed(&fan_eloop, NULL, T), -ENODEV,
		      "a failed init_fn: no calls, with or without dependencies");
	zassert_equal(zephlet_init_result(&room_eloop), -ENODEV);
	zassert_equal(init_pos(&room_eloop), SIZE_MAX);
	zassert_ok(zephlet_init_result(&sensor_b), "unrelated instances initialize");
}

ZTEST(deps, test_bad_binding_skips_consumer)
{
	zassert_equal(zephlet_init_result(&room_unbound), -ENODEV);
	zassert_equal(zephlet_init_result(&room_mistyped), -ENODEV);
	zassert_equal(init_pos(&room_unbound), SIZE_MAX);
	zassert_equal(init_pos(&room_mistyped), SIZE_MAX);
	/* Their providers are fine, and still serve everyone else. */
	zassert_equal(zephlet_init_result(&fan_a), 0);
	zassert_equal(zephlet_init_result(&sensor_b), 0);
}

ZTEST(deps, test_init_result_of_unknown)
{
	zassert_equal(zephlet_init_result(NULL), -ENOENT);
}

ZTEST(deps, test_wrapper_rejects_instance_of_another_type)
{
	/* A climate wrapper given a temperature instance, asserts or not. */
	zassert_equal(climate_regulate(&sensor_a, NULL, T), -EINVAL);
	zassert_equal(fan_set_speed(&room_a, &(struct fan_speed){.percent = 1}, T), -EINVAL);
	zassert_equal(climate_regulate(NULL, NULL, T), -EINVAL);
}

ZTEST(deps, test_skipped_instance_refuses_calls)
{
	/* Its handler would reach the slot initialization rejected. */
	zassert_equal(climate_regulate(&room_unbound, NULL, T), -ENODEV);
	zassert_equal(climate_regulate(&room_mistyped, NULL, T), -ENODEV);
	zassert_equal(climate_regulate(&room_broken, NULL, T), -ENODEV);
}

ZTEST(deps, test_inst_define_with_deps)
{
	zassert_equal(zephlet_init_result(&climate_auto), 0);
	zassert_equal_ptr(climate_auto.dependencies, &auto_deps);
	zassert_equal_ptr(climate_auto.config, &climate_auto_cfg);
}

ZTEST_SUITE(deps, NULL, NULL, NULL, NULL, NULL);
