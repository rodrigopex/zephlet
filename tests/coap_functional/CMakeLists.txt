cmake_minimum_required(VERSION 3.20.0)

get_filename_component(ZEPHLET_INFRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

list(APPEND EXTRA_ZEPHYR_MODULES "${ZEPHLET_INFRA_ROOT}")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephlet_coap_functional_test)

target_sources(app PRIVATE src/main.c)

# Each COAP_SERVICE_DEFINE produces a custom-named iterable section
# (`coap_resource_<svc>`); the app owns its linker registration. The LD
# template covers the pre-link pass; zephyr_iterable_section covers the
# CMake linker generator path.
zephyr_linker_sources(DATA_SECTIONS sections-ram.ld)
zephyr_iterable_section(
    NAME coap_resource_zlet_coap_test_service
    GROUP DATA_REGION ${XIP_ALIGN_WITH_INPUT})
