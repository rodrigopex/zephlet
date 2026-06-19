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
	struct payload received;
	k_timeout_t await_timeout;
	int resume_count;
};

static struct test_ctx ctx;

static K_SEM_DEFINE(done_sem, 0, 1);
static K_SEM_DEFINE(await_armed_sem, 0, 1);
static K_SEM_DEFINE(busy_step_started_sem, 0, 1);
static K_SEM_DEFINE(busy_blocker_sem, 0, 1);

static void s_sync_a(struct zephlet_coord *c);
static void s_sync_b(struct zephlet_coord *c);
static void s_sync_end(struct zephlet_coord *c);

ZEPHLET_COORD_DEFINE(coord_sync, ctx, s_sync_a);

static void s_sync_a(struct zephlet_coord *c)
{
	struct test_ctx *st = c->ctx;

	trace_push(&st->trace, 'A');
	zephlet_coord_next(c, s_sync_b);
}

static void s_sync_b(struct zephlet_coord *c)
{
	struct test_ctx *st = c->ctx;

	trace_push(&st->trace, 'B');
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
	return ctx.match_should_accept;
}

static void s_async_await(struct zephlet_coord *c);
static void s_async_resume(struct zephlet_coord *c);

ZEPHLET_COORD_ASYNC_DEFINE(coord_async, ctx, s_async_await);

static void s_async_await(struct zephlet_coord *c)
{
	struct test_ctx *st = c->ctx;

	zephlet_coord_await(c, &chan_test_events, st->await_dst,
			    st->use_match_fn ? match_predicate : NULL, s_async_resume,
			    st->await_timeout);
	k_sem_give(&await_armed_sem);
}

static void s_async_resume(struct zephlet_coord *c)
{
	struct test_ctx *st = c->ctx;

	st->resume_count++;
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
	k_sem_reset(&busy_step_started_sem);
	k_sem_reset(&busy_blocker_sem);
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
