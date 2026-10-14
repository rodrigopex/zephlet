if(CONFIG_ZEPHLETS_LUA)
    zephyr_library_named(zephlet_lua_frontend)
    zephyr_library_sources(
        zephlet_lua_events.c
        zephlet_lua_module.c
        zephlet_lua_pb.c
        zephlet_lua_run.c
    )
    zephyr_include_directories(include)

    # One zephlet_lua_instance record per ZEPHLET_NEW(...), emitted by the
    # per-type hook. RAM: the records hold the flags and counters.
    zephyr_linker_sources(DATA_SECTIONS sections-ram.ld)
    zephyr_iterable_section(
        NAME zephlet_lua_instance
        GROUP DATA_REGION ${XIP_ALIGN_WITH_INPUT})

    # The frontend's own tasks, embedded as Lua source.
    set(_zlet_lua_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
    zephyr_library_include_directories(${_zlet_lua_gen})
    generate_inc_file_for_target(zephlet_lua_frontend
        "${CMAKE_CURRENT_SOURCE_DIR}/zephlet_lua_prelude.lua"
        "${_zlet_lua_gen}/zephlet_lua_prelude.inc")
endif()
