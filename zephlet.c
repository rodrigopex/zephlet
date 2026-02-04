#include "zephlet.h"

#include <zephyr/kernel.h>

int zephlets_init_fn(void)
{
	printk("Init zephlets:\n");

	STRUCT_SECTION_FOREACH(zephlet, instance) {
		printk("%p: %s initialing...\n", instance, instance->name);
		if (instance->init_fn != NULL) {
			instance->init_fn(instance);
		}
	}

	return 0;
}

SYS_INIT(zephlets_init_fn, APPLICATION, 99);
