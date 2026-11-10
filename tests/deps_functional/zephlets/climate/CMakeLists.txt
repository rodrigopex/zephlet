if(CONFIG_ZEPHLET_CLIMATE)
    zephyr_zephlet_generate(
        TYPE    climate
        PREFIX  zlet_climate
        SOURCES zlet_climate.c)
endif()
