# SPDX-License-Identifier: Apache-2.0
#
# Zephlet v0.3 CMake helpers.
#
# Provides:
#   zephyr_zephlet_generate(TYPE <snake_name> PREFIX <file_prefix>
#                           [SOURCES <user.c...>]
#                           [INCLUDE_DIRS <dir...>])
#       Register the zephlet's .proto for nanopb, run the v0.3 Python
#       generator to emit <prefix>_interface.{h,c} into the build dir,
#       and wire up a zephyr_library with the user sources + generated
#       interface source.
#
#       The caller is the zephlet module's top-level CMakeLists.txt and
#       runs in the module's Zephyr module scope. Expected layout:
#
#         <module-root>/
#           <prefix>.proto
#           <prefix>.c                       ← user strong overrides
#           <prefix>.h                       ← user-owned types / init_fn
#
#       Emitted into build dir:
#           ${CMAKE_BINARY_DIR}/modules/<prefix>/<prefix>_interface.{h,c}
#
#   zephlet_adapter_generate(...)   — Phase 5. Kept as the existing
#                                     Invoke/Report-shaped generator for
#                                     now; will be rewritten in the
#                                     adapters phase.

function(zephyr_zephlet_generate)
  cmake_parse_arguments(_zg "" "TYPE;PREFIX" "SOURCES;INCLUDE_DIRS" ${ARGN})

  if(NOT _zg_TYPE OR NOT _zg_PREFIX)
    message(FATAL_ERROR "zephyr_zephlet_generate: TYPE and PREFIX are required")
  endif()

  set(CODEGEN_SCRIPT
      "${ZEPHYR_ZEPHLET_MODULE_DIR}/codegen/generate_zephlet.py")
  set(TEMPLATE_GLOB
      "${ZEPHYR_ZEPHLET_MODULE_DIR}/codegen/templates/zephlet_interface*.jinja")
  file(GLOB _zg_TEMPLATES ${TEMPLATE_GLOB})

  set(_zg_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/${_zg_PREFIX}.proto")
  set(_zg_GEN_DIR "${CMAKE_BINARY_DIR}/modules/${_zg_PREFIX}")
  set(_zg_GEN_H "${_zg_GEN_DIR}/${_zg_PREFIX}_interface.h")
  set(_zg_GEN_C "${_zg_GEN_DIR}/${_zg_PREFIX}_interface.c")

  # Register proto for nanopb (top-level CMakeLists consumes the list).
  set_property(GLOBAL APPEND PROPERTY PROTO_FILES_LIST "${_zg_PROTO}")

  file(MAKE_DIRECTORY "${_zg_GEN_DIR}")
  add_custom_command(
    OUTPUT ${_zg_GEN_H} ${_zg_GEN_C}
    COMMAND ${PYTHON_EXECUTABLE} ${CODEGEN_SCRIPT}
        --proto ${_zg_PROTO}
        --output-dir ${_zg_GEN_DIR}
        --type ${_zg_TYPE}
        --prefix ${_zg_PREFIX}
    DEPENDS ${_zg_PROTO} ${CODEGEN_SCRIPT} ${_zg_TEMPLATES}
    COMMENT "zephlet v0.3: generating ${_zg_PREFIX}_interface"
    VERBATIM)

  add_custom_target(${_zg_PREFIX}_codegen DEPENDS ${_zg_GEN_H} ${_zg_GEN_C})

  zephyr_include_directories(
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${_zg_GEN_DIR}
      ${CMAKE_BINARY_DIR}
      ${_zg_INCLUDE_DIRS})

  zephyr_library()
  zephyr_library_sources(${_zg_GEN_C})
  foreach(_src IN LISTS _zg_SOURCES)
    zephyr_library_sources("${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
  endforeach()
  add_dependencies(${ZEPHYR_CURRENT_LIBRARY} ${_zg_PREFIX}_codegen)
endfunction()

# ---------------------------------------------------------------------------
# Legacy helpers — kept until their callers migrate.
# ---------------------------------------------------------------------------

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

# Adapter code generation — Phase 5 rewrites this. The function currently
# targets the pre-v0.3 Invoke/Report adapter shape and will not work
# against v0.3 zephlets; callers should treat it as unavailable until
# the adapter generator is rewritten.
function(zephlet_adapter_generate)
  cmake_parse_arguments(ARG "" "ORIGIN;DEST" "REPORTS" ${ARGN})

  if(NOT ARG_ORIGIN OR NOT ARG_DEST OR NOT ARG_REPORTS)
    message(FATAL_ERROR "zephlet_adapter_generate: ORIGIN, DEST, and REPORTS are required")
  endif()

  if(NOT ZEPHLETS_PATH)
    message(FATAL_ERROR "zephlet_adapter_generate: ZEPHLETS_PATH must be set")
  endif()

  string(TOUPPER "${ARG_ORIGIN}" _origin_upper)
  string(TOUPPER "${ARG_DEST}" _dest_upper)
  set(_config_var "CONFIG_${_origin_upper}_TO_${_dest_upper}_ADAPTER")

  if(NOT ${_config_var})
    return()
  endif()

  _zephlet_capitalize(${ARG_ORIGIN} _origin_cap)
  _zephlet_capitalize(${ARG_DEST} _dest_cap)
  set(ADAPTER_NAME "${_origin_cap}+${_dest_cap}_zlet_adapter")

  list(JOIN ARG_REPORTS "," SELECTED_FIELDS)

  set(CODEGEN_SCRIPT "${ZEPHYR_ZEPHLET_MODULE_DIR}/codegen/generate_adapter.py")
  set(BUILD_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${ADAPTER_NAME}.c")
  set(IMPL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/${ADAPTER_NAME}_impl.c")
  set(GENERATED_PROTOS_PATH "${CMAKE_BINARY_DIR}/modules")

  file(GLOB TEMPLATE_FILES "${ZEPHYR_ZEPHLET_MODULE_DIR}/codegen/templates/adapter*.jinja")
  file(GLOB PROTO_FILES "${ZEPHLETS_PATH}/*/*.proto")

  set(_gen_proto_origin "${GENERATED_PROTOS_PATH}/zlet_${ARG_ORIGIN}/zlet_${ARG_ORIGIN}.proto")
  set(_gen_proto_dest "${GENERATED_PROTOS_PATH}/zlet_${ARG_DEST}/zlet_${ARG_DEST}.proto")

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

  if(TARGET zlet_${ARG_ORIGIN}_proto_gen)
    add_dependencies(${ADAPTER_NAME}_codegen zlet_${ARG_ORIGIN}_proto_gen)
  endif()
  if(TARGET zlet_${ARG_DEST}_proto_gen)
    add_dependencies(${ADAPTER_NAME}_codegen zlet_${ARG_DEST}_proto_gen)
  endif()

  zephyr_library_sources(${BUILD_OUTPUT} "src/${ADAPTER_NAME}_impl.c")
  add_dependencies(${ZEPHYR_CURRENT_LIBRARY} ${ADAPTER_NAME}_codegen)
endfunction()
