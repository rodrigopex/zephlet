#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/ztest.h>
#include <zephyr/sys/iterable_sections.h>

#include "fakes.h"
#include "zephlet.h"

/**
 * @file
 * @brief Isolated unit tests for zephlet.c: zephlet_init_one(), the init
 * entries ZEPHLET_INIT_ENTRY registers, the dispatch gate,
 * zephlet_init_result() and zephlet_get_by_name().
 *
 * Nine instances, n0..n8, live in the real instance section, each with its
 * runtime record and its own init entry. Each has its own api, so each is
 * its own type, and its own dependency record. A test binds them into a
 * graph with link(), then initializes them in an order it picks with
 * INIT(), or through their init entries in the order the linker sorted
 * them with boot(). Every init_fn is one FFF fake, so its call history is
 * the init order. Logging is a fake that keeps the formatted lines.
 */

DEFINE_FFF_GLOBALS;

#define NUM_NODES 9
#define MAX_SLOTS 3

DECLARE_FAKE_VALUE_FUNC(int, node_init, const struct zephlet *);
DEFINE_FAKE_VALUE_FUNC(int, node_init, const struct zephlet *);
DECLARE_FAKE_VALUE_FUNC(int, node_handler, const struct zephlet *, struct zephlet_call *);
DEFINE_FAKE_VALUE_FUNC(int, node_handler, const struct zephlet *, struct zephlet_call *);

struct node_record {
	const struct zephlet *slot[MAX_SLOTS];
};

static struct zephlet_api apis[NUM_NODES];
static struct zephlet_dep_slot slots[NUM_NODES][MAX_SLOTS];
static struct node_record records[NUM_NODES];

static const struct zephlet_method methods[] = {
	{0},
	{.handler = node_handler},
	{.handler = NULL},
};

ZEPHLET_RUNTIME_DEFINE(n0);
STRUCT_SECTION_ITERABLE(zephlet, n0) = {.name = "n0", .runtime = ZEPHLET_RUNTIME(n0)};
ZEPHLET_RUNTIME_DEFINE(n1);
STRUCT_SECTION_ITERABLE(zephlet, n1) = {.name = "n1", .runtime = ZEPHLET_RUNTIME(n1)};
ZEPHLET_RUNTIME_DEFINE(n2);
STRUCT_SECTION_ITERABLE(zephlet, n2) = {.name = "n2", .runtime = ZEPHLET_RUNTIME(n2)};
ZEPHLET_RUNTIME_DEFINE(n3);
STRUCT_SECTION_ITERABLE(zephlet, n3) = {.name = "n3", .runtime = ZEPHLET_RUNTIME(n3)};
ZEPHLET_RUNTIME_DEFINE(n4);
STRUCT_SECTION_ITERABLE(zephlet, n4) = {.name = "n4", .runtime = ZEPHLET_RUNTIME(n4)};
ZEPHLET_RUNTIME_DEFINE(n5);
STRUCT_SECTION_ITERABLE(zephlet, n5) = {.name = "n5", .runtime = ZEPHLET_RUNTIME(n5)};
ZEPHLET_RUNTIME_DEFINE(n6);
STRUCT_SECTION_ITERABLE(zephlet, n6) = {.name = "n6", .runtime = ZEPHLET_RUNTIME(n6)};
ZEPHLET_RUNTIME_DEFINE(n7);
STRUCT_SECTION_ITERABLE(zephlet, n7) = {.name = "n7", .runtime = ZEPHLET_RUNTIME(n7)};
ZEPHLET_RUNTIME_DEFINE(n8);
STRUCT_SECTION_ITERABLE(zephlet, n8) = {.name = "n8", .runtime = ZEPHLET_RUNTIME(n8)};

/* Each instance's init entry, as ZEPHLET_NEW registers one. Boot order: n2
 * (20, sub 1), n1 (20, sub 5), n0 (21), then n3..n8 (22, in an unspecified
 * order). */
#define NODE(_name, _prio, _sub) ZEPHLET_INIT_ENTRY(_name, _prio, _sub)
NODE(n0, 21, 0);
NODE(n1, 20, 5);
NODE(n2, 20, 1);
NODE(n3, 22, 0);
NODE(n4, 22, 0);
NODE(n5, 22, 0);
NODE(n6, 22, 0);
NODE(n7, 22, 0);
NODE(n8, 22, 0);

/* Not in the section, and no runtime record: never initialized. */
static struct zephlet outsider = {.name = "outsider"};

static struct zephlet_runtime *const runtime[NUM_NODES] = {
	ZEPHLET_RUNTIME(n0), ZEPHLET_RUNTIME(n1), ZEPHLET_RUNTIME(n2),
	ZEPHLET_RUNTIME(n3), ZEPHLET_RUNTIME(n4), ZEPHLET_RUNTIME(n5),
	ZEPHLET_RUNTIME(n6), ZEPHLET_RUNTIME(n7), ZEPHLET_RUNTIME(n8),
};

static struct zephlet *const node[NUM_NODES] = {&n0, &n1, &n2, &n3, &n4, &n5, &n6, &n7, &n8};
static const char *const default_name[NUM_NODES] = {"n0", "n1", "n2", "n3", "n4",
						    "n5", "n6", "n7", "n8"};
static const char *const slot_name[MAX_SLOTS] = {"a", "b", "c"};

/* ---- log capture ---- */

static char log_text[4096];
static size_t log_used;

static void log_capture(const char *fmt, va_list ap)
{
	if (log_used < sizeof(log_text)) {
		int n = vsnprintf(log_text + log_used, sizeof(log_text) - log_used, fmt, ap);

		if (n > 0) {
			log_used += (size_t)n;
		}
	}
}

static bool logged(const char *text)
{
	return strstr(log_text, text) != NULL;
}

/* ---- graph building ---- */

/* Give @p consumer one more slot, bound to @p provider (NULL: unbound),
 * that requires @p provider's type. */
static void link(size_t consumer, int provider)
{
	struct zephlet_api *api = &apis[consumer];
	size_t d = api->num_deps++;

	zassert_true(d < MAX_SLOTS, "too many slots on n%zu", consumer);
	slots[consumer][d] = (struct zephlet_dep_slot){
		.name = slot_name[d],
		.type = (provider < 0) ? "n0" : default_name[provider],
		.api = (provider < 0) ? &apis[0] : &apis[provider],
		.offset = offsetof(struct node_record, slot) + d * sizeof(records[0].slot[0]),
	};
	records[consumer].slot[d] = (provider < 0) ? NULL : node[provider];
}

/* Initialize the instances @p order lists, in that order. */
static void init(const size_t *order, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		(void)zephlet_init_one(node[order[i]]);
	}
}

#define INIT(...) init((const size_t[]){__VA_ARGS__}, ARRAY_SIZE(((const size_t[]){__VA_ARGS__})))

/* Run every init entry, in the order the linker sorted them, as the kernel
 * does at APPLICATION level. */
static void boot(void)
{
	extern const struct init_entry _init_unit_init_start[];
	extern const struct init_entry _init_unit_init_end[];
	size_t ran = 0;

	for (const struct init_entry *e = _init_unit_init_start; e < _init_unit_init_end; e++) {
		if (e->init_fn != NULL) {
			(void)e->init_fn();
			ran++;
		}
	}
	zassert_equal(ran, NUM_NODES, "one init entry per instance");
}

static size_t init_position(const struct zephlet *z)
{
	for (size_t i = 0; i < node_init_fake.call_count; i++) {
		if (node_init_fake.arg0_history[i] == z) {
			return i;
		}
	}
	return SIZE_MAX;
}

static int call(const struct zephlet *z, uint16_t method_id)
{
	struct zephlet_call c = {.method_id = method_id, .return_code = 1};

	zassert_ok(zephlet_dispatch(z, &c));
	return c.return_code;
}

static void reset(void *fixture)
{
	ARG_UNUSED(fixture);

	INIT_UNIT_FAKES_LIST(RESET_FAKE);
	RESET_FAKE(node_init);
	RESET_FAKE(node_handler);
	FFF_RESET_HISTORY();
	z_log_minimal_printk_fake.custom_fake = log_capture;
	memset(log_text, 0, sizeof(log_text));
	log_used = 0;

	memset(slots, 0, sizeof(slots));
	memset(records, 0, sizeof(records));
	for (size_t i = 0; i < NUM_NODES; i++) {
		apis[i] = (struct zephlet_api){
			.methods = methods,
			.num_methods = ARRAY_SIZE(methods),
			.deps = slots[i],
			.num_deps = 0,
		};
		node[i]->name = default_name[i];
		node[i]->api = &apis[i];
		node[i]->dependencies = &records[i];
		node[i]->init_fn = node_init;
		/* Back to "init entry not run yet". */
		node[i]->runtime = runtime[i];
		memset(runtime[i], 0, sizeof(*runtime[i]));
	}
	outsider.api = NULL;
}

/* ==== before initialization ==== */

ZTEST(init_0_uninitialized, test_results_and_gate_before_init)
{
	link(1, 0);
	zassert_equal(zephlet_init_result(&n0), -EAGAIN);
	zassert_equal(zephlet_init_result(NULL), -ENOENT);
	zassert_equal(zephlet_init_one(NULL), -ENOENT);
	zassert_equal(call(&n1, 1), -EAGAIN, "a consumer serves nothing before its init");
	zassert_equal(call(&n0, 1), -EAGAIN, "neither does a type without dependencies");
	zassert_equal(node_handler_fake.call_count, 0);
}

ZTEST_SUITE(init_0_uninitialized, NULL, NULL, reset, NULL, NULL);

/* ==== dispatch and lookup ==== */

ZTEST(init_1_dispatch, test_dispatch_rejects_null)
{
	struct zephlet_call c = {0};

	zassert_equal(zephlet_dispatch(NULL, &c), -EINVAL);
	zassert_equal(zephlet_dispatch(&n0, NULL), -EINVAL);
}

ZTEST(init_1_dispatch, test_dispatch_without_methods)
{
	apis[0].methods = NULL;
	zassert_equal(call(&n0, 1), -ENOSYS);
	n0.api = NULL;
	zassert_equal(call(&n0, 1), -ENOSYS);
}

ZTEST(init_1_dispatch, test_dispatch_bad_method)
{
	INIT(0);
	zassert_equal(call(&n0, ARRAY_SIZE(methods)), -ENOSYS, "id past the table");
	zassert_equal(call(&n0, 2), -ENOSYS, "row with no handler");
	zassert_equal(node_handler_fake.call_count, 0);
}

ZTEST(init_1_dispatch, test_dispatch_returns_handler_rc)
{
	INIT(0);
	node_handler_fake.return_val = -EIO;
	zassert_equal(call(&n0, 1), -EIO);
	zassert_equal(node_handler_fake.call_count, 1);
	zassert_equal_ptr(node_handler_fake.arg0_val, &n0);
}

ZTEST(init_1_dispatch, test_get_by_name)
{
	zassert_is_null(zephlet_get_by_name(NULL));
	zassert_equal_ptr(zephlet_get_by_name("n3"), &n3);
	zassert_is_null(zephlet_get_by_name("nope"));
}

ZTEST(init_1_dispatch, test_gate_follows_initialization)
{
	link(1, 0);
	INIT(0, 1);
	zassert_equal(call(&n1, 1), 0, "initialized consumer is served");
	zassert_equal(node_handler_fake.call_count, 1);
}

ZTEST(init_1_dispatch, test_skipped_consumer_is_gated)
{
	link(1, 0);
	node_init_fake.return_val = -EIO;
	INIT(0, 1);
	zassert_equal(call(&n1, 1), -ENODEV);
	zassert_equal(node_handler_fake.call_count, 0);
}

/* What an init_fn saw when it called its own instance. */
static int self_call_rc;
static int self_result_rc;
static int self_init_rc;

static int call_self(const struct zephlet *z)
{
	self_call_rc = call(z, 1);
	self_result_rc = zephlet_init_result(z);
	self_init_rc = zephlet_init_one(z);
	return 0;
}

static void self_call_during_init(size_t k)
{
	node_init_fake.custom_fake = call_self;
	self_call_rc = self_result_rc = self_init_rc = 1;
	INIT(0, k);

	zassert_equal(self_call_rc, 0, "an init_fn may call its own RPCs");
	zassert_equal(self_result_rc, -EAGAIN, "not final while init_fn runs");
	zassert_equal(self_init_rc, -EAGAIN, "and initializing again does nothing");
	zassert_ok(zephlet_init_result(node[k]));
	zassert_equal(call(node[k], 1), 0);
}

ZTEST(init_1_dispatch, test_init_fn_calls_itself_without_dependencies)
{
	self_call_during_init(0);
}

ZTEST(init_1_dispatch, test_init_fn_calls_itself_with_dependencies)
{
	link(1, 0);
	self_call_during_init(1);
}

static int fail_n0_n1(const struct zephlet *z)
{
	return (z == &n0 || z == &n1) ? -EIO : 0;
}

ZTEST(init_1_dispatch, test_failed_init_is_gated_with_or_without_dependencies)
{
	/* n0 has no dependencies; n1 needs n2. Both init_fns fail. */
	node_init_fake.custom_fake = fail_n0_n1;
	link(1, 2);
	INIT(0, 2, 1);

	zassert_equal(zephlet_init_result(&n0), -EIO);
	zassert_equal(zephlet_init_result(&n1), -EIO);
	zassert_equal(call(&n0, 1), -ENODEV, "no dependencies, failed init");
	zassert_equal(call(&n1, 1), -ENODEV, "dependencies, failed init");
	zassert_equal(call(&n2, 1), 0);
	zassert_equal(node_handler_fake.call_count, 1);
}

ZTEST_SUITE(init_1_dispatch, NULL, NULL, reset, NULL, NULL);

/* ==== initialization ==== */

ZTEST(init_2_init, test_init_entries_run_by_priority_then_sub_priority)
{
	boot();

	zassert_equal(node_init_fake.call_count, NUM_NODES);
	zassert_equal_ptr(node_init_fake.arg0_history[0], &n2, "20, sub-priority 1");
	zassert_equal_ptr(node_init_fake.arg0_history[1], &n1, "20, sub-priority 5");
	zassert_equal_ptr(node_init_fake.arg0_history[2], &n0, "21");
	for (size_t i = 3; i < NUM_NODES; i++) {
		zassert_true(init_position(node[i]) > 2, "n%zu at 22 comes last", i);
		zassert_ok(zephlet_init_result(node[i]));
	}
}

ZTEST(init_2_init, test_boot_initializes_providers_first)
{
	/* n0 (21) -> n2 (20, sub 1) and n1 (20, sub 5) -> n2: priorities that
	 * rank every provider below its consumers. */
	link(0, 1);
	link(0, 2);
	link(1, 2);
	boot();

	zassert_ok(zephlet_init_result(&n0));
	zassert_ok(zephlet_init_result(&n1));
	zassert_true(init_position(&n2) < init_position(&n1));
	zassert_true(init_position(&n1) < init_position(&n0));
	zassert_equal(call(&n0, 1), 0);
}

ZTEST(init_2_init, test_initializes_once)
{
	INIT(0, 0);
	zassert_equal(node_init_fake.call_count, 1);

	node_init_fake.return_val = -EIO;
	INIT(1);
	zassert_equal(zephlet_init_one(&n1), -EIO, "a second call returns the first result");
	zassert_equal(node_init_fake.call_count, 2);
}

ZTEST(init_2_init, test_shared_provider_and_repeated_slots)
{
	link(1, 0);
	link(2, 0);
	link(3, 0);
	link(3, 0);
	INIT(0, 1, 2, 3);

	zassert_equal(node_init_fake.call_count, 4, "n0 initializes once");
	zassert_ok(zephlet_init_result(&n3));
}

ZTEST(init_2_init, test_null_init_fn_counts_as_initialized)
{
	n0.init_fn = NULL;
	link(1, 0);
	INIT(0, 1);

	zassert_ok(zephlet_init_result(&n0));
	zassert_ok(zephlet_init_result(&n1));
	zassert_equal(init_position(&n0), SIZE_MAX);
}

ZTEST(init_2_init, test_instance_without_api)
{
	n0.api = NULL;
	INIT(0);

	zassert_ok(zephlet_init_result(&n0), "an api-less instance has no dependencies");
	zassert_equal(call(&n0, 1), -ENOSYS);
}

static int fail_n0(const struct zephlet *z)
{
	return (z == &n0) ? -EIO : 0;
}

ZTEST(init_2_init, test_failed_provider_skips_consumers_transitively)
{
	node_init_fake.custom_fake = fail_n0;
	link(1, 0);
	link(2, 1);
	link(3, 2);
	INIT(0, 1, 2, 3, 4);

	zassert_equal(zephlet_init_result(&n0), -EIO);
	zassert_equal(zephlet_init_result(&n1), -ENODEV);
	zassert_equal(zephlet_init_result(&n2), -ENODEV);
	zassert_equal(zephlet_init_result(&n3), -ENODEV);
	zassert_equal(init_position(&n1), SIZE_MAX, "a skipped init_fn never runs");
	zassert_equal(init_position(&n3), SIZE_MAX);
	zassert_ok(zephlet_init_result(&n4), "unrelated instances still initialize");
	zassert_true(logged("zephlet 'n0' init_fn returned -5"));
	zassert_true(logged("provider 'n0' (dependency 'a') is unavailable (-5)"));
}

static int eloop_n0(const struct zephlet *z)
{
	return (z == &n0) ? -ELOOP : 0;
}

ZTEST(init_2_init, test_init_fn_returning_eloop_is_its_own_result)
{
	node_init_fake.custom_fake = eloop_n0;
	link(1, 0);
	INIT(0, 1, 2);

	zassert_equal(zephlet_init_result(&n0), -ELOOP);
	zassert_equal(zephlet_init_result(&n1), -ENODEV);
	zassert_ok(zephlet_init_result(&n2));
}

ZTEST(init_2_init, test_missing_record)
{
	link(1, 0);
	n1.dependencies = NULL;
	link(2, 1);
	INIT(0, 1, 2);

	zassert_equal(zephlet_init_result(&n1), -ENODEV);
	zassert_equal(zephlet_init_result(&n2), -ENODEV, "consumer of a skipped one");
	zassert_true(logged("zephlet 'n1' has no dependency record"));
}

ZTEST(init_2_init, test_unbound_slot)
{
	link(1, 0);
	link(1, -1);
	INIT(0, 1);

	zassert_equal(zephlet_init_result(&n1), -ENODEV);
	zassert_true(logged("dependency 'b' (a n0) is not bound"));
}

ZTEST(init_2_init, test_wrong_type)
{
	link(1, 0);
	records[1].slot[0] = &n2;
	INIT(0, 2, 1);

	zassert_equal(zephlet_init_result(&n1), -ENODEV);
	zassert_true(logged("dependency 'a' needs a n0, got 'n2'"));
	zassert_equal(init_position(&n1), SIZE_MAX);
}

ZTEST(init_2_init, test_bound_to_a_non_instance)
{
	link(1, 0);
	records[1].slot[0] = &outsider;
	outsider.api = &apis[0];
	INIT(0, 1);

	zassert_equal(zephlet_init_result(&n1), -ENODEV);
	zassert_true(logged("dependency 'a' is not bound to a zephlet instance"));
	zassert_equal(zephlet_init_result(&outsider), -ENOENT);
	zassert_equal(zephlet_init_one(&outsider), -ENOENT);
	outsider.api = &apis[1];
	zassert_equal(call(&outsider, 1), -EAGAIN, "a record without runtime is never served");
}

ZTEST(init_2_init, test_bound_to_an_instance_without_runtime)
{
	link(1, 2);
	n2.runtime = NULL;
	INIT(2, 1);

	zassert_equal(zephlet_init_result(&n2), -ENOENT);
	zassert_equal(zephlet_init_result(&n1), -ENODEV);
	zassert_true(logged("dependency 'a' is not bound to a zephlet instance"));
}

#define NOT_YET "is not initialized yet; it must rank below its consumer"

ZTEST(init_2_init, test_consumer_before_its_provider_is_skipped)
{
	link(1, 0);
	INIT(1, 0);

	zassert_equal(zephlet_init_result(&n1), -ENODEV);
	zassert_true(
		logged("zephlet 'n1' not initialized: provider 'n0' (dependency 'a') " NOT_YET));
	zassert_ok(zephlet_init_result(&n0), "the provider still initializes at its turn");
}

ZTEST(init_2_init, test_self_cycle)
{
	link(0, 0);
	INIT(0);

	zassert_equal(zephlet_init_result(&n0), -ENODEV);
	zassert_equal(init_position(&n0), SIZE_MAX);
	zassert_true(logged("provider 'n0' (dependency 'a') " NOT_YET));
}

ZTEST(init_2_init, test_cycles_never_run_an_init_fn)
{
	/* n0 <-> n1, n2 -> n3 -> n4 -> n2, n5 -> n1 (a consumer of a cycle). */
	link(0, 1);
	link(1, 0);
	link(2, 3);
	link(3, 4);
	link(4, 2);
	link(5, 1);
	INIT(0, 1, 2, 3, 4, 5, 6);

	for (size_t i = 0; i < 6; i++) {
		zassert_equal(zephlet_init_result(node[i]), -ENODEV, "n%zu", i);
		zassert_equal(init_position(node[i]), SIZE_MAX, "n%zu", i);
	}
	zassert_ok(zephlet_init_result(&n6));
}

/* a -> {b, p}, b -> a, p fails its init_fn: no cyclic init_fn runs, p keeps
 * its own result. */
static int fail_n2(const struct zephlet *z)
{
	return (z == &n2) ? -EIO : 0;
}

static void failing_provider_and_cycle(bool cycle_slot_first)
{
	node_init_fake.custom_fake = fail_n2;
	link(0, cycle_slot_first ? 1 : 2);
	link(0, cycle_slot_first ? 2 : 1);
	link(1, 0);
	INIT(2, 0, 1, 3);

	zassert_equal(zephlet_init_result(&n0), -ENODEV);
	zassert_equal(zephlet_init_result(&n1), -ENODEV);
	zassert_equal(zephlet_init_result(&n2), -EIO);
	zassert_equal(init_position(&n0), SIZE_MAX);
	zassert_equal(init_position(&n1), SIZE_MAX);
	zassert_ok(zephlet_init_result(&n3));
}

ZTEST(init_2_init, test_failing_provider_beside_a_cycle)
{
	failing_provider_and_cycle(true);
}

ZTEST(init_2_init, test_failing_provider_beside_a_cycle_other_order)
{
	failing_provider_and_cycle(false);
}

ZTEST(init_2_init, test_bad_slot_beside_a_cycle)
{
	/* n0 -> {n1, unbound}, n1 -> n0: both skipped, the bad slot logged. */
	link(0, 1);
	link(0, -1);
	link(1, 0);
	INIT(0, 1);

	zassert_equal(zephlet_init_result(&n0), -ENODEV);
	zassert_equal(zephlet_init_result(&n1), -ENODEV);
	zassert_equal(init_position(&n0), SIZE_MAX);
	zassert_equal(init_position(&n1), SIZE_MAX);
	zassert_true(logged("dependency 'b' (a n0) is not bound"));
}

ZTEST_SUITE(init_2_init, NULL, NULL, reset, NULL, NULL);
