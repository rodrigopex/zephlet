if(CONFIG_ZEPHLET_TICK)
    zephyr_zephlet_generate(
        TYPE    tick
        PREFIX  zlet_tick
        SOURCES zlet_tick.c)
endif()
