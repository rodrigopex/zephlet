# Zephlet codegen setup - loaded first so all zephlets can use it
list(APPEND CMAKE_MODULE_PATH "${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}/codegen")
include(zephyr_zephlet_codegen)

# Find Python for zephlet code generation
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(PYTHON_EXECUTABLE ${Python3_EXECUTABLE})

# Check for required Python packages
execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -c "import proto_schema_parser; import jinja2"
    RESULT_VARIABLE PKG_CHECK
    OUTPUT_QUIET ERROR_QUIET
)
if(NOT PKG_CHECK EQUAL 0)
    message(FATAL_ERROR "Missing Python packages. Install: pip install -r ${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}/codegen/requirements.txt")
endif()

if(CONFIG_ZEPHLETS)
    set_property(
        GLOBAL
        APPEND
        PROPERTY
        PROTO_FILES_LIST
        "${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}/zephlet.proto"
    )
    message(WARNING "Shared Zephlet Proto File Added: ${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}/zephlet.proto")

    zephyr_library()
    zephyr_library_sources("${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}/zephlet.c")
    zephyr_include_directories(.)
    zephyr_linker_sources(DATA_SECTIONS zephlet_iterables.ld)

    # Expose nanopb parent directory globally (all .pb.h files)
    zephyr_include_directories("${CMAKE_BINARY_DIR}/zephlets")
    # Expose shared zephlet build directory for zephlet.pb.h
    zephyr_include_directories("${CMAKE_BINARY_DIR}/zephlets/shared")

endif()
