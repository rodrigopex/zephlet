/*
 * Initialization of hand-built instance records, without codegen: each
 * registers its runtime record and SYS_INIT entry with ZEPHLET_INIT_DEFINE
 * at a priority this file picks (LEAF_PRIO < MID_PRIO < TOP_PRIO for a
 * chain).
 *
 * The build's type-graph check rejects every cycle of zephlet types, and a
 * slot only accepts an instance of its own type, so a cycle of instances
 * only appears in records written by hand. A cycle member finds a provider
 * not initialized yet and is skipped with -ENODEV; so is anything that
 * needs it. These are such records: a `node` type that requires another
 * `node` (so instances can form any cycle), and a chain of types `top` ->
 * `mid` -> `leaf` for graphs without one.
 */

#include <errno.h>
#include <stddef.h>

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/ztest.h>

#include "zephlet.h"

#define MAX_INIT 32

static const struct zephlet *init_seq[MAX_INIT];
static size_t init_len;

static int record_init(const struct zephlet *z)
{
	zassert_true(init_len < MAX_INIT);
	init_seq[init_len++] = z;
	return 0;
}

static int failing_init(const struct zephlet *z)
{
	record_init(z);
	return -EIO;
}

static bool initialized(const struct zephlet *z)
{
	for (size_t i = 0; i < init_len; i++) {
		if (init_seq[i] == z) {
			return true;
		}
	}
	return false;
}

struct node_dependencies {
	const struct zephlet *next;
};

extern const struct zephlet_api node_api;

static const struct zephlet_dep_slot node_slots[] = {
	{
		.name = "next",
		.type = "node",
		.api = &node_api,
		.offset = offsetof(struct node_dependencies, next),
	},
};

const struct zephlet_api node_api = {
	.deps = node_slots,
	.num_deps = ARRAY_SIZE(node_slots),
};

static const struct zephlet_api leaf_api;

/* One single-slot type, `_type` requiring `_req`. */
#define SINGLE_SLOT_TYPE(_type, _req)                                                              \
	static const struct zephlet_dep_slot _type##_slots[] = {                                   \
		{                                                                                  \
			.name = "next",                                                            \
			.type = #_req,                                                             \
			.api = &_req##_api,                                                        \
			.offset = offsetof(struct node_dependencies, next),                        \
		},                                                                                 \
	};                                                                                         \
	static const struct zephlet_api _type##_api = {                                            \
		.deps = _type##_slots,                                                             \
		.num_deps = ARRAY_SIZE(_type##_slots),                                             \
	}

SINGLE_SLOT_TYPE(mid, leaf);
SINGLE_SLOT_TYPE(top, mid);

/* Init priorities of a chain; all above CONFIG_ZBUS_CHANNELS_SYS_INIT_PRIORITY. */
#define LEAF_PRIO 20
#define MID_PRIO  21
#define TOP_PRIO  22

/* What ZEPHLET_NEW does for every instance: its runtime record and init
 * entry, ahead of the record that points at it. */
#define LEAF(_name, _init, _prio)                                                                  \
	ZEPHLET_INIT_DEFINE(_name, _prio, 0);                                                      \
	const STRUCT_SECTION_ITERABLE(zephlet, _name) = {                                          \
		.name = #_name,                                                                    \
		.api = &leaf_api,                                                                  \
		.init_fn = (_init),                                                                \
		.runtime = ZEPHLET_RUNTIME(_name),                                                 \
	}

#define BOUND(_type, _name, _next, _prio)                                                          \
	extern const struct zephlet _next;                                                         \
	static const struct node_dependencies _name##_deps = {.next = &(_next)};                   \
	ZEPHLET_INIT_DEFINE(_name, _prio, 0);                                                      \
	const STRUCT_SECTION_ITERABLE(zephlet, _name) = {                                          \
		.name = #_name,                                                                    \
		.api = &_type##_api,                                                               \
		.dependencies = &_name##_deps,                                                     \
		.init_fn = record_init,                                                            \
		.runtime = ZEPHLET_RUNTIME(_name),                                                 \
	}

#define NODE(_name, _next) BOUND(node, _name, _next, LEAF_PRIO)

/* a -> b -> c -> a */
NODE(cyc3_a, cyc3_b);
NODE(cyc3_b, cyc3_c);
NODE(cyc3_c, cyc3_a);

/* a -> b -> a */
NODE(cyc2_a, cyc2_b);
NODE(cyc2_b, cyc2_a);

/* a -> a */
NODE(cyc1_a, cyc1_a);

/* A `pair` requires two `pair`s, so one instance can sit on two cycles:
 * a -> b, b -> {c, d}, c -> a, d -> c holds a -> b -> c -> a and
 * a -> b -> d -> c -> a. Every member is skipped. */
struct pair_dependencies {
	const struct zephlet *first;
	const struct zephlet *second;
};

extern const struct zephlet_api pair_api;

static const struct zephlet_dep_slot pair_slots[] = {
	{
		.name = "first",
		.type = "pair",
		.api = &pair_api,
		.offset = offsetof(struct pair_dependencies, first),
	},
	{
		.name = "second",
		.type = "pair",
		.api = &pair_api,
		.offset = offsetof(struct pair_dependencies, second),
	},
};

const struct zephlet_api pair_api = {
	.deps = pair_slots,
	.num_deps = ARRAY_SIZE(pair_slots),
};

#define PAIR(_name, _first, _second)                                                               \
	extern const struct zephlet _first;                                                        \
	extern const struct zephlet _second;                                                       \
	static const struct pair_dependencies _name##_deps = {.first = &(_first),                  \
							      .second = &(_second)};               \
	ZEPHLET_INIT_DEFINE(_name, LEAF_PRIO, 0);                                                  \
	const STRUCT_SECTION_ITERABLE(zephlet, _name) = {                                          \
		.name = #_name,                                                                    \
		.api = &pair_api,                                                                  \
		.dependencies = &_name##_deps,                                                     \
		.init_fn = record_init,                                                            \
		.runtime = ZEPHLET_RUNTIME(_name),                                                 \
	}

PAIR(dia_a, dia_b, dia_b);
PAIR(dia_b, dia_c, dia_d);
PAIR(dia_c, dia_a, dia_a);
PAIR(dia_d, dia_c, dia_c);

/* Off a cycle, but needs one of its members: never initializable either. */
BOUND(node, tail_on_cycle, cyc3_b, MID_PRIO);

/* A healthy chain: chain_a -> chain_b -> leaf_ok. */
BOUND(top, chain_a, chain_b, TOP_PRIO);
BOUND(mid, chain_b, leaf_ok, MID_PRIO);
LEAF(leaf_ok, record_init, LEAF_PRIO);

/* A failure two hops down cascades: fail_a -> fail_b -> leaf_bad. */
BOUND(top, fail_a, fail_b, TOP_PRIO);
BOUND(mid, fail_b, leaf_bad, MID_PRIO);
LEAF(leaf_bad, failing_init, LEAF_PRIO);

/* Ranked before its provider: it finds the leaf not initialized yet. The
 * leaf itself still initializes later. */
BOUND(mid, too_early, leaf_late, LEAF_PRIO);
LEAF(leaf_late, record_init, TOP_PRIO);

/* A node whose dependency record is missing altogether. */
ZEPHLET_INIT_DEFINE(no_record, LEAF_PRIO, 0);
const STRUCT_SECTION_ITERABLE(zephlet, no_record) = {
	.name = "no_record",
	.api = &node_api,
	.init_fn = record_init,
	.runtime = ZEPHLET_RUNTIME(no_record),
};

/* Bound to a leaf where a node is required. */
BOUND(node, wrong_type, leaf_ok, MID_PRIO);

ZTEST(deps_init, test_cycles_of_any_length_are_skipped)
{
	const struct zephlet *on_cycle[] = {
		&cyc3_a, &cyc3_b, &cyc3_c, &cyc2_a, &cyc2_b,
		&cyc1_a, &dia_a,  &dia_b,  &dia_c,  &dia_d,
	};

	for (size_t i = 0; i < ARRAY_SIZE(on_cycle); i++) {
		zassert_equal(zephlet_init_result(on_cycle[i]), -ENODEV, "%s", on_cycle[i]->name);
		zassert_false(initialized(on_cycle[i]), "%s", on_cycle[i]->name);
	}
}

ZTEST(deps_init, test_consumer_of_a_cycle_is_skipped)
{
	zassert_equal(zephlet_init_result(&tail_on_cycle), -ENODEV);
	zassert_false(initialized(&tail_on_cycle));
}

ZTEST(deps_init, test_consumer_ranked_before_its_provider_is_skipped)
{
	zassert_equal(zephlet_init_result(&too_early), -ENODEV);
	zassert_false(initialized(&too_early));
	zassert_equal(zephlet_init_result(&leaf_late), 0, "the provider runs at its own turn");
}

ZTEST(deps_init, test_chain_initializes_leaf_first)
{
	zassert_equal(zephlet_init_result(&chain_a), 0);
	zassert_equal(zephlet_init_result(&chain_b), 0);
	zassert_equal(zephlet_init_result(&leaf_ok), 0);

	size_t a = SIZE_MAX, b = SIZE_MAX, leaf = SIZE_MAX;

	for (size_t i = 0; i < init_len; i++) {
		if (init_seq[i] == &chain_a) {
			a = i;
		} else if (init_seq[i] == &chain_b) {
			b = i;
		} else if (init_seq[i] == &leaf_ok) {
			leaf = i;
		}
	}
	zassert_true(leaf < b && b < a, "leaf %zu, b %zu, a %zu", leaf, b, a);
}

ZTEST(deps_init, test_failure_cascades_along_the_chain)
{
	zassert_equal(zephlet_init_result(&leaf_bad), -EIO);
	zassert_equal(zephlet_init_result(&fail_b), -ENODEV);
	zassert_equal(zephlet_init_result(&fail_a), -ENODEV);
	zassert_false(initialized(&fail_b));
	zassert_false(initialized(&fail_a));
}

ZTEST(deps_init, test_bad_records_are_skipped)
{
	zassert_equal(zephlet_init_result(&no_record), -ENODEV);
	zassert_equal(zephlet_init_result(&wrong_type), -ENODEV);
	zassert_false(initialized(&no_record));
	zassert_false(initialized(&wrong_type));
}

ZTEST(deps_init, test_every_initialized_instance_ran_once)
{
	/* chain_a, chain_b, leaf_ok, leaf_bad, leaf_late. */
	zassert_equal(init_len, 5);
}

ZTEST_SUITE(deps_init, NULL, NULL, NULL, NULL, NULL);
