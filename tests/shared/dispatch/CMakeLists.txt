cmake_minimum_required(VERSION 3.20.0)

get_filename_component(ZEPHLET_INFRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# Register the infra module itself as an extra Zephyr module so that
# zephlet.c and zephlet_iterables.ld get picked up.
list(APPEND EXTRA_ZEPHYR_MODULES "${ZEPHLET_INFRA_ROOT}")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephlet_dispatch_test)

target_sources(app PRIVATE src/main.c)
