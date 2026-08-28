cmake_minimum_required(VERSION 3.20.0)

# Walk up to the infra root (modules/lib/zephlet/) from this test dir.
get_filename_component(ZEPHLET_INFRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Register the infra module: with CONFIG_ZEPHLETS_SHELL=y (set in
# prj.conf) its own CMakeLists.txt pulls in frontends/shell/ (the macro
# framework's value.c + its include dir) automatically — nothing to
# wire manually here.
list(APPEND EXTRA_ZEPHYR_MODULES "${ZEPHLET_INFRA_ROOT}")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephlet_shell_macros_test)

target_sources(app PRIVATE src/main.c)

# Wire nanopb for the local fixture proto (all 18 supported scalar
# ltypes in one message).
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
set(NANOPB_OPTIONS "--c-style")
zephyr_nanopb_sources(app src/fixture.proto)
