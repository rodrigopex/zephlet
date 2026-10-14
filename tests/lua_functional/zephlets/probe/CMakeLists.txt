if(CONFIG_ZEPHLET_PROBE)
    zephyr_zephlet_generate(
        TYPE    probe
        PREFIX  zlet_probe
        SOURCES zlet_probe.c)
endif()
