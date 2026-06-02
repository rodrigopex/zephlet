cmake_minimum_required(VERSION 3.20.0)

get_filename_component(ZEPHLET_INFRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set_property(GLOBAL PROPERTY PROTO_FILES_LIST)

list(APPEND EXTRA_ZEPHYR_MODULES
    "${ZEPHLET_INFRA_ROOT}"
    "${CMAKE_CURRENT_SOURCE_DIR}/zephlets/tick"
    "${CMAKE_CURRENT_SOURCE_DIR}/zephlets/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/zephlets/silent"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephlet_coap_functional_test)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
set(NANOPB_OPTIONS "--c-style")

get_property(LOCAL_PROTO_FILES_LIST GLOBAL PROPERTY PROTO_FILES_LIST)

# Anchor nanopb output under tests/coap_functional/zephlets/ so each
# proto lands at ${CMAKE_BINARY_DIR}/<type>/<prefix>.pb.h, matching the
# `#include "<type>/<prefix>.pb.h"` pattern that codegen emits.
set(_saved_csd ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_CURRENT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zephlets")
zephyr_nanopb_sources(app ${LOCAL_PROTO_FILES_LIST})
set(CMAKE_CURRENT_SOURCE_DIR ${_saved_csd})

target_sources(app PRIVATE src/main.c)
