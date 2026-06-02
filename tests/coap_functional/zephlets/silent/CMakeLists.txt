if(CONFIG_ZEPHLET_SILENT)
    zephyr_zephlet_generate(
        TYPE    silent
        PREFIX  zlet_silent
        SOURCES zlet_silent.c)
endif()
