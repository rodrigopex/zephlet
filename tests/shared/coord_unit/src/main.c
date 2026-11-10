#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/zbus/zbus.h>

#include "fakes.h"
#include "zephlet_coord.h"

/**
 * @file
 * @brief Isolated unit tests for zephlet_coord.c.
 *
 * The coordinator runs against FFF fakes. k_work_submit_to_queue() queues
 * into a test-owned FIFO that tests drain explicitly, so every transition
 * is deterministic and nothing runs behind a test's back. A publish is
 * modelled by calling the generated listener directly, and a timeout by
 * running the timer's handler. These tests check how the coordinator
 * responds to modelled dependency behaviour; real scheduling and races are
 * covered by tests/shared/coord.
 */

DEFINE_FFF_GLOBALS;

#define DST_SENTINEL 0x5A5A

struct payload {
	int value;
};

ZBUS_CHAN_DEFINE(chan_test, struct payload, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.value = 0));

struct unit_ctx {
	struct payload dst;
	zephlet_coord_match_fn match;
	k_timeout_t timeout;
	uint8_t result_at_arm;
	uint8_t results[4];
	int after_count;
};

static struct unit_ctx ctx;

/* ---- deterministic stand-in for the coordinator workqueue ---- */

#define FIFO_LEN 16

static struct k_work *fifo[FIFO_LEN];
static size_t fifo_count;

static int fifo_submit(struct k_work_q *queue, struct k_work *work)
{
	ARG_UNUSED(queue);

	for (size_t i = 0; i < fifo_count; i++) {
		if (fifo[i] == work) {
			/* Already pending: the real kernel also reports 0. */
			return 0;
		}
	}
	zassert_true(fifo_count < FIFO_LEN, "work FIFO overflow");
	fifo[fifo_count++] = work;
	return 1;
}

static bool run_one(void)
{
	struct k_work *work;

	if (fifo_count == 0U) {
		return false;
	}
	work = fifo[0];
	fifo_count--;
	memmove(&fifo[0], &fifo[1], fifo_count * sizeof(fifo[0]));
	work->handler(work);
	return true;
}

static void run_all(void)
{
	int guard = 0;

	while (run_one()) {
		zassert_true(++guard < 64, "runaway work chain");
	}
}

static bool fifo_holds_only(const struct k_work *work)
{
	return (fifo_count == 1U) && (fifo[0] == work);
}

static size_t submits_of(const struct k_work *work)
{
	size_t n = MIN(k_work_submit_to_queue_fake.call_count, FFF_ARG_HISTORY_LEN);
	size_t hits = 0;

	for (size_t i = 0; i < n; i++) {
		if (k_work_submit_to_queue_fake.arg1_history[i] == work) {
			hits++;
		}
	}
	return hits;
}

/* ---- coordinators under test ---- */

static void s_await(struct zephlet_coord *c);
static void s_after(struct zephlet_coord *c);

ZEPHLET_COORD_ASYNC_DEFINE(coord, ctx, s_await);

static void s_await(struct zephlet_coord *c)
{
	struct unit_ctx *st = c->ctx;

	zephlet_coord_await(c, &chan_test, &st->dst, st->match, s_after, st->timeout);
	st->result_at_arm = zephlet_coord_await_result(c);
}

static void s_after(struct zephlet_coord *c)
{
	struct unit_ctx *st = c->ctx;

	if (st->after_count < (int)ARRAY_SIZE(st->results)) {
		st->results[st->after_count] = zephlet_coord_await_result(c);
	}
	st->after_count++;
	zephlet_coord_done(c);
}

static void s_chain_1(struct zephlet_coord *c);
static void s_chain_2(struct zephlet_coord *c);
static void s_chain_3(struct zephlet_coord *c);
static void s_chain_4(struct zephlet_coord *c);

ZEPHLET_COORD_ASYNC_DEFINE(coord_chain, ctx, s_chain_1);

static void s_chain_1(struct zephlet_coord *c)
{
	struct unit_ctx *st = c->ctx;

	st->results[0] = zephlet_coord_await_result(c);
	zephlet_coord_await(c, &chan_test, &st->dst, NULL, s_chain_2, K_MSEC(100));
}

static void s_chain_2(struct zephlet_coord *c)
{
	struct unit_ctx *st = c->ctx;

	st->results[1] = zephlet_coord_await_result(c);
	zephlet_coord_await(c, &chan_test, &st->dst, NULL, s_chain_3, K_MSEC(100));
}

static void s_chain_3(struct zephlet_coord *c)
{
	struct unit_ctx *st = c->ctx;

	st->results[2] = zephlet_coord_await_result(c);
	zephlet_coord_next(c, s_chain_4);
}

static void s_chain_4(struct zephlet_coord *c)
{
	struct unit_ctx *st = c->ctx;

	st->results[3] = zephlet_coord_await_result(c);
	st->after_count++;
	zephlet_coord_done(c);
}

/* ---- helpers ---- */

static void publish(const struct zbus_observer *obs, int value)
{
	((struct payload *)chan_test.message)->value = value;
	obs->callback(&chan_test);
}

static void fire_timeout(struct zephlet_coord_async *async)
{
	async->timer.work.handler(&async->timer.work);
}

static bool reject_all(const void *msg)
{
	ARG_UNUSED(msg);
	return false;
}

static int event_wins_then_schedule_fails(struct k_work_q *queue, struct k_work_delayable *dwork,
					  k_timeout_t delay)
{
	ARG_UNUSED(queue);
	ARG_UNUSED(dwork);
	ARG_UNUSED(delay);

	publish(&coord_zlet_obs, 42);
	return -EBUSY;
}

static int resolve_wins_then_schedule_fails(struct k_work_q *queue, struct k_work_delayable *dwork,
					    k_timeout_t delay)
{
	ARG_UNUSED(queue);
	ARG_UNUSED(dwork);
	ARG_UNUSED(delay);

	zephlet_coord_resolve(coord);
	return -EBUSY;
}

static int cancel_wins_then_schedule_fails(struct k_work_q *queue, struct k_work_delayable *dwork,
					   k_timeout_t delay)
{
	ARG_UNUSED(queue);
	ARG_UNUSED(dwork);
	ARG_UNUSED(delay);

	zephlet_coord_cancel(coord);
	return -EBUSY;
}

static void reset_async(struct zephlet_coord_async *async)
{
	async->base.current = NULL;
	async->base.kick_pending = false;
	async->base.await_flags = 0U;
	async->chan = NULL;
	async->pending_rm = NULL;
	async->match = NULL;
	async->dst = NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	COORD_UNIT_FAKES_LIST(RESET_FAKE);
	FFF_RESET_HISTORY();
	k_work_submit_to_queue_fake.custom_fake = fifo_submit;
	k_work_schedule_for_queue_fake.return_val = 1;

	fifo_count = 0U;
	memset(&ctx, 0, sizeof(ctx));
	ctx.dst.value = DST_SENTINEL;
	ctx.timeout = K_MSEC(100);
	reset_async(&coord_storage);
	reset_async(&coord_chain_storage);
}

static void after(void *fixture)
{
	ARG_UNUSED(fixture);

	/*
	 * Drop anything a failed test left queued, so it cannot run in the
	 * next test. Passing tests drain the FIFO themselves.
	 */
	fifo_count = 0U;
}

ZTEST_SUITE(coord_unit, NULL, NULL, before, after, NULL);

/* Kick, then run only the await step. */
static void kick_and_arm(void)
{
	zassert_equal(zephlet_coord_kick(coord), 0);
	zassert_true(fifo_holds_only(&coord->work));
	zassert_true(run_one());
}

/* ---- tests ---- */

ZTEST(coord_unit, test_arm_reports_pending_and_cancel_completes)
{
	kick_and_arm();

	zassert_equal(ctx.result_at_arm, ZEPHLET_COORD_AWAIT_FLAG_PENDING);
	zassert_equal(zbus_chan_add_obs_fake.call_count, 1U);
	zassert_equal_ptr(zbus_chan_add_obs_fake.arg0_val, &chan_test);
	zassert_equal_ptr(zbus_chan_add_obs_fake.arg1_val, &coord_zlet_obs);
	zassert_equal(k_work_schedule_for_queue_fake.call_count, 1U);
	zassert_equal_ptr(k_work_schedule_for_queue_fake.arg1_val, &coord_storage.timer);
	zassert_equal(fifo_count, 0U, "armed await queued work");

	zephlet_coord_cancel(coord);
	zassert_true(fifo_holds_only(&coord_storage.cleanup_work));
	zassert_equal(k_work_cancel_delayable_fake.call_count, 1U);
	run_all();

	zassert_equal(ctx.after_count, 1);
	zassert_equal(ctx.results[0], ZEPHLET_COORD_AWAIT_FLAG_CANCELLED);
	zassert_equal(zbus_chan_rm_obs_fake.call_count, 1U);
	zassert_equal_ptr(zbus_chan_rm_obs_fake.arg1_val, &coord_zlet_obs);
	zassert_equal(ctx.dst.value, DST_SENTINEL, "cancel wrote dst");
	zassert_is_null(coord->current);
}

ZTEST(coord_unit, test_attach_failure_runs_continuation_and_replays_kick)
{
	zbus_chan_add_obs_fake.return_val = -EAGAIN;

	kick_and_arm();

	zassert_equal(ctx.result_at_arm, ZEPHLET_COORD_AWAIT_FLAG_ZBUS_FAILED);
	zassert_equal(k_work_schedule_for_queue_fake.call_count, 0U, "timer armed after failure");
	zassert_true(fifo_holds_only(&coord->work), "continuation not queued exactly once");

	/* A kick while the failed flow is still in flight is deferred. */
	zassert_equal(zephlet_coord_kick(coord), -EBUSY);

	run_all();

	zassert_equal(ctx.after_count, 2, "deferred kick not replayed");
	zassert_equal(ctx.results[0], ZEPHLET_COORD_AWAIT_FLAG_ZBUS_FAILED);
	zassert_equal(ctx.results[1], ZEPHLET_COORD_AWAIT_FLAG_ZBUS_FAILED);
	zassert_equal(zbus_chan_rm_obs_fake.call_count, 0U, "removed an observer never attached");
	zassert_equal(k_work_cancel_delayable_fake.call_count, 0U);
	zassert_equal(ctx.dst.value, DST_SENTINEL, "failed arm wrote dst");
	zassert_is_null(coord->current);
}

ZTEST(coord_unit, test_timer_failure_rollback_wins)
{
	k_work_schedule_for_queue_fake.return_val = -EBUSY;

	kick_and_arm();

	zassert_equal(ctx.result_at_arm, ZEPHLET_COORD_AWAIT_FLAG_TIMER_FAILED);
	zassert_equal(zbus_chan_rm_obs_fake.call_count, 1U, "rollback did not detach");
	zassert_equal_ptr(zbus_chan_rm_obs_fake.arg0_val, &chan_test);
	zassert_equal_ptr(zbus_chan_rm_obs_fake.arg1_val, &coord_zlet_obs);
	zassert_true(fifo_holds_only(&coord->work), "continuation not queued exactly once");

	/* A publish after the rollback finds nothing to claim. */
	publish(&coord_zlet_obs, 7);
	zassert_true(fifo_holds_only(&coord->work), "late publish queued work");

	run_all();

	zassert_equal(ctx.after_count, 1);
	zassert_equal(ctx.results[0], ZEPHLET_COORD_AWAIT_FLAG_TIMER_FAILED);
	zassert_equal(ctx.dst.value, DST_SENTINEL, "failed arm wrote dst");
	zassert_equal(k_work_cancel_delayable_fake.call_count, 0U);
	zassert_equal(submits_of(&coord_storage.cleanup_work), 0U);
}

ZTEST(coord_unit, test_timer_failure_keeps_event_that_won)
{
	k_work_schedule_for_queue_fake.custom_fake = event_wins_then_schedule_fails;

	kick_and_arm();

	zassert_equal(ctx.result_at_arm, ZEPHLET_COORD_AWAIT_FLAG_MATCHED,
		      "rollback overwrote the winning outcome");
	zassert_equal(zbus_chan_rm_obs_fake.call_count, 0U, "losing rollback detached");
	zassert_true(fifo_holds_only(&coord_storage.cleanup_work),
		     "losing rollback queued the continuation");

	run_all();

	zassert_equal(ctx.after_count, 1);
	zassert_equal(ctx.results[0], ZEPHLET_COORD_AWAIT_FLAG_MATCHED);
	zassert_equal(ctx.dst.value, 42, "winning event not copied");
	zassert_equal(zbus_chan_rm_obs_fake.call_count, 1U, "winner did not detach once");
	zassert_equal(submits_of(&coord->work), 2U, "kick + one continuation expected");
}

ZTEST(coord_unit, test_timer_failure_keeps_resolve_that_won)
{
	k_work_schedule_for_queue_fake.custom_fake = resolve_wins_then_schedule_fails;

	kick_and_arm();
	zassert_equal(ctx.result_at_arm, ZEPHLET_COORD_AWAIT_FLAG_RESOLVED);
	zassert_true(fifo_holds_only(&coord_storage.cleanup_work));
	run_all();

	zassert_equal(ctx.after_count, 1);
	zassert_equal(ctx.results[0], ZEPHLET_COORD_AWAIT_FLAG_RESOLVED);
	zassert_equal(ctx.dst.value, DST_SENTINEL, "resolve wrote dst");
	zassert_equal(zbus_chan_rm_obs_fake.call_count, 1U);
	zassert_equal(submits_of(&coord->work), 2U, "kick + one continuation expected");
}

ZTEST(coord_unit, test_timer_failure_keeps_cancel_that_won)
{
	k_work_schedule_for_queue_fake.custom_fake = cancel_wins_then_schedule_fails;

	kick_and_arm();
	zassert_equal(ctx.result_at_arm, ZEPHLET_COORD_AWAIT_FLAG_CANCELLED);
	run_all();

	zassert_equal(ctx.after_count, 1);
	zassert_equal(ctx.results[0], ZEPHLET_COORD_AWAIT_FLAG_CANCELLED);
	zassert_equal(ctx.dst.value, DST_SENTINEL, "cancel wrote dst");
	zassert_equal(zbus_chan_rm_obs_fake.call_count, 1U);
	zassert_equal(submits_of(&coord->work), 2U, "kick + one continuation expected");
}

ZTEST(coord_unit, test_event_wins_then_late_timeout_and_event_are_ignored)
{
	kick_and_arm();

	publish(&coord_zlet_obs, 42);
	zassert_true(fifo_holds_only(&coord_storage.cleanup_work));
	zassert_equal(k_work_cancel_delayable_fake.call_count, 1U, "winner did not cancel timer");

	fire_timeout(&coord_storage);
	publish(&coord_zlet_obs, 99);
	zassert_true(fifo_holds_only(&coord_storage.cleanup_work), "a loser queued work");
	zassert_equal(zbus_chan_rm_obs_fake.call_count, 0U, "losing timeout detached");
	zassert_equal(ctx.dst.value, 42, "losing event overwrote dst");

	run_all();

	zassert_equal(ctx.after_count, 1);
	zassert_equal(ctx.results[0], ZEPHLET_COORD_AWAIT_FLAG_MATCHED);
	zassert_equal(zbus_chan_rm_obs_fake.call_count, 1U);
}

ZTEST(coord_unit, test_timeout_wins_then_late_event_and_calls_are_ignored)
{
	kick_and_arm();

	fire_timeout(&coord_storage);
	zassert_equal(zbus_chan_rm_obs_fake.call_count, 1U, "timeout did not detach");
	zassert_true(fifo_holds_only(&coord->work));

	publish(&coord_zlet_obs, 42);
	zephlet_coord_resolve(coord);
	zephlet_coord_cancel(coord);
	zassert_true(fifo_holds_only(&coord->work), "a loser queued work");
	zassert_equal(ctx.dst.value, DST_SENTINEL, "losing event wrote dst");

	run_all();

	zassert_equal(ctx.after_count, 1);
	zassert_equal(ctx.results[0], ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT);

	/* The next kick starts from a clean outcome. */
	zassert_equal(zephlet_coord_kick(coord), 0);
	zassert_equal(zephlet_coord_await_result(coord), 0U);
	zassert_true(run_one());
	zephlet_coord_cancel(coord);
	run_all();
}

ZTEST(coord_unit, test_rejected_publish_leaves_await_armed)
{
	ctx.match = reject_all;

	kick_and_arm();

	publish(&coord_zlet_obs, 42);
	zassert_equal(fifo_count, 0U, "rejected publish queued work");
	zassert_equal(k_work_cancel_delayable_fake.call_count, 0U);
	zassert_equal(ctx.dst.value, DST_SENTINEL, "rejected publish wrote dst");
	zassert_equal(zephlet_coord_await_result(coord), ZEPHLET_COORD_AWAIT_FLAG_PENDING);

	fire_timeout(&coord_storage);
	run_all();

	zassert_equal(ctx.results[0], ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT);
}

ZTEST(coord_unit, test_failed_deferred_removal_still_continues)
{
	zbus_chan_rm_obs_fake.return_val = -EAGAIN;

	kick_and_arm();
	publish(&coord_zlet_obs, 42);
	run_all();

	zassert_equal(zbus_chan_rm_obs_fake.call_count, 1U);
	zassert_equal(ctx.after_count, 1);
	zassert_equal(ctx.results[0], ZEPHLET_COORD_AWAIT_FLAG_MATCHED);
	zassert_equal(ctx.dst.value, 42);
}

ZTEST(coord_unit, test_failed_timeout_removal_still_continues)
{
	zbus_chan_rm_obs_fake.return_val = -EAGAIN;

	kick_and_arm();
	fire_timeout(&coord_storage);
	run_all();

	zassert_equal(zbus_chan_rm_obs_fake.call_count, 1U);
	zassert_equal(ctx.after_count, 1);
	zassert_equal(ctx.results[0], ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT);
	zassert_equal(ctx.dst.value, DST_SENTINEL);
}

ZTEST(coord_unit, test_idle_dispatch_and_empty_cleanup_run_nothing)
{
	coord->work.handler(&coord->work);
	zassert_equal(fifo_count, 0U);

	/* A cleanup with nothing to remove still dispatches; idle runs no step. */
	coord_storage.cleanup_work.handler(&coord_storage.cleanup_work);
	zassert_equal(zbus_chan_rm_obs_fake.call_count, 0U);
	zassert_true(fifo_holds_only(&coord->work));
	run_all();

	zassert_equal(ctx.after_count, 0, "a step ran while idle");
	zassert_is_null(coord->current);
}

ZTEST(coord_unit, test_consecutive_awaits_report_their_own_outcome)
{
	zassert_equal(zephlet_coord_kick(coord_chain), 0);
	zassert_true(run_one()); /* s_chain_1: arms await 1 */

	publish(&coord_chain_zlet_obs, 1);
	run_all(); /* cleanup -> s_chain_2: arms await 2 */

	fire_timeout(&coord_chain_storage);
	run_all(); /* s_chain_3 -> next -> s_chain_4 */

	zassert_equal(ctx.after_count, 1);
	zassert_equal(ctx.results[0], 0U, "kick must report zero");
	zassert_equal(ctx.results[1], ZEPHLET_COORD_AWAIT_FLAG_MATCHED, "await 1");
	zassert_equal(ctx.results[2], ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT, "await 2");
	zassert_equal(ctx.results[3], 0U, "plain next must report zero");
}
