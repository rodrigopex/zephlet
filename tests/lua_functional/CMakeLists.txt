cmake_minimum_required(VERSION 3.20.0)

get_filename_component(ZEPHLET_INFRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set_property(GLOBAL PROPERTY PROTO_FILES_LIST)

# CONFIG_ZEPHLETS_LUA=y (prj.conf) makes the infra module pull in
# frontends/lua/ automatically. lua_zephyr comes from the west manifest.
list(APPEND EXTRA_ZEPHYR_MODULES
    "${ZEPHLET_INFRA_ROOT}"
    "${CMAKE_CURRENT_SOURCE_DIR}/zephlets/probe"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephlet_lua_functional_test)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
set(NANOPB_OPTIONS "--c-style")

get_property(LOCAL_PROTO_FILES_LIST GLOBAL PROPERTY PROTO_FILES_LIST)

# Anchor nanopb output under zephlets/ so each proto lands at
# ${CMAKE_BINARY_DIR}/<type>/<prefix>.pb.h, the path codegen #includes.
set(_saved_csd ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_CURRENT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zephlets")
zephyr_nanopb_sources(app ${LOCAL_PROTO_FILES_LIST})
set(CMAKE_CURRENT_SOURCE_DIR ${_saved_csd})

target_sources(app PRIVATE src/main.c)

if(CONFIG_ZEPHLETS_LUA)
    # One suite per scenario, chosen in testcase.yaml; main() runs it as
    # ZEPHLET_LUA_SCRIPT(SUITE).
    luaz_add_bytecode_file("scripts/${CONFIG_TEST_LUA_SUITE}.lua")
    target_compile_definitions(app PRIVATE
        SUITE=${CONFIG_TEST_LUA_SUITE}
        SUITE_HEADER="${CONFIG_TEST_LUA_SUITE}_lua_bytecode.h")
endif()
