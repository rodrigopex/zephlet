cmake_minimum_required(VERSION 3.20.0)

# Configure-only host app for test_deps_config.py. The test writes the
# zephlet modules of each scenario into DEPS_CONFIG_ROOT and lists them in
# DEPS_CONFIG_MODULES; nothing here knows which zephlets exist.
get_filename_component(ZEPHLET_INFRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set_property(GLOBAL PROPERTY PROTO_FILES_LIST)

list(APPEND EXTRA_ZEPHYR_MODULES "${ZEPHLET_INFRA_ROOT}" ${DEPS_CONFIG_MODULES})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephlet_deps_config_test)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
set(NANOPB_OPTIONS "--c-style")

get_property(LOCAL_PROTO_FILES_LIST GLOBAL PROPERTY PROTO_FILES_LIST)

# Each proto lands at ${CMAKE_BINARY_DIR}/<type>/<prefix>.pb.h.
set(_saved_csd ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_CURRENT_SOURCE_DIR "${DEPS_CONFIG_ROOT}")
zephyr_nanopb_sources(app ${LOCAL_PROTO_FILES_LIST})
set(CMAKE_CURRENT_SOURCE_DIR ${_saved_csd})

target_sources(app PRIVATE src/main.c)
