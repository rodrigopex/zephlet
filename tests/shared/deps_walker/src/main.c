/*
 * The SYS_INIT walker on hand-built instance records, without codegen.
 *
 * Codegen and the configuration-time graph check reject every cycle of
 * zephlet types, and binding validation then rules out every cycle of
 * instances, so a cycle only reaches the walker through records written by
 * hand. These are such records: a `node` type that requires another `node`
 * (so instances can form any cycle), and a chain of types `top` -> `mid` ->
 * `leaf` for graphs without one.
 */

#include <errno.h>
#include <stddef.h>

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/ztest.h>

#include "zephlet.h"

static const struct zephlet *init_seq[CONFIG_ZEPHLET_MAX_INSTANCES];
static size_t init_len;

static int record_init(const struct zephlet *z)
{
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

#define LEAF(_name, _init)                                                                         \
	const STRUCT_SECTION_ITERABLE(zephlet, _name) = {                                          \
		.name = #_name,                                                                    \
		.api = &leaf_api,                                                                  \
		.init_fn = (_init),                                                                \
	}

#define BOUND(_type, _name, _next)                                                                 \
	extern const struct zephlet _next;                                                         \
	static const struct node_dependencies _name##_deps = {.next = &(_next)};                   \
	const STRUCT_SECTION_ITERABLE(zephlet, _name) = {                                          \
		.name = #_name,                                                                    \
		.api = &_type##_api,                                                               \
		.dependencies = &_name##_deps,                                                     \
		.init_fn = record_init,                                                            \
	}

#define NODE(_name, _next) BOUND(node, _name, _next)

/* a -> b -> c -> a */
NODE(cyc3_a, cyc3_b);
NODE(cyc3_b, cyc3_c);
NODE(cyc3_c, cyc3_a);

/* a -> b -> a */
NODE(cyc2_a, cyc2_b);
NODE(cyc2_b, cyc2_a);

/* a -> a */
NODE(cyc1_a, cyc1_a);

/* Off a cycle, but needs one of its members. */
NODE(tail_on_cycle, cyc3_b);

/* A healthy chain: chain_a -> chain_b -> leaf_ok. */
BOUND(top, chain_a, chain_b);
BOUND(mid, chain_b, leaf_ok);
LEAF(leaf_ok, record_init);

/* A failure two hops down cascades: fail_a -> fail_b -> leaf_bad. */
BOUND(top, fail_a, fail_b);
BOUND(mid, fail_b, leaf_bad);
LEAF(leaf_bad, failing_init);

/* A node whose dependency record is missing altogether. */
const STRUCT_SECTION_ITERABLE(zephlet, no_record) = {
	.name = "no_record",
	.api = &node_api,
	.init_fn = record_init,
};

/* Bound to a leaf where a node is required. */
NODE(wrong_type, leaf_ok);

ZTEST(deps_walker, test_cycles_of_any_length_are_skipped)
{
	const struct zephlet *on_cycle[] = {
		&cyc3_a, &cyc3_b, &cyc3_c, &cyc2_a, &cyc2_b, &cyc1_a,
	};

	for (size_t i = 0; i < ARRAY_SIZE(on_cycle); i++) {
		zassert_equal(zephlet_init_result(on_cycle[i]), -ELOOP, "%s", on_cycle[i]->name);
		zassert_false(initialized(on_cycle[i]), "%s", on_cycle[i]->name);
	}
}

ZTEST(deps_walker, test_consumer_of_a_cycle_is_skipped)
{
	zassert_equal(zephlet_init_result(&tail_on_cycle), -ENODEV);
	zassert_false(initialized(&tail_on_cycle));
}

ZTEST(deps_walker, test_chain_initializes_leaf_first)
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

ZTEST(deps_walker, test_failure_cascades_along_the_chain)
{
	zassert_equal(zephlet_init_result(&leaf_bad), -EIO);
	zassert_equal(zephlet_init_result(&fail_b), -ENODEV);
	zassert_equal(zephlet_init_result(&fail_a), -ENODEV);
	zassert_false(initialized(&fail_b));
	zassert_false(initialized(&fail_a), "fail_a sorts first, yet sees fail_b's skip");
}

ZTEST(deps_walker, test_bad_records_are_skipped)
{
	zassert_equal(zephlet_init_result(&no_record), -ENODEV);
	zassert_equal(zephlet_init_result(&wrong_type), -ENODEV);
	zassert_false(initialized(&no_record));
	zassert_false(initialized(&wrong_type));
}

ZTEST(deps_walker, test_every_initialized_instance_ran_once)
{
	/* chain_a, chain_b, leaf_ok, leaf_bad. */
	zassert_equal(init_len, 4);
}

ZTEST_SUITE(deps_walker, NULL, NULL, NULL, NULL, NULL);
