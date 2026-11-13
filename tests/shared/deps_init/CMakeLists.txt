cmake_minimum_required(VERSION 3.20.0)

get_filename_component(ZEPHLET_INFRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

list(APPEND EXTRA_ZEPHYR_MODULES "${ZEPHLET_INFRA_ROOT}")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephlet_deps_init_test)

target_sources(app PRIVATE src/main.c)
# The shell scenario: the shell frontend prints through nanopb text format,
# which needs the infra's own zephlet.proto generated (at the build root).
if(CONFIG_ZEPHLETS_SHELL)
    list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
    include(nanopb)
    set(NANOPB_OPTIONS "--c-style")
    get_property(LOCAL_PROTO_FILES_LIST GLOBAL PROPERTY PROTO_FILES_LIST)
    set(_saved_csd ${CMAKE_CURRENT_SOURCE_DIR})
    set(CMAKE_CURRENT_SOURCE_DIR "${ZEPHLET_INFRA_ROOT}")
    zephyr_nanopb_sources(app ${LOCAL_PROTO_FILES_LIST})
    set(CMAKE_CURRENT_SOURCE_DIR ${_saved_csd})
    target_sources(app PRIVATE src/shell.c)
endif()
