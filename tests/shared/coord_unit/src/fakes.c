#include <zephyr/logging/log.h>

#include "fakes.h"

DEFINE_FAKE_VALUE_FUNC(int, k_work_submit_to_queue, struct k_work_q *, struct k_work *);
DEFINE_FAKE_VALUE_FUNC(int, k_work_schedule_for_queue, struct k_work_q *, struct k_work_delayable *,
		       k_timeout_t);
DEFINE_FAKE_VALUE_FUNC(int, k_work_cancel_delayable, struct k_work_delayable *);
DEFINE_FAKE_VOID_FUNC(k_work_queue_init, struct k_work_q *);
DEFINE_FAKE_VOID_FUNC(k_work_queue_start, struct k_work_q *, k_thread_stack_t *, size_t, int,
		      const struct k_work_queue_config *);
DEFINE_FAKE_VALUE_FUNC(int, zbus_chan_add_obs, const struct zbus_channel *,
		       const struct zbus_observer *, k_timeout_t);
DEFINE_FAKE_VALUE_FUNC(int, zbus_chan_rm_obs, const struct zbus_channel *,
		       const struct zbus_observer *, k_timeout_t);
DEFINE_FAKE_VOID_FUNC_VARARG(z_log_minimal_printk, const char *, ...);

/*
 * zephlet.c normally registers the "zephlet" log module that
 * zephlet_coord.c declares; it is not part of this binary.
 */
LOG_MODULE_REGISTER(zephlet, CONFIG_ZEPHLET_LOG_LEVEL);
