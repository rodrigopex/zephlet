cmake_minimum_required(VERSION 3.20.0)

# Walk up to the infra root (modules/lib/zephlet/) from this test dir.
get_filename_component(ZEPHLET_INFRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Register the infra module so zephlet.h's include dir is exported and
# zephlet.c gets pulled in by CONFIG_ZEPHLETS=y.
list(APPEND EXTRA_ZEPHYR_MODULES "${ZEPHLET_INFRA_ROOT}")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephlet_coap_translate_test)

# Compile the translate translation unit directly so the unit test does
# not have to enable CONFIG_ZEPHLETS_COAP (which selects NETWORKING /
# NET_UDP / NET_SOCKETS / COAP_SERVER). Packet-level CoAP helpers come
# from CONFIG_COAP=y alone.
target_sources(app PRIVATE
    src/main.c
    ${ZEPHLET_INFRA_ROOT}/frontends/coap/zephlet_coap_translate.c
)
target_include_directories(app PRIVATE
    ${ZEPHLET_INFRA_ROOT}/frontends/coap/include
)

# Wire nanopb for the local test proto.
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
set(NANOPB_OPTIONS "--c-style")

zephyr_nanopb_sources(app src/test_msg.proto)
