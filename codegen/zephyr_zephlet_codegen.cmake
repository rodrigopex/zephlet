# SPDX-License-Identifier: Apache-2.0
# Zephlet code generation CMake module
# Auto-generates zephlet infrastructure (.h, .c, private/*_priv.h) during build

function(zephyr_zephlet_generate ZEPHLET_NAME PROTO_FILE)
  set(CODEGEN_SCRIPT "${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}/codegen/generate_zephlet.py")
  set(OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

  set(GENERATED_H "${OUTPUT_DIR}/${ZEPHLET_NAME}_interface.h")
  set(GENERATED_C "${OUTPUT_DIR}/${ZEPHLET_NAME}_interface.c")
  set(GENERATED_PRIV_H "${OUTPUT_DIR}/${ZEPHLET_NAME}.h")

  file(GLOB TEMPLATE_FILES "${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}/codegen/templates/*.jinja")

  # Check if .c exists in source, generate once if missing (bootstrap)
  set(IMPL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${ZEPHLET_NAME}.c")
  if(NOT EXISTS ${IMPL_FILE})
    message(STATUS "Bootstrapping ${ZEPHLET_NAME}.c (one-time generation)")
    execute_process(
      COMMAND ${PYTHON_EXECUTABLE} ${CODEGEN_SCRIPT}
        --proto ${PROTO_FILE}
        --output-dir ${CMAKE_CURRENT_SOURCE_DIR}
        --zephlet-name ${ZEPHLET_NAME}
        --module-dir ${CMAKE_CURRENT_SOURCE_DIR}
        --impl-only
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      RESULT_VARIABLE BOOTSTRAP_RESULT
    )
    if(NOT BOOTSTRAP_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to bootstrap ${ZEPHLET_NAME}.c")
    endif()
  endif()

  # Auto-generate infrastructure files to build directory
  add_custom_command(
    OUTPUT ${GENERATED_H} ${GENERATED_C} ${GENERATED_PRIV_H}
    COMMAND ${PYTHON_EXECUTABLE} ${CODEGEN_SCRIPT}
      --proto ${PROTO_FILE}
      --output-dir ${OUTPUT_DIR}
      --zephlet-name ${ZEPHLET_NAME}
      --module-dir ${CMAKE_CURRENT_SOURCE_DIR}
      --no-generate-impl
    DEPENDS ${PROTO_FILE} ${TEMPLATE_FILES} ${CODEGEN_SCRIPT}
    COMMENT "Generating ${ZEPHLET_NAME} from ${PROTO_FILE}"
    VERBATIM
  )

  add_custom_target(${ZEPHLET_NAME}_codegen
    DEPENDS ${GENERATED_H} ${GENERATED_C} ${GENERATED_PRIV_H})

  set(${ZEPHLET_NAME}_GENERATED_C ${GENERATED_C} PARENT_SCOPE)
endfunction()

# Proto generation: generates full .proto from schema + simplified base proto
# Outputs the generated proto to build directory for nanopb compilation.
function(zephyr_zephlet_generate_proto ZEPHLET_NAME SCHEMA_FILE BASE_PROTO)
  set(GENERATE_PROTO_SCRIPT "${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}/codegen/generate_proto.py")
  set(GENERATED_PROTO "${CMAKE_CURRENT_BINARY_DIR}/${ZEPHLET_NAME}.proto")

  file(GLOB PROTO_TEMPLATE_FILES "${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}/codegen/templates/zephlet_proto*.jinja")

  add_custom_command(
    OUTPUT ${GENERATED_PROTO}
    COMMAND ${PYTHON_EXECUTABLE} ${GENERATE_PROTO_SCRIPT}
      --schema ${SCHEMA_FILE}
      --proto ${BASE_PROTO}
      --output ${GENERATED_PROTO}
    DEPENDS ${SCHEMA_FILE} ${BASE_PROTO} ${GENERATE_PROTO_SCRIPT} ${PROTO_TEMPLATE_FILES}
    COMMENT "Generating ${ZEPHLET_NAME}.proto from base"
    VERBATIM
  )

  add_custom_target(${ZEPHLET_NAME}_proto_gen DEPENDS ${GENERATED_PROTO})

  set(${ZEPHLET_NAME}_GENERATED_PROTO ${GENERATED_PROTO} PARENT_SCOPE)
endfunction()

# Helper: snake_case to PascalCase (tick -> Tick, tamper_detection -> TamperDetection)
function(_zephlet_capitalize INPUT OUTPUT_VAR)
  set(_result "")
  string(REPLACE "_" ";" _parts "${INPUT}")
  foreach(_part IN LISTS _parts)
    string(SUBSTRING "${_part}" 0 1 _first)
    string(TOUPPER "${_first}" _first)
    string(SUBSTRING "${_part}" 1 -1 _rest)
    string(APPEND _result "${_first}${_rest}")
  endforeach()
  set(${OUTPUT_VAR} "${_result}" PARENT_SCOPE)
endfunction()

# Adapter code generation
# Generates auto-gen adapter.c (build dir) and bootstraps _impl.c (source dir)
# Handles Kconfig guard, zephyr_library_sources, and add_dependencies internally.
#
# Usage:
#   zephlet_adapter_generate(ORIGIN tick DEST ui REPORTS events)
#
# Derives CONFIG_<ORIGIN>_TO_<DEST>_ADAPTER and skips if disabled.
# Requires ZEPHLETS_PATH to be set in caller scope.
function(zephlet_adapter_generate)
  cmake_parse_arguments(ARG "" "ORIGIN;DEST" "REPORTS" ${ARGN})

  if(NOT ARG_ORIGIN OR NOT ARG_DEST OR NOT ARG_REPORTS)
    message(FATAL_ERROR "zephlet_adapter_generate: ORIGIN, DEST, and REPORTS are required")
  endif()

  if(NOT ZEPHLETS_PATH)
    message(FATAL_ERROR "zephlet_adapter_generate: ZEPHLETS_PATH must be set")
  endif()

  # Derive Kconfig symbol: CONFIG_TICK_TO_UI_ADAPTER
  string(TOUPPER "${ARG_ORIGIN}" _origin_upper)
  string(TOUPPER "${ARG_DEST}" _dest_upper)
  set(_config_var "CONFIG_${_origin_upper}_TO_${_dest_upper}_ADAPTER")

  if(NOT ${_config_var})
    return()
  endif()

  # Derive adapter name: Tick+Ui_zlet_adapter
  _zephlet_capitalize(${ARG_ORIGIN} _origin_cap)
  _zephlet_capitalize(${ARG_DEST} _dest_cap)
  set(ADAPTER_NAME "${_origin_cap}+${_dest_cap}_zlet_adapter")

  # Join REPORTS list into comma-separated string for --fields
  list(JOIN ARG_REPORTS "," SELECTED_FIELDS)

  set(CODEGEN_SCRIPT "${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}/codegen/generate_adapter.py")
  set(BUILD_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${ADAPTER_NAME}.c")
  set(IMPL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/${ADAPTER_NAME}_impl.c")
  set(GENERATED_PROTOS_PATH "${CMAKE_BINARY_DIR}/modules")

  file(GLOB TEMPLATE_FILES "${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}/codegen/templates/adapter*.jinja")
  file(GLOB PROTO_FILES "${ZEPHLETS_PATH}/*/*.proto")

  # Collect generated proto files as dependencies
  set(_gen_proto_origin "${GENERATED_PROTOS_PATH}/zlet_${ARG_ORIGIN}/zlet_${ARG_ORIGIN}.proto")
  set(_gen_proto_dest "${GENERATED_PROTOS_PATH}/zlet_${ARG_DEST}/zlet_${ARG_DEST}.proto")

  # Bootstrap: generate _impl.c if missing
  if(NOT EXISTS ${IMPL_FILE})
    message(STATUS "Bootstrapping ${ADAPTER_NAME}_impl.c (one-time generation)")
    execute_process(
      COMMAND ${PYTHON_EXECUTABLE} ${CODEGEN_SCRIPT}
        --non-interactive
        --zephlets-path ${ZEPHLETS_PATH}
        --generated-protos-path ${GENERATED_PROTOS_PATH}
        --origin ${ARG_ORIGIN}
        --dest ${ARG_DEST}
        --fields "${SELECTED_FIELDS}"
        --output-dir ${CMAKE_CURRENT_SOURCE_DIR}
        --impl-only
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      RESULT_VARIABLE BOOTSTRAP_RESULT
    )
    if(NOT BOOTSTRAP_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to bootstrap ${ADAPTER_NAME}_impl.c")
    endif()
  endif()

  # Build-time: always re-gen adapter.c to build dir + smart-update impl
  add_custom_command(
    OUTPUT ${BUILD_OUTPUT}
    COMMAND ${PYTHON_EXECUTABLE} ${CODEGEN_SCRIPT}
      --non-interactive
      --zephlets-path ${ZEPHLETS_PATH}
      --generated-protos-path ${GENERATED_PROTOS_PATH}
      --origin ${ARG_ORIGIN}
      --dest ${ARG_DEST}
      --fields "${SELECTED_FIELDS}"
      --output-dir ${CMAKE_CURRENT_SOURCE_DIR}
      --build-dir ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${PROTO_FILES} ${TEMPLATE_FILES} ${CODEGEN_SCRIPT}
      ${_gen_proto_origin} ${_gen_proto_dest}
    COMMENT "Generating ${ADAPTER_NAME} adapter"
    VERBATIM
  )

  add_custom_target(${ADAPTER_NAME}_codegen DEPENDS ${BUILD_OUTPUT})

  # Ensure adapter codegen runs after proto generation
  if(TARGET zlet_${ARG_ORIGIN}_proto_gen)
    add_dependencies(${ADAPTER_NAME}_codegen zlet_${ARG_ORIGIN}_proto_gen)
  endif()
  if(TARGET zlet_${ARG_DEST}_proto_gen)
    add_dependencies(${ADAPTER_NAME}_codegen zlet_${ARG_DEST}_proto_gen)
  endif()

  zephyr_library_sources(${BUILD_OUTPUT} "src/${ADAPTER_NAME}_impl.c")
  add_dependencies(${ZEPHYR_CURRENT_LIBRARY} ${ADAPTER_NAME}_codegen)
endfunction()

# Defines a complete zephlet: proto gen, codegen, interface library, and zephyr library.
# Wraps the entire if(CONFIG_ZEPHLET_<NAME>) guard.
#
# Usage:
#   zephyr_zephlet_define(tick)
#   zephyr_zephlet_define(ui INCLUDE_DIRS ${EXTRA_INC} SRCS extra.c)
macro(zephyr_zephlet_define _zephlet_name)
  cmake_parse_arguments(_zdef "" "" "INCLUDE_DIRS;SRCS" ${ARGN})

  string(TOUPPER "${_zephlet_name}" _zdef_NAME_UPPER)
  set(_zdef_PREFIX "zlet_${_zephlet_name}")
  set(_zdef_MODULE_DIR "${ZEPHYR_ZLET_${_zdef_NAME_UPPER}_MODULE_DIR}")

  if(CONFIG_ZEPHLET_${_zdef_NAME_UPPER})
    # Generate full proto from schema + base proto
    zephyr_zephlet_generate_proto(${_zdef_PREFIX}
        "${ZEPHYR_SHARED_ZEPHLET_MODULE_DIR}/zephlet.proto"
        "${_zdef_MODULE_DIR}/${_zdef_PREFIX}.proto")

    # Register generated proto for nanopb
    set_property(GLOBAL APPEND PROPERTY PROTO_FILES_LIST
        "${${_zdef_PREFIX}_GENERATED_PROTO}")

    # Auto-generate zephlet infrastructure from generated proto
    zephyr_zephlet_generate(${_zdef_PREFIX} "${${_zdef_PREFIX}_GENERATED_PROTO}")
    add_dependencies(${_zdef_PREFIX}_codegen ${_zdef_PREFIX}_proto_gen)

    # Expose build directory globally for zephlet headers
    zephyr_include_directories("${CMAKE_CURRENT_BINARY_DIR}")

    # Interface library with include paths
    zephyr_interface_library_named(${_zdef_PREFIX})
    target_include_directories(${_zdef_PREFIX} INTERFACE
        ${_zdef_MODULE_DIR}
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_BINARY_DIR}/zephlets"
        ${_zdef_INCLUDE_DIRS}
    )

    # Zephyr library with generated + implementation sources
    zephyr_library()
    zephyr_library_sources(
        ${${_zdef_PREFIX}_GENERATED_C}
        ${_zdef_MODULE_DIR}/${_zdef_PREFIX}.c
        ${_zdef_SRCS}
    )

    add_dependencies(${ZEPHYR_CURRENT_LIBRARY} ${_zdef_PREFIX}_codegen)
    zephyr_library_link_libraries(${_zdef_PREFIX})
  endif()
endmacro()
