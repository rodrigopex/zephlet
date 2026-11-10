#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "zephlet_coord.h"

/**
 * @file
 * @brief Tests for the zephlet_coord framework.
 *
 * Three test coordinators (sync / async / busy) cover the public surface
 * declared in zephlet_coord.h. Semaphores synchronise the test thread
 * with the coord_workq thread; no k_sleep-based polling.
 */

struct payload {
	int value;
};

ZBUS_CHAN_DEFINE(chan_test_events, struct payload, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.value = 0));

struct trace {
	char buf[17];
	int len;
};

static inline void trace_push(struct trace *t, char c)
{
	if (t->len < (int)(sizeof(t->buf) - 1)) {
		t->buf[t->len++] = c;
		t->buf[t->len] = '\0';
	}
}

struct test_ctx {
	struct trace trace;
	void *await_dst;
	bool use_match_fn;
	bool match_should_accept;
	bool block_in_match;
	struct payload received;
	k_timeout_t await_timeout;
	int resume_count;
	uint8_t result_at_arm;
	uint8_t result;
	uint8_t chain_results[3];
	uint8_t sync_results[2];
};

static struct test_ctx ctx;

/*
 * The struct layout before await_flags was added. The flags byte must
 * land in the padding after kick_pending, leaving the size unchanged.
 */
struct coord_layout_before {
	struct k_work work;
	zephlet_coord_step_fn current;
	zephlet_coord_step_fn entry;
	void *ctx;
	bool kick_pending;
	struct k_spinlock lock;
};

BUILD_ASSERT(sizeof(struct zephlet_coord) == sizeof(struct coord_layout_before),
	     "await_flags must fit in existing padding");

static K_SEM_DEFINE(done_sem, 0, 1);
static K_SEM_DEFINE(await_armed_sem, 0, 1);
static K_SEM_DEFINE(chain_armed_sem, 0, 3);
static K_SEM_DEFINE(busy_step_started_sem, 0, 1);
static K_SEM_DEFINE(busy_blocker_sem, 0, 1);
static K_SEM_DEFINE(in_match_sem, 0, 1);
static K_SEM_DEFINE(release_match_sem, 0, 1);
static K_SEM_DEFINE(pub_done_sem, 0, 1);

static void s_sync_a(struct zephlet_coord *c);
static void s_sync_b(struct zephlet_coord *c);
static void s_sync_end(struct zephlet_coord *c);

ZEPHLET_COORD_DEFINE(coord_sync, ctx, s_sync_a);

static void s_sync_a(struct zephlet_coord *c)
{
	struct test_ctx *st = c->ctx;

	trace_push(&st->trace, 'A');
	st->sync_results[0] = zephlet_coord_await_result(c);
	zephlet_coord_next(c, s_sync_b);
}

static void s_sync_b(struct zephlet_coord *c)
{
	struct test_ctx *st = c->ctx;

	trace_push(&st->trace, 'B');
	st->sync_results[1] = zephlet_coord_await_result(c);
	zephlet_coord_next(c, s_sync_end);
}

static void s_sync_end(struct zephlet_coord *c)
{
	struct test_ctx *st = c->ctx;

	trace_push(&st->trace, 'E');
	zephlet_coord_done(c);
	k_sem_give(&done_sem);
}

static bool match_predicate(const void *msg)
{
	ARG_UNUSED(msg);

	if (ctx.block_in_match) {
		/* Hold the publisher inside the listener so the timeout can win. */
		k_sem_give(&in_match_sem);
		k_sem_take(&release_match_sem, K_FOREVER);
	}
	return ctx.match_should_accept;
}

/* Publishes from the system workqueue, off the test thread. */
static void publish_work_handler(struct k_work *w)
{
	struct payload msg = {.value = 7};

	ARG_UNUSED(w);
	zbus_chan_pub(&chan_test_events, &msg, K_FOREVER);
	k_sem_give(&pub_done_sem);
}

static K_WORK_DEFINE(publish_work, publish_work_handler);

static void s_async_await(struct zephlet_coord *c);
static void s_async_resume(struct zephlet_coord *c);

ZEPHLET_COORD_ASYNC_DEFINE(coord_async, ctx, s_async_await);

static void s_async_await(struct zephlet_coord *c)
{
	struct test_ctx *st = c->ctx;

	zephlet_coord_await(c, &chan_test_events, st->await_dst,
			    st->use_match_fn ? match_predicate : NULL, s_async_resume,
			    st->await_timeout);
	st->result_at_arm = zephlet_coord_await_result(c);
	k_sem_give(&await_armed_sem);
}

static void s_async_resume(struct zephlet_coord *c)
{
	struct test_ctx *st = c->ctx;

	st->resume_count++;
	st->result = zephlet_coord_await_result(c);
	zephlet_coord_done(c);
	k_sem_give(&done_sem);
}

/* Three awaits in one flow: event, timeout, event. */
static void s_chain_1(struct zephlet_coord *c);
static void s_chain_2(struct zephlet_coord *c);
static void s_chain_3(struct zephlet_coord *c);
static void s_chain_end(struct zephlet_coord *c);

ZEPHLET_COORD_ASYNC_DEFINE(coord_chain, ctx, s_chain_1);

static void s_chain_1(struct zephlet_coord *c)
{
	zephlet_coord_await(c, &chan_test_events, NULL, NULL, s_chain_2, K_MSEC(500));
	k_sem_give(&chain_armed_sem);
}

static void s_chain_2(struct zephlet_coord *c)
{
	struct test_ctx *st = c->ctx;

	st->chain_results[0] = zephlet_coord_await_result(c);
	zephlet_coord_await(c, &chan_test_events, NULL, NULL, s_chain_3, K_MSEC(30));
	k_sem_give(&chain_armed_sem);
}

static void s_chain_3(struct zephlet_coord *c)
{
	struct test_ctx *st = c->ctx;

	st->chain_results[1] = zephlet_coord_await_result(c);
	zephlet_coord_await(c, &chan_test_events, NULL, NULL, s_chain_end, K_MSEC(500));
	k_sem_give(&chain_armed_sem);
}

static void s_chain_end(struct zephlet_coord *c)
{
	struct test_ctx *st = c->ctx;

	st->chain_results[2] = zephlet_coord_await_result(c);
	zephlet_coord_done(c);
	k_sem_give(&done_sem);
}

static void s_busy_blocker(struct zephlet_coord *c);

ZEPHLET_COORD_DEFINE(coord_busy, ctx, s_busy_blocker);

static void s_busy_blocker(struct zephlet_coord *c)
{
	k_sem_give(&busy_step_started_sem);
	k_sem_take(&busy_blocker_sem, K_FOREVER);
	zephlet_coord_done(c);
	k_sem_give(&done_sem);
}

static void reset(void *fixture)
{
	ARG_UNUSED(fixture);

	memset(&ctx, 0, sizeof(ctx));
	ctx.await_timeout = K_MSEC(50);
	k_sem_reset(&done_sem);
	k_sem_reset(&await_armed_sem);
	k_sem_reset(&chain_armed_sem);
	k_sem_reset(&busy_step_started_sem);
	k_sem_reset(&busy_blocker_sem);
	k_sem_reset(&in_match_sem);
	k_sem_reset(&release_match_sem);
	k_sem_reset(&pub_done_sem);
}

ZTEST_SUITE(zephlet_coord, NULL, NULL, reset, NULL, NULL);

ZTEST(zephlet_coord, test_sync_chain)
{
	int err = zephlet_coord_kick(coord_sync);

	zassert_equal(err, 0, "kick err=%d", err);

	err = k_sem_take(&done_sem, K_MSEC(500));
	zassert_equal(err, 0, "sync flow did not complete");

	zassert_str_equal(ctx.trace.buf, "ABE", "trace mismatch");
	zassert_is_null(coord_sync->current, "coord did not return to idle");
	zassert_equal(ctx.sync_results[0], 0U, "kick result=0x%02x", ctx.sync_results[0]);
	zassert_equal(ctx.sync_results[1], 0U, "next result=0x%02x", ctx.sync_results[1]);

	err = zephlet_coord_kick(coord_sync);
	zassert_equal(err, 0, "re-kick rejected: %d", err);
	err = k_sem_take(&done_sem, K_MSEC(500));
	zassert_equal(err, 0, "second flow did not complete");
	zassert_equal(ctx.trace.len, 6, "second chain did not run");
}

ZTEST(zephlet_coord, test_kick_busy_returns_ebusy)
{
	int err = zephlet_coord_kick(coord_busy);

	zassert_equal(err, 0);

	err = k_sem_take(&busy_step_started_sem, K_MSEC(500));
	zassert_equal(err, 0, "busy step never started");

	err = zephlet_coord_kick(coord_busy);
	zassert_equal(err, -EBUSY, "second kick should -EBUSY, got %d", err);

	k_sem_give(&busy_blocker_sem);
	err = k_sem_take(&done_sem, K_MSEC(500));
	zassert_equal(err, 0, "busy flow did not unblock");

	/* the busy-time kick is deferred, not dropped: done re-runs once */
	err = k_sem_take(&busy_step_started_sem, K_MSEC(500));
	zassert_equal(err, 0, "deferred kick did not re-run the flow");
	k_sem_give(&busy_blocker_sem);
	err = k_sem_take(&done_sem, K_MSEC(500));
	zassert_equal(err, 0, "deferred flow did not complete");
}

ZTEST(zephlet_coord, test_await_accept_any_publish)
{
	ctx.await_dst = &ctx.received;
	ctx.use_match_fn = false;
	ctx.await_timeout = K_MSEC(500);

	int err = zephlet_coord_kick(coord_async);

	zassert_equal(err, 0);
	zassert_equal(k_sem_take(&await_armed_sem, K_MSEC(500)), 0, "await never armed");

	struct payload msg = {.value = 42};

	zbus_chan_pub(&chan_test_events, &msg, K_NO_WAIT);

	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0, "resume did not run");
	zassert_equal(ctx.received.value, 42, "memcpy missed payload");
	zassert_equal(ctx.resume_count, 1);
	zassert_equal(ctx.result_at_arm, ZEPHLET_COORD_AWAIT_FLAG_PENDING, "armed result=0x%02x",
		      ctx.result_at_arm);
	zassert_equal(ctx.result, ZEPHLET_COORD_AWAIT_FLAG_MATCHED, "event flags=0x%02x",
		      ctx.result);
}

ZTEST(zephlet_coord, test_await_predicate_filters_non_qualifying)
{
	ctx.await_dst = &ctx.received;
	ctx.use_match_fn = true;
	ctx.match_should_accept = false;
	ctx.await_timeout = K_MSEC(500);

	int err = zephlet_coord_kick(coord_async);

	zassert_equal(err, 0);
	zassert_equal(k_sem_take(&await_armed_sem, K_MSEC(500)), 0);

	struct payload msg1 = {.value = 1};

	zbus_chan_pub(&chan_test_events, &msg1, K_NO_WAIT);
	k_msleep(10);
	zassert_equal(ctx.resume_count, 0, "non-qualifying event triggered resume");
	zassert_equal(ctx.received.value, 0, "non-qualifying event was memcpy'd");

	ctx.match_should_accept = true;

	struct payload msg2 = {.value = 99};

	zbus_chan_pub(&chan_test_events, &msg2, K_NO_WAIT);

	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0, "qualifying event did not resume");
	zassert_equal(ctx.resume_count, 1);
	zassert_equal(ctx.received.value, 99);
}

ZTEST(zephlet_coord, test_await_all_non_qualifying_then_timeout)
{
	ctx.await_dst = &ctx.received;
	ctx.use_match_fn = true;
	ctx.match_should_accept = false;
	ctx.await_timeout = K_MSEC(50);

	int err = zephlet_coord_kick(coord_async);

	zassert_equal(err, 0);
	zassert_equal(k_sem_take(&await_armed_sem, K_MSEC(500)), 0);

	for (int i = 0; i < 3; i++) {
		struct payload msg = {.value = i + 1};

		zbus_chan_pub(&chan_test_events, &msg, K_NO_WAIT);
	}

	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0, "timeout did not resume");
	zassert_equal(ctx.received.value, 0, "dst should be untouched");
	zassert_equal(ctx.resume_count, 1);
	zassert_equal(ctx.result, ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT, "result=0x%02x", ctx.result);
}

ZTEST(zephlet_coord, test_await_no_publish_timeout)
{
	ctx.await_dst = &ctx.received;
	ctx.use_match_fn = false;
	ctx.await_timeout = K_MSEC(50);

	int err = zephlet_coord_kick(coord_async);

	zassert_equal(err, 0);
	zassert_equal(k_sem_take(&await_armed_sem, K_MSEC(500)), 0);
	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0, "timeout did not resume");
	zassert_equal(ctx.received.value, 0);
	zassert_equal(ctx.resume_count, 1);
	zassert_equal(ctx.result, ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT, "result=0x%02x", ctx.result);
}

ZTEST(zephlet_coord, test_await_dst_null_no_copy)
{
	ctx.await_dst = NULL;
	ctx.use_match_fn = false;
	ctx.await_timeout = K_MSEC(500);

	int err = zephlet_coord_kick(coord_async);

	zassert_equal(err, 0);
	zassert_equal(k_sem_take(&await_armed_sem, K_MSEC(500)), 0);

	struct payload msg = {.value = 42};

	zbus_chan_pub(&chan_test_events, &msg, K_NO_WAIT);

	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0);
	zassert_equal(ctx.received.value, 0, "ctx.received changed despite dst=NULL");
	zassert_equal(ctx.resume_count, 1);
}

ZTEST(zephlet_coord, test_resolve_idempotent_after_event)
{
	ctx.await_dst = &ctx.received;
	ctx.use_match_fn = false;
	ctx.await_timeout = K_MSEC(500);

	int err = zephlet_coord_kick(coord_async);

	zassert_equal(err, 0);
	zassert_equal(k_sem_take(&await_armed_sem, K_MSEC(500)), 0);

	struct payload msg = {.value = 42};

	zbus_chan_pub(&chan_test_events, &msg, K_NO_WAIT);
	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0);
	zassert_equal(ctx.resume_count, 1);

	zephlet_coord_resolve(coord_async);
	k_msleep(50);
	zassert_equal(ctx.resume_count, 1, "extra resolve after event caused double-resume");
}

ZTEST(zephlet_coord, test_resolve_idempotent_after_timeout)
{
	ctx.await_dst = &ctx.received;
	ctx.use_match_fn = false;
	ctx.await_timeout = K_MSEC(50);

	int err = zephlet_coord_kick(coord_async);

	zassert_equal(err, 0);
	zassert_equal(k_sem_take(&await_armed_sem, K_MSEC(500)), 0);
	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0);
	zassert_equal(ctx.resume_count, 1);

	struct payload msg = {.value = 42};

	zbus_chan_pub(&chan_test_events, &msg, K_NO_WAIT);
	k_msleep(50);
	zassert_equal(ctx.resume_count, 1, "publish after timeout triggered resume");
	zassert_equal(ctx.received.value, 0, "publish after timeout was memcpy'd");
}

ZTEST(zephlet_coord, test_manual_resolve_reports_success)
{
	ctx.await_dst = &ctx.received;
	ctx.await_timeout = K_MSEC(500);

	zassert_equal(zephlet_coord_kick(coord_async), 0);
	zassert_equal(k_sem_take(&await_armed_sem, K_MSEC(500)), 0);

	zephlet_coord_resolve(coord_async);

	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0, "resolve did not resume");
	zassert_equal(ctx.received.value, 0, "resolve must not copy into dst");
	zassert_equal(ctx.result, ZEPHLET_COORD_AWAIT_FLAG_RESOLVED,
		      "resolve must be distinguishable from a match: flags=0x%02x", ctx.result);
	zassert_equal(ctx.resume_count, 1);
}

ZTEST(zephlet_coord, test_cancel_reports_ecanceled)
{
	ctx.await_dst = &ctx.received;
	ctx.await_timeout = K_MSEC(500);

	zassert_equal(zephlet_coord_kick(coord_async), 0);
	zassert_equal(k_sem_take(&await_armed_sem, K_MSEC(500)), 0);

	zephlet_coord_cancel(coord_async);

	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0, "cancel did not resume");
	zassert_equal(ctx.received.value, 0, "cancel must not copy into dst");
	zassert_equal(ctx.result, ZEPHLET_COORD_AWAIT_FLAG_CANCELLED, "flags=0x%02x", ctx.result);
	zassert_equal(ctx.resume_count, 1);

	/* A publish after the cancel is not observed. */
	struct payload msg = {.value = 42};

	zbus_chan_pub(&chan_test_events, &msg, K_NO_WAIT);
	k_msleep(50);
	zassert_equal(ctx.resume_count, 1, "publish after cancel triggered resume");
	zassert_equal(ctx.received.value, 0, "publish after cancel was memcpy'd");
}

ZTEST(zephlet_coord, test_cancel_after_timeout_is_noop)
{
	ctx.await_dst = &ctx.received;
	ctx.await_timeout = K_MSEC(50);

	zassert_equal(zephlet_coord_kick(coord_async), 0);
	zassert_equal(k_sem_take(&await_armed_sem, K_MSEC(500)), 0);
	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0);
	zassert_equal(ctx.result, ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT);

	zephlet_coord_cancel(coord_async);
	k_msleep(50);
	zassert_equal(ctx.resume_count, 1, "cancel after timeout caused double-resume");
}

ZTEST(zephlet_coord, test_consecutive_awaits_report_their_own_result)
{
	struct payload msg = {.value = 1};

	zassert_equal(zephlet_coord_kick(coord_chain), 0);

	zassert_equal(k_sem_take(&chain_armed_sem, K_MSEC(500)), 0, "await 1 not armed");
	zbus_chan_pub(&chan_test_events, &msg, K_NO_WAIT);

	/* await 2 is left to time out */
	zassert_equal(k_sem_take(&chain_armed_sem, K_MSEC(500)), 0, "await 2 not armed");

	zassert_equal(k_sem_take(&chain_armed_sem, K_MSEC(500)), 0, "await 3 not armed");
	zbus_chan_pub(&chan_test_events, &msg, K_NO_WAIT);

	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0, "chain did not finish");
	zassert_equal(ctx.chain_results[0], ZEPHLET_COORD_AWAIT_FLAG_MATCHED,
		      "await 1 result=0x%02x", ctx.chain_results[0]);
	zassert_equal(ctx.chain_results[1], ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT,
		      "await 2 result=0x%02x", ctx.chain_results[1]);
	zassert_equal(ctx.chain_results[2], ZEPHLET_COORD_AWAIT_FLAG_MATCHED,
		      "await 3 result=0x%02x", ctx.chain_results[2]);
}

ZTEST(zephlet_coord, test_event_losing_to_timeout_leaves_dst_untouched)
{
	ctx.await_dst = &ctx.received;
	ctx.use_match_fn = true;
	ctx.match_should_accept = true;
	ctx.block_in_match = true;
	ctx.await_timeout = K_MSEC(20);

	zassert_equal(zephlet_coord_kick(coord_async), 0);
	zassert_equal(k_sem_take(&await_armed_sem, K_MSEC(500)), 0);

	/* The publisher enters the listener and blocks inside match. */
	k_work_submit(&publish_work);
	zassert_equal(k_sem_take(&in_match_sem, K_MSEC(500)), 0, "listener never ran");

	/* Wait for the timeout to claim the await, then let the listener go. */
	for (int i = 0; i < 500 && zephlet_coord_await_result(coord_async) !=
					   ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT;
	     i++) {
		k_msleep(1);
	}
	zassert_equal(zephlet_coord_await_result(coord_async), ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT,
		      "timeout never claimed");
	ctx.block_in_match = false;
	k_sem_give(&release_match_sem);

	zassert_equal(k_sem_take(&pub_done_sem, K_MSEC(500)), 0, "publisher stuck");
	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0, "timeout did not resume");
	zassert_equal(ctx.received.value, 0, "losing event was memcpy'd into dst");
	zassert_equal(ctx.result, ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT, "flags=0x%02x", ctx.result);
	zassert_equal(ctx.resume_count, 1, "resumed more than once");
}

ZTEST(zephlet_coord, test_arm_failure_runs_next_with_zbus_failed)
{
	ctx.await_dst = &ctx.received;
	ctx.await_timeout = K_MSEC(500);

	/*
	 * Hold the channel past CONFIG_ZEPHLETS_COORD_ASYNC_ZBUS_TIMEOUT so
	 * zbus_chan_add_obs() times out inside the await.
	 */
	zassert_equal(zbus_chan_claim(&chan_test_events, K_NO_WAIT), 0, "could not hold channel");

	zassert_equal(zephlet_coord_kick(coord_async), 0);
	/* A kick while the flow is running is deferred, not lost by the failure. */
	zassert_equal(zephlet_coord_kick(coord_async), -EBUSY);

	zassert_equal(k_sem_take(&await_armed_sem, K_MSEC(500)), 0, "await step never ran");

	zassert_equal(ctx.result_at_arm, ZEPHLET_COORD_AWAIT_FLAG_ZBUS_FAILED,
		      "status after failed arm=0x%02x", ctx.result_at_arm);

	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0, "next did not run after failed arm");
	zassert_equal(ctx.result, ZEPHLET_COORD_AWAIT_FLAG_ZBUS_FAILED, "flags=0x%02x", ctx.result);

	/* done() honoured the deferred kick: the flow ran again and failed again. */
	zassert_equal(k_sem_take(&done_sem, K_MSEC(500)), 0, "deferred kick did not re-run");
	zassert_equal(ctx.resume_count, 2, "resume_count=%d", ctx.resume_count);

	zassert_equal(zbus_chan_finish(&chan_test_events), 0);

	zassert_equal(ctx.received.value, 0, "dst touched by a failed arm");
	zassert_is_null(coord_async->current, "coord not idle after the flow ended");

	/* The observer was never attached: a publish now resumes nothing. */
	struct payload msg = {.value = 42};

	zbus_chan_pub(&chan_test_events, &msg, K_NO_WAIT);
	k_msleep(50);
	zassert_equal(ctx.resume_count, 2, "publish after failed arm resumed the flow");
}
