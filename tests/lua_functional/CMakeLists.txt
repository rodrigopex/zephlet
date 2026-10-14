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
    # One suite per scenario, chosen in testcase.yaml.
    set(_suite_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_include_directories(app PRIVATE ${_suite_gen})
    generate_inc_file_for_target(app
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/${CONFIG_TEST_LUA_SUITE}.lua"
        "${_suite_gen}/suite.lua.inc")
endif()
