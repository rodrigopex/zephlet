#include <errno.h>
#include <inttypes.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>

#include "zephlet_coord.h"

LOG_MODULE_DECLARE(zephlet, CONFIG_ZEPHLET_LOG_LEVEL);

struct k_work_q zephlet_coord_workq;

static K_THREAD_STACK_DEFINE(zephlet_coord_stack, CONFIG_ZEPHLETS_COORD_STACK_SIZE);

static const char *get_chan_name(const struct zbus_channel *chan)
{
#if defined(CONFIG_ZBUS_CHANNEL_NAME)
	return zbus_chan_name(chan);
#else
	ARG_UNUSED(chan);
	return "<unnamed>";
#endif
}

static int zephlet_coord_workq_init(void)
{
	const struct k_work_queue_config cfg = {
		.name = "zephlet_coord_wq",
	};

	k_work_queue_init(&zephlet_coord_workq);
	k_work_queue_start(&zephlet_coord_workq, zephlet_coord_stack,
			   K_THREAD_STACK_SIZEOF(zephlet_coord_stack),
			   CONFIG_ZEPHLETS_COORD_PRIORITY, &cfg);
	LOG_DBG("workqueue started: prio=%d stack=%zu", CONFIG_ZEPHLETS_COORD_PRIORITY,
		K_THREAD_STACK_SIZEOF(zephlet_coord_stack));
	return 0;
}

SYS_INIT(zephlet_coord_workq_init, APPLICATION, 0);

void zephlet_coord_dispatch(struct k_work *w)
{
	struct zephlet_coord *c = CONTAINER_OF(w, struct zephlet_coord, work);

	LOG_DBG("dispatch %p step=%p", c, c->current);
	if (c->current != NULL) {
		c->current(c);
	}
}

int zephlet_coord_kick(struct zephlet_coord *c)
{
	k_spinlock_key_t key = k_spin_lock(&c->lock);

	if (c->current != NULL) {
		c->kick_pending = true;
		k_spin_unlock(&c->lock, key);
		LOG_DBG("kick %p deferred: already running", c);
		return -EBUSY;
	}
	c->current = c->entry;
	c->await_flags = 0U;
	k_spin_unlock(&c->lock, key);

	k_work_submit_to_queue(&zephlet_coord_workq, &c->work);
	LOG_DBG("kick %p entry=%p", c, c->entry);
	return 0;
}

void zephlet_coord_next(struct zephlet_coord *c, zephlet_coord_step_fn fn)
{
	c->current = fn;
	c->await_flags = 0U;
	k_work_submit_to_queue(&zephlet_coord_workq, &c->work);
	LOG_DBG("next %p step=%p", c, fn);
}

void zephlet_coord_done(struct zephlet_coord *c)
{
	k_spinlock_key_t key = k_spin_lock(&c->lock);

	if (c->kick_pending) {
		c->kick_pending = false;
		c->current = c->entry;
		c->await_flags = 0U;
		k_spin_unlock(&c->lock, key);

		k_work_submit_to_queue(&zephlet_coord_workq, &c->work);
		LOG_DBG("done %p: re-running for deferred kick", c);
		return;
	}
	c->current = NULL;
	k_spin_unlock(&c->lock, key);
	LOG_DBG("done %p", c);
}

uint8_t zephlet_coord_await_result(const struct zephlet_coord *c)
{
	return c->await_flags;
}

/*
 * Claim the armed await for one path and record its outcome. The
 * winner reads a non-NULL chan, clears it, and writes the outcome under
 * the lock; every later path sees NULL and must bail. The outcome is in
 * place before the winner queues the next step.
 */
static const struct zbus_channel *zephlet_coord_async_claim(struct zephlet_coord_async *async,
							    uint8_t outcome)
{
	const struct zbus_channel *chan = NULL;

	K_SPINLOCK(&async->lock) {
		if (async->chan != NULL) {
			chan = async->chan;
			async->chan = NULL;
			async->base.await_flags = outcome;
		}
	}
	return chan;
}

void zephlet_coord_await(struct zephlet_coord *c, const struct zbus_channel *chan, void *dst,
			 zephlet_coord_match_fn match, zephlet_coord_step_fn next,
			 k_timeout_t timeout)
{
	struct zephlet_coord_async *async = CONTAINER_OF(c, struct zephlet_coord_async, base);
	int err;

	c->current = next;
	c->await_flags = ZEPHLET_COORD_AWAIT_FLAG_PENDING;

	/*
	 * dst and match are in place before the observer is attached. chan
	 * is set only after: a publish in between fails its claim and is
	 * ignored, as if it had arrived before the await, and a resolve or
	 * cancel cannot claim an await whose observer is not attached yet.
	 */
	async->dst = dst;
	async->match = match;

	err = zbus_chan_add_obs(chan, async->obs, K_MSEC(CONFIG_ZEPHLETS_COORD_ASYNC_ZBUS_TIMEOUT));
	if (err < 0) {
		LOG_ERR("zbus_chan_add_obs failed on %s: %d", get_chan_name(chan), err);
		/* Nothing is armed, so no other path can claim: run next with the failure. */
		c->await_flags = ZEPHLET_COORD_AWAIT_FLAG_ZBUS_FAILED;
		k_work_submit_to_queue(&zephlet_coord_workq, &c->work);
		return;
	}

	K_SPINLOCK(&async->lock) {
		async->chan = chan;
	}

	err = k_work_schedule_for_queue(&zephlet_coord_workq, &async->timer, timeout);
	if (err < 0) {
		LOG_ERR("k_work_schedule_for_queue failed: %d", err);
		if (zephlet_coord_async_claim(async, ZEPHLET_COORD_AWAIT_FLAG_TIMER_FAILED) ==
		    NULL) {
			/* An event, resolve, or cancel won first and already queued next. */
			return;
		}
		(void)zbus_chan_rm_obs(chan, async->obs,
				       K_MSEC(CONFIG_ZEPHLETS_COORD_ASYNC_ZBUS_TIMEOUT));
		k_work_submit_to_queue(&zephlet_coord_workq, &c->work);
		return;
	}

	/* k_ticks_t is int64_t or uint32_t depending on CONFIG_TIMEOUT_64BIT. */
	LOG_DBG("armed %p: chan=%s timeout=%" PRId64, c, get_chan_name(chan),
		(int64_t)timeout.ticks);
}

void zephlet_coord_cleanup_dispatch(struct k_work *w)
{
	struct zephlet_coord_async *async =
		CONTAINER_OF(w, struct zephlet_coord_async, cleanup_work);
	int err;

	if (async->pending_rm != NULL) {
		err = zbus_chan_rm_obs(async->pending_rm, async->obs,
				       K_MSEC(CONFIG_ZEPHLETS_COORD_ASYNC_ZBUS_TIMEOUT));
		if (err < 0) {
			LOG_ERR("zbus_chan_rm_obs (deferred) failed on %s: %d",
				get_chan_name(async->pending_rm), err);
		}
		async->pending_rm = NULL;
	}

	k_work_submit_to_queue(&zephlet_coord_workq, &async->base.work);
}

bool zephlet_coord_claim_event(struct zephlet_coord *c)
{
	struct zephlet_coord_async *async = CONTAINER_OF(c, struct zephlet_coord_async, base);
	const struct zbus_channel *chan =
		zephlet_coord_async_claim(async, ZEPHLET_COORD_AWAIT_FLAG_MATCHED);

	if (chan == NULL) {
		LOG_DBG("event %p: already claimed", c);
		return false;
	}

	async->pending_rm = chan;
	return true;
}

void zephlet_coord_finish_event(struct zephlet_coord *c)
{
	struct zephlet_coord_async *async = CONTAINER_OF(c, struct zephlet_coord_async, base);

	LOG_DBG("resolved %p: flags=0x%02x", c, c->await_flags);

	k_work_cancel_delayable(&async->timer);
	k_work_submit_to_queue(&zephlet_coord_workq, &async->cleanup_work);
}

static void zephlet_coord_end_await(struct zephlet_coord *c, uint8_t outcome)
{
	struct zephlet_coord_async *async = CONTAINER_OF(c, struct zephlet_coord_async, base);
	const struct zbus_channel *chan = zephlet_coord_async_claim(async, outcome);

	if (chan == NULL) {
		LOG_DBG("end %p: already claimed", c);
		return;
	}

	async->pending_rm = chan;
	zephlet_coord_finish_event(c);
}

void zephlet_coord_resolve(struct zephlet_coord *c)
{
	zephlet_coord_end_await(c, ZEPHLET_COORD_AWAIT_FLAG_RESOLVED);
}

void zephlet_coord_cancel(struct zephlet_coord *c)
{
	zephlet_coord_end_await(c, ZEPHLET_COORD_AWAIT_FLAG_CANCELLED);
}

void zephlet_coord_await_timeout(struct k_work *w)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(w);
	struct zephlet_coord_async *async = CONTAINER_OF(dwork, struct zephlet_coord_async, timer);
	const struct zbus_channel *chan =
		zephlet_coord_async_claim(async, ZEPHLET_COORD_AWAIT_FLAG_TIMED_OUT);
	int err;

	if (chan == NULL) {
		LOG_DBG("timeout %p: already claimed", &async->base);
		return;
	}

	err = zbus_chan_rm_obs(chan, async->obs, K_MSEC(CONFIG_ZEPHLETS_COORD_ASYNC_ZBUS_TIMEOUT));
	if (err < 0) {
		LOG_ERR("zbus_chan_rm_obs failed on %s: %d", get_chan_name(chan), err);
	}

	k_work_submit_to_queue(&zephlet_coord_workq, &async->base.work);

	LOG_DBG("resolved %p on timeout", &async->base);
}
