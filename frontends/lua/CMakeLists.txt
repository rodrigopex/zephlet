if(CONFIG_ZEPHLETS_LUA)
    zephyr_library_named(zephlet_lua_frontend)
    zephyr_library_sources(
        zephlet_lua_events.c
        zephlet_lua_module.c
        zephlet_lua_pb.c
        zephlet_lua_coordinate.c
    )
    zephyr_include_directories(include)

    # One zephlet_lua_instance record per ZEPHLET_NEW(...), emitted by the
    # per-type hook. RAM: the records hold the flags and counters.
    zephyr_linker_sources(DATA_SECTIONS sections-ram.ld)
    zephyr_iterable_section(
        NAME zephlet_lua_instance
        GROUP DATA_REGION ${XIP_ALIGN_WITH_INPUT})

    # The frontend's own tasks, embedded as bytecode by lua_zephyr's
    # luaz_add_bytecode_file(). This module is processed before lua_zephyr,
    # which defines that function, so the call is deferred to the end of the
    # application's CMakeLists.txt. luaz resolves the path against the
    # application directory and writes the header to <build>/lua/.
    function(_zlet_lua_embed_prelude)
        file(RELATIVE_PATH _prelude "${CMAKE_SOURCE_DIR}"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/zephlet_lua_prelude.lua")
        luaz_add_bytecode_file("${_prelude}")
        target_include_directories(zephlet_lua_frontend PRIVATE "${CMAKE_BINARY_DIR}/lua")
        add_dependencies(zephlet_lua_frontend zephlet_lua_prelude_lua_bytecode_header)
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _zlet_lua_embed_prelude)

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
