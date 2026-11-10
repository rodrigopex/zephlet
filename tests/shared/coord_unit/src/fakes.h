#ifndef ZEPHLET_TESTS_COORD_UNIT_FAKES_H
#define ZEPHLET_TESTS_COORD_UNIT_FAKES_H

#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

/*
 * FFF fakes for every external function zephlet_coord.c links against.
 * The real implementations (kernel/work.c, subsys/zbus, logging) are not
 * part of this test binary.
 */

DECLARE_FAKE_VALUE_FUNC(int, k_work_submit_to_queue, struct k_work_q *, struct k_work *);
DECLARE_FAKE_VALUE_FUNC(int, k_work_schedule_for_queue, struct k_work_q *,
			struct k_work_delayable *, k_timeout_t);
DECLARE_FAKE_VALUE_FUNC(int, k_work_cancel_delayable, struct k_work_delayable *);
DECLARE_FAKE_VOID_FUNC(k_work_queue_init, struct k_work_q *);
DECLARE_FAKE_VOID_FUNC(k_work_queue_start, struct k_work_q *, k_thread_stack_t *, size_t, int,
		       const struct k_work_queue_config *);
DECLARE_FAKE_VALUE_FUNC(int, zbus_chan_add_obs, const struct zbus_channel *,
			const struct zbus_observer *, k_timeout_t);
DECLARE_FAKE_VALUE_FUNC(int, zbus_chan_rm_obs, const struct zbus_channel *,
			const struct zbus_observer *, k_timeout_t);
DECLARE_FAKE_VOID_FUNC_VARARG(z_log_minimal_printk, const char *, ...);

#define COORD_UNIT_FAKES_LIST(FAKE)                                                                \
	FAKE(k_work_submit_to_queue)                                                               \
	FAKE(k_work_schedule_for_queue)                                                            \
	FAKE(k_work_cancel_delayable)                                                              \
	FAKE(k_work_queue_init)                                                                    \
	FAKE(k_work_queue_start)                                                                   \
	FAKE(zbus_chan_add_obs)                                                                    \
	FAKE(zbus_chan_rm_obs)                                                                     \
	FAKE(z_log_minimal_printk)

#endif /* ZEPHLET_TESTS_COORD_UNIT_FAKES_H */
