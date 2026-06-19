#ifndef MODULES_ZEPHLETS_SHARED_ZEPHLET_COORD_H
#define MODULES_ZEPHLETS_SHARED_ZEPHLET_COORD_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zephlet Coordinator API
 * @defgroup zephlet_coord_apis Zephlet Coordinator APIs
 * @since 0.1.0
 * @version 0.1.0
 * @{
 */

/**
 * @brief Shared workqueue for every coordinator in the project.
 *
 * Coordinators submit their k_work items here rather than to the system
 * workqueue so long-running steps cannot stall driver work, debouncers,
 * or timer handlers. Started at SYS_INIT(APPLICATION, 0).
 */
extern struct k_work_q zephlet_coord_workq;

struct zephlet_coord;

/**
 * @brief Step callback shape.
 *
 * Every step is a plain function of this type. The dispatcher calls
 * @c c->current(c); the body recovers its typed per-flow state via
 * @c c->ctx and advances via @ref zephlet_coord_next,
 * @ref zephlet_coord_await, or @ref zephlet_coord_done.
 */
typedef void (*zephlet_coord_step_fn)(struct zephlet_coord *c);

/**
 * @brief Event-match predicate shape.
 *
 * Used by @ref zephlet_coord_await to decide whether an incoming
 * channel publish qualifies as the awaited event. Return @c true to
 * resolve the await; @c false to leave it armed for the next publish.
 * Passing @c NULL as the predicate accepts any publish.
 */
typedef bool (*zephlet_coord_match_fn)(const void *msg);

/**
 * @brief Framework-owned coordinator handle.
 *
 * Allocated by @ref ZEPHLET_COORD_DEFINE or
 * @ref ZEPHLET_COORD_ASYNC_DEFINE; never embedded in author code. The
 * @c ctx pointer references a user-defined per-flow state struct whose
 * contents are bounded by one flow (trigger to @ref zephlet_coord_done).
 *
 * @see ZEPHLET_COORD_DEFINE
 * @see ZEPHLET_COORD_ASYNC_DEFINE
 */
struct zephlet_coord {
	/** Generic dispatcher work item; handler is @c zephlet_coord_dispatch.
	 */
	struct k_work work;
	/** Next step to run; NULL means idle. */
	zephlet_coord_step_fn current;
	/** First step kicked off by @ref zephlet_coord_kick. */
	zephlet_coord_step_fn entry;
	/** User-defined per-flow state; opaque to the framework. */
	void *ctx;
	/** A kick arrived while running; @ref zephlet_coord_done re-runs from
	 *  @c entry instead of going idle. */
	bool kick_pending;
	/** Serialises @ref zephlet_coord_kick against @ref zephlet_coord_done. */
	struct k_spinlock lock;
};

/**
 * @brief Async sidecar for coordinators that may suspend on a zbus event.
 *
 * Allocated by @ref ZEPHLET_COORD_ASYNC_DEFINE. The public coordinator
 * pointer aliases @c base; the framework recovers the sidecar from a
 * @c struct zephlet_coord* via CONTAINER_OF inside the await,
 * resolve, and timeout paths.
 *
 * The framework emits a per-coordinator zbus listener at definition
 * time; @c obs is initialised to point at it and never changes. The
 * listener runs @c match against the incoming message, memcpys into
 * @c dst on match, and calls @ref zephlet_coord_resolve.
 *
 * The race between event-resolve and timeout is protected by @c lock
 * and a swap-to-NULL of @c chan: the first path through reads a
 * non-NULL @c chan and clears it; the loser sees NULL and bails.
 */
struct zephlet_coord_async {
	/** Embedded coordinator; public pointer aliases @c &base. */
	struct zephlet_coord base;
	/** Delayable work for the await's bounded wait. */
	struct k_work_delayable timer;
	/** Deferred work that runs @c zbus_chan_rm_obs off the listener call
	 *  stack and then advances the flow.
	 */
	struct k_work cleanup_work;
	/** Protects the chan-claim race. */
	struct k_spinlock lock;
	/** Channel currently awaited; cleared on claim. */
	const struct zbus_channel *chan;
	/** Channel handed to @c cleanup_work for deferred observer removal;
	 *  set on claim, cleared once the obs has been removed.
	 */
	const struct zbus_channel *pending_rm;
	/** Framework-generated observer; set at definition time. */
	const struct zbus_observer *obs;
	/** Match predicate; NULL accepts any publish. */
	zephlet_coord_match_fn match;
	/** Destination for the matched message memcpy; NULL skips the copy. */
	void *dst;
};

/** @cond INTERNAL_HIDDEN */
void zephlet_coord_dispatch(struct k_work *w);
void zephlet_coord_await_timeout(struct k_work *w);
void zephlet_coord_cleanup_dispatch(struct k_work *w);
/** @endcond */

/**
 * @brief Start a flow.
 *
 * Sets @c current to the registered entry step and submits the work
 * item to @c zephlet_coord_workq.
 *
 * The trigger source decides the policy (drop / queue / reject) on
 * @c -EBUSY; the framework imposes none.
 *
 * @warning A trigger source that writes to @c c->ctx before calling
 * this function will corrupt the in-flight flow's state on @c -EBUSY.
 * Peek @c c->current first, mutate @c ctx only on the success path,
 * then call this function as a safety net.
 *
 * @param[in] c The coordinator instance reference.
 *
 * @retval 0       Flow started.
 * @retval -EBUSY  Coordinator is already running.
 */
int zephlet_coord_kick(struct zephlet_coord *c);

/**
 * @brief Queue the next step within an in-flight flow.
 *
 * Sets @c c->current and re-submits the work item to
 * @c zephlet_coord_workq. No re-entry guard — only called from inside
 * a step or an await's resolve/timeout path; the dispatcher is
 * single-threaded per coordinator.
 *
 * @param[in] c  The coordinator instance reference.
 * @param[in] fn The next step to run.
 */
void zephlet_coord_next(struct zephlet_coord *c, zephlet_coord_step_fn fn);

/**
 * @brief Terminate the flow. Marks the coordinator idle.
 *
 * Does NOT reset @c c->ctx. Anything the author allocated, armed, or
 * registered during the flow — timers, dynamic zbus observers, cached
 * pointers, accumulated state — must be torn down by the author
 * before this call. The framework owns @c base and the async sidecar;
 * everything else is yours.
 *
 * @param[in] c The coordinator instance reference.
 */
void zephlet_coord_done(struct zephlet_coord *c);

/**
 * @brief Suspend the flow until a publish matches or the timeout expires.
 *
 * Adds the framework-generated observer to @p chan and arms a
 * delayable work item for @p timeout. Returns immediately; the step
 * exits.
 *
 * On each publish to @p chan while the await is armed, the framework
 * runs @p match against the message. If it returns @c true (or if
 * @p match is @c NULL), the framework memcpys @c zbus_chan_msg_size
 * bytes into @p dst (if non-NULL), cancels the timer, removes the
 * observer, and submits @p next. Non-qualifying events leave the
 * await armed.
 *
 * Distinguishing event-vs-timeout in @p next is the author's job.
 * Recommended idiom: pre-zero @p dst before the await and inspect a
 * naturally-never-zero field on the way out.
 *
 * @warning Requires the coordinator to have been defined with
 * @ref ZEPHLET_COORD_ASYNC_DEFINE. Calling on a sync-only coordinator
 * is a programmer error.
 *
 * @param[in] c       The coordinator instance reference.
 * @param[in] chan    The channel to observe.
 * @param[out] dst    Destination for the matched message memcpy; NULL
 *                    to skip the copy.
 * @param[in] match   Match predicate; NULL accepts any publish.
 * @param[in] next    Step to run on resolve or timeout.
 * @param[in] timeout Bounded wait, or @c K_FOREVER.
 */
void zephlet_coord_await(struct zephlet_coord *c, const struct zbus_channel *chan, void *dst,
			 zephlet_coord_match_fn match, zephlet_coord_step_fn next,
			 k_timeout_t timeout);

/**
 * @brief Finalise the await and advance to the pending step.
 *
 * Normally called by the framework-generated listener emitted by
 * @ref ZEPHLET_COORD_ASYNC_DEFINE; surfaced for hand-rolled resolve
 * paths. Idempotent against the timer-expiry path via the async
 * sidecar's spinlock + swap-chan-to-NULL claim.
 *
 * @param[in] c The coordinator instance reference.
 */
void zephlet_coord_resolve(struct zephlet_coord *c);

/**
 * @brief Define a synchronous coordinator.
 *
 * Emits @p _name as a @c struct zephlet_coord *const pointing at
 * file-scope storage. The per-flow state variable @p _ctx_var must be
 * a static in the same translation unit.
 *
 * @param[in] _name        The coordinator's name.
 * @param[in] _ctx_var     The static per-flow state variable.
 * @param[in] _first_step  The entry step kicked by @ref zephlet_coord_kick.
 */
#define ZEPHLET_COORD_DEFINE(_name, _ctx_var, _first_step)                                         \
	static struct zephlet_coord _name##_storage = {                                            \
		.work = Z_WORK_INITIALIZER(zephlet_coord_dispatch),                                \
		.entry = (_first_step),                                                            \
		.ctx = &(_ctx_var),                                                                \
	};                                                                                         \
	static struct zephlet_coord *const _name = &_name##_storage

/**
 * @brief Define a coordinator that may suspend on a zbus event.
 *
 * Allocates the async sidecar and emits a framework-owned zbus
 * listener (@c <_name>_zlet_obs) whose callback runs the await's
 * @c match predicate, memcpys the matched message into @c dst, and
 * calls @ref zephlet_coord_resolve. The author never writes the
 * listener; per-await behaviour is fully specified by the arguments
 * to @ref zephlet_coord_await.
 *
 * Emits at file scope:
 *  - @c <_name>_storage of type @c struct zephlet_coord_async
 *  - @c <_name> as @c struct zephlet_coord *const (aliasing @c &storage.base)
 *  - @c <_name>_zlet_obs / @c <_name>_zlet_cb (framework listener)
 *
 * @param[in] _name        The coordinator's name.
 * @param[in] _ctx_var     The static per-flow state variable.
 * @param[in] _first_step  The entry step kicked by @ref zephlet_coord_kick.
 */
#define ZEPHLET_COORD_ASYNC_DEFINE(_name, _ctx_var, _first_step)                                   \
	static void _name##_zlet_cb(const struct zbus_channel *chan);                              \
	ZBUS_LISTENER_DEFINE(_name##_zlet_obs, _name##_zlet_cb);                                   \
	static struct zephlet_coord_async _name##_storage = {                                      \
		.base =                                                                            \
			{                                                                          \
				.work = Z_WORK_INITIALIZER(zephlet_coord_dispatch),                \
				.entry = (_first_step),                                            \
				.ctx = &(_ctx_var),                                                \
			},                                                                         \
		.timer = Z_WORK_DELAYABLE_INITIALIZER(zephlet_coord_await_timeout),                \
		.cleanup_work = Z_WORK_INITIALIZER(zephlet_coord_cleanup_dispatch),                \
		.obs = &_name##_zlet_obs,                                                          \
	};                                                                                         \
	static struct zephlet_coord *const _name = &_name##_storage.base;                          \
	static void _name##_zlet_cb(const struct zbus_channel *chan)                               \
	{                                                                                          \
		const void *_msg = zbus_chan_const_msg(chan);                                      \
		if (_name##_storage.match != NULL && !_name##_storage.match(_msg)) {               \
			return;                                                                    \
		}                                                                                  \
		if (_name##_storage.dst != NULL) {                                                 \
			memcpy(_name##_storage.dst, _msg, zbus_chan_msg_size(chan));               \
		}                                                                                  \
		zephlet_coord_resolve(_name);                                                      \
	}

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* MODULES_ZEPHLETS_SHARED_ZEPHLET_COORD_H */
