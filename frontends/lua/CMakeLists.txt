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

    # LuaLS: after the link, read the instance records' symbols back from
    # the ELF and gather every type's annotations into one directory.
    set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
        COMMAND ${PYTHON_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/zephlet_lua_instances.py"
            --elf "${ZEPHYR_BINARY_DIR}/${KERNEL_ELF_NAME}"
            --meta-dir "${CMAKE_BINARY_DIR}/modules"
            --base "${CMAKE_CURRENT_SOURCE_DIR}/luals/zephlets_base.lua"
            --out "${CMAKE_BINARY_DIR}/zephlets_lua")
endif()
