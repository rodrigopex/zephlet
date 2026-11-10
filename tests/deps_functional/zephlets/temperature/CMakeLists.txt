if(CONFIG_ZEPHLET_TEMPERATURE)
    zephyr_zephlet_generate(
        TYPE    temperature
        PREFIX  zlet_temperature
        SOURCES zlet_temperature.c)
endif()
