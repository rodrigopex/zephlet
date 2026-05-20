# Use either the Zephyr-provided module dir or fallback to current source dir
if(ZEPHYR_SHARED_ZEPHLET_MODULE_DIR)
    set(SHARED_ZEPHLET_DIR "${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}")
else()
    set(SHARED_ZEPHLET_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Zephlet codegen setup - loaded first so all zephlets can use it
list(APPEND CMAKE_MODULE_PATH "${SHARED_ZEPHLET_DIR}/codegen")
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
    message(FATAL_ERROR "Missing Python packages. Install: pip install -r ${SHARED_ZEPHLET_DIR}/codegen/requirements.txt")
endif()

# Always add framework proto files to the list (registered before
# CONFIG variables are available). zephlet_options.proto declares the
# `(zephlet.coap)` custom service option referenced by opted-in
# per-zephlet protos.
set_property(
    GLOBAL
    APPEND
    PROPERTY
    PROTO_FILES_LIST
    "${SHARED_ZEPHLET_DIR}/zephlet.proto"
    "${SHARED_ZEPHLET_DIR}/zephlet_options.proto"
)

if(CONFIG_ZEPHLETS)
    zephyr_library()
    zephyr_library_sources("${SHARED_ZEPHLET_DIR}/zephlet.c")
    zephyr_include_directories(.)
    zephyr_linker_sources(DATA_SECTIONS zephlet_iterables.ld)

    # Expose nanopb parent directory globally (all .pb.h files)
    zephyr_include_directories("${CMAKE_BINARY_DIR}/zephlets")
    # Expose build root for zephlet.pb.h (generated at root due to RELPATH)
    zephyr_include_directories("${CMAKE_BINARY_DIR}")
endif()

add_subdirectory_ifdef(CONFIG_ZEPHLETS_COAP "${SHARED_ZEPHLET_DIR}/frontends/coap")
