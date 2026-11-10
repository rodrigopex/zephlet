if(CONFIG_ZEPHLET_FAN)
    zephyr_zephlet_generate(
        TYPE    fan
        PREFIX  zlet_fan
        SOURCES zlet_fan.c)
endif()
