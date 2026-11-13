/* Sections zephlet.c needs that the unittest linker script does not
 * provide: the zephlet instances and their SYS_INIT entries, so a test can
 * run them in the kernel's order. */
SECTIONS
{
	.data.zephlet_area : ALIGN(8)
	{
		_zephlet_list_start = .;
		KEEP(*(SORT_BY_NAME(._zephlet.static.*)))
		_zephlet_list_end = .;
	}
	.data.init_unit_init_area : ALIGN(8)
	{
		_init_unit_init_start = .;
		/* As the kernel's CREATE_OBJ_LEVEL: by priority width, then name. */
		KEEP(*(SORT(.z_init_APPLICATION_P_?_*)))
		KEEP(*(SORT(.z_init_APPLICATION_P_??_*)))
		KEEP(*(SORT(.z_init_APPLICATION_P_???_*)))
		_init_unit_init_end = .;
	}
}
INSERT AFTER .data;
