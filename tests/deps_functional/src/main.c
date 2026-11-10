/*
 * Composition through declared dependencies: `climate` requires one
 * `temperature` and two `fan` slots (intake, exhaust); each controller
 * instance is bound to its own providers, and both share one exhaust fan.
 */

#include <errno.h>

#include <zephyr/ztest.h>
#include <zephyr/ztest_error_hook.h>

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

/* Initialized late on purpose: its consumers still wait for it. */
static struct fan_config fan_b_cfg = {.max_percent = 100};
static struct fan_data fan_b_data;
ZEPHLET_NEW_PRIO(fan, fan_b, &fan_b_cfg, &fan_b_data, fan_init, 50);

static struct fan_config fan_shared_cfg = {.max_percent = 80};
static struct fan_data fan_shared_data;
ZEPHLET_NEW(fan, fan_shared, &fan_shared_cfg, &fan_shared_data, fan_init);

/* ----- Consumers ---------------------------------------------------------- */

static const struct climate_dependencies room_a_deps = {
	.temperature = &sensor_a,
	.intake = &fan_a,
	.exhaust = &fan_shared,
};
static struct climate_config room_a_cfg = {.setpoint_c = 20};
static struct climate_data room_a_data;
/* A lower priority than every provider: dependency order wins. */
ZEPHLET_NEW_PRIO_WITH_DEPS(climate, room_a, &room_a_cfg, &room_a_data, &room_a_deps,
			   controller_init, -10);

static const struct climate_dependencies room_b_deps = {
	.temperature = &sensor_b,
	.intake = &fan_b,
	.exhaust = &fan_shared,
};
static struct climate_config room_b_cfg = {.setpoint_c = 25};
static struct climate_data room_b_data;
ZEPHLET_NEW_WITH_DEPS(climate, room_b, &room_b_cfg, &room_b_data, &room_b_deps,
		      controller_init);

/* A provider whose init_fn fails. */
static const struct climate_dependencies room_broken_deps = {
	.temperature = &sensor_broken,
	.intake = &fan_a,
	.exhaust = &fan_shared,
};
static struct climate_config room_broken_cfg;
static struct climate_data room_broken_data;
ZEPHLET_NEW_WITH_DEPS(climate, room_broken, &room_broken_cfg, &room_broken_data,
		      &room_broken_deps, controller_init);

/* A slot left unbound. */
static const struct climate_dependencies room_unbound_deps = {
	.temperature = &sensor_a,
	.intake = &fan_a,
};
static struct climate_config room_unbound_cfg;
static struct climate_data room_unbound_data;
ZEPHLET_NEW_WITH_DEPS(climate, room_unbound, &room_unbound_cfg, &room_unbound_data,
		      &room_unbound_deps, controller_init);

/* A slot bound to an instance of another type: the field type cannot tell. */
static const struct climate_dependencies room_mistyped_deps = {
	.temperature = &sensor_a,
	.intake = &sensor_b,
	.exhaust = &fan_shared,
};
static struct climate_config room_mistyped_cfg;
static struct climate_data room_mistyped_data;
ZEPHLET_NEW_WITH_DEPS(climate, room_mistyped, &room_mistyped_cfg, &room_mistyped_data,
		      &room_mistyped_deps, controller_init);

/* ----- Tests -------------------------------------------------------------- */

ZTEST(deps, test_providers_initialize_before_consumers)
{
	zassert_equal(zephlet_init_result(&room_a), 0);
	zassert_equal(zephlet_init_result(&room_b), 0);

	zassert_true(init_pos(&sensor_a) < init_pos(&room_a));
	zassert_true(init_pos(&fan_a) < init_pos(&room_a));
	zassert_true(init_pos(&fan_shared) < init_pos(&room_a));
	zassert_true(init_pos(&sensor_b) < init_pos(&room_b));
	/* fan_b has the highest priority value of all, yet room_b waits. */
	zassert_true(init_pos(&fan_b) < init_pos(&room_b));
}

ZTEST(deps, test_init_order_is_priority_among_ready)
{
	/*
	 * By priority (ties in section, i.e. name, order): room_a (-10); fan_a,
	 * fan_shared, room_b, room_broken, room_mistyped, room_unbound,
	 * sensor_a, sensor_b, sensor_broken (0); fan_b (50). Each step runs the
	 * first instance whose providers are done: room_a as soon as sensor_a
	 * is, ahead of sensor_b; room_b only after fan_b. Skipped instances
	 * never run.
	 */
	const struct zephlet *expected[] = {
		&fan_a, &fan_shared, &sensor_a, &room_a, &sensor_b, &sensor_broken, &fan_b, &room_b,
	};

	zassert_equal(init_len, ARRAY_SIZE(expected));
	for (size_t i = 0; i < ARRAY_SIZE(expected); i++) {
		zassert_equal_ptr(init_seq[i], expected[i], "step %zu: got '%s', want '%s'", i,
				  init_seq[i]->name, expected[i]->name);
	}
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

static bool expect_assert;

void ztest_post_assert_fail_hook(void)
{
	if (expect_assert) {
		expect_assert = false;
		ztest_test_pass();
	} else {
		ztest_test_fail();
	}
}

ZTEST(deps, test_wrapper_rejects_instance_of_another_type)
{
	expect_assert = true;
	ztest_set_assert_valid(true);
	/* A climate wrapper given a temperature instance. */
	(void)climate_regulate(&sensor_a, NULL, T);
	ztest_test_fail();
}

ZTEST_SUITE(deps, NULL, NULL, NULL, NULL, NULL);
