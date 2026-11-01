if(CONFIG_ZEPHLET_BLOB)
    zephyr_zephlet_generate(
        TYPE    blob
        PREFIX  zlet_blob
        SOURCES zlet_blob.c)
endif()
