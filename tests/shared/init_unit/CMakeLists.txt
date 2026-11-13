cmake_minimum_required(VERSION 3.20.0)

# Isolated unit test of zephlet.c: the real module is compiled on the
# unit_testing board against an FFF fake of the logging function it links
# (src/fakes.c). init_unit.ld collects the zephlet instance section and
# the instances' SYS_INIT entries, so a test can run them in the kernel's
# order over instances it rebinds. No kernel or zbus implementation is
# linked in.
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephlet_init_unit)

get_filename_component(ZEPHLET_INFRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

target_sources(testbinary PRIVATE
	src/main.c
	src/fakes.c
	${ZEPHLET_INFRA_ROOT}/zephlet.c
)

# zephlet.h includes pb.h for the method descriptors; nothing of nanopb
# is linked. Unit builds do not process modules, so locate it by hand.
if(NOT DEFINED ZEPHYR_NANOPB_MODULE_DIR)
	get_filename_component(ZEPHYR_NANOPB_MODULE_DIR "$ENV{ZEPHYR_BASE}/../modules/lib/nanopb"
		ABSOLUTE)
endif()

target_include_directories(testbinary PRIVATE
	src
	${ZEPHLET_INFRA_ROOT}
	${ZEPHYR_NANOPB_MODULE_DIR}
)

# The unit_testing arch stub does not define it; kernel.h, which zephlet.h
# includes, needs it.
target_compile_definitions(testbinary PRIVATE ARCH_STACK_PTR_ALIGN=8)

target_link_options(testbinary PRIVATE
	-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/init_unit.ld
)
