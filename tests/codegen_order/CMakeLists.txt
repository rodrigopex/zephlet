cmake_minimum_required(VERSION 3.20.0)

get_filename_component(ZEPHLET_INFRA_ROOT
                       "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(ZEPHLET_FIXTURES
                       "${CMAKE_CURRENT_SOURCE_DIR}/../coap_functional/zephlets"
                       ABSOLUTE)

set_property(GLOBAL PROPERTY PROTO_FILES_LIST)

# `tick` is reused from the coap_functional fixtures rather than cloned.
# `consumer` is the point of this fixture: a plain zephyr_library, in its
# own module, that includes tick's generated interface header.
list(APPEND EXTRA_ZEPHYR_MODULES
    "${ZEPHLET_INFRA_ROOT}"
    "${ZEPHLET_FIXTURES}/tick"
    "${CMAKE_CURRENT_SOURCE_DIR}/consumer"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephlet_codegen_order_test)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
set(NANOPB_OPTIONS "--c-style")

get_property(LOCAL_PROTO_FILES_LIST GLOBAL PROPERTY PROTO_FILES_LIST)

# Anchor nanopb output under the fixtures dir so each proto lands at
# ${CMAKE_BINARY_DIR}/<type>/<prefix>.pb.h, matching the
# `#include "<type>/<prefix>.pb.h"` pattern that codegen emits.
set(_saved_csd ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_CURRENT_SOURCE_DIR "${ZEPHLET_FIXTURES}")
zephyr_nanopb_sources(app ${LOCAL_PROTO_FILES_LIST})
set(CMAKE_CURRENT_SOURCE_DIR ${_saved_csd})

target_sources(app PRIVATE src/main.c)
