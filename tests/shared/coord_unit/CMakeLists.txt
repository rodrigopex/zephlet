cmake_minimum_required(VERSION 3.20.0)

# Isolated unit test of zephlet_coord.c: the real module is compiled on the
# unit_testing board and every external dependency it links against (work
# queue, zbus runtime observers, minimal logging) is an FFF fake from
# src/fakes.c. No kernel, zbus, or logging implementation is linked in.
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephlet_coord_unit)

get_filename_component(ZEPHLET_INFRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

target_sources(testbinary PRIVATE
	src/main.c
	src/fakes.c
	${ZEPHLET_INFRA_ROOT}/zephlet_coord.c
)

target_include_directories(testbinary PRIVATE
	src
	${ZEPHLET_INFRA_ROOT}
)

# The unit_testing arch stub does not define it, and the coordinator's
# K_THREAD_STACK_DEFINE needs it to size the (unused) workqueue stack.
target_compile_definitions(testbinary PRIVATE ARCH_STACK_PTR_ALIGN=8)
