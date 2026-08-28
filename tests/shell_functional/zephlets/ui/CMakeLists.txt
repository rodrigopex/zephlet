if(CONFIG_ZEPHLET_UI)
    zephyr_zephlet_generate(
        TYPE    ui
        PREFIX  zlet_ui
        SOURCES zlet_ui.c)
endif()
