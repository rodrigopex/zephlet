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
# Adapters in v0.3 are NOT generated. They are written by hand using
# the ZEPHLET_ADAPTER_DEFINE macro (see zephlet.h) under a Kconfig +
# CMake guard that depends on the participating zephlets.

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
  set(_zg_GEN_COAP_H "${_zg_GEN_DIR}/${_zg_PREFIX}_coap_interface.h")
  set(_zg_GEN_COAP_C "${_zg_GEN_DIR}/${_zg_PREFIX}_coap_interface.c")

  # Register proto for nanopb (top-level CMakeLists consumes the list).
  set_property(GLOBAL APPEND PROPERTY PROTO_FILES_LIST "${_zg_PROTO}")

  file(MAKE_DIRECTORY "${_zg_GEN_DIR}")
  add_custom_command(
    OUTPUT ${_zg_GEN_H} ${_zg_GEN_C} ${_zg_GEN_COAP_H} ${_zg_GEN_COAP_C}
    COMMAND ${PYTHON_EXECUTABLE} ${CODEGEN_SCRIPT}
        --proto ${_zg_PROTO}
        --output-dir ${_zg_GEN_DIR}
        --type ${_zg_TYPE}
        --prefix ${_zg_PREFIX}
    DEPENDS ${_zg_PROTO} ${CODEGEN_SCRIPT} ${_zg_TEMPLATES}
    COMMENT "zephlet v0.3: generating ${_zg_PREFIX}_interface"
    VERBATIM)

  add_custom_target(${_zg_PREFIX}_codegen
    DEPENDS ${_zg_GEN_H} ${_zg_GEN_C} ${_zg_GEN_COAP_H} ${_zg_GEN_COAP_C})

  zephyr_include_directories(
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${_zg_GEN_DIR}
      ${CMAKE_BINARY_DIR}
      ${_zg_INCLUDE_DIRS})

  zephyr_library()
  zephyr_library_sources(${_zg_GEN_C})
  # The CoAP interface TU is compiled only when the CoAP frontend is on.
  # When opt-in is absent from the .proto, the generated file is an
  # empty stub (zero symbols) — see codegen/generate_zephlet.py. The
  # opt-in decision lives entirely in Python; CMake just gates on
  # Kconfig.
  zephyr_library_sources_ifdef(CONFIG_ZEPHLETS_COAP ${_zg_GEN_COAP_C})
  foreach(_src IN LISTS _zg_SOURCES)
    zephyr_library_sources("${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
  endforeach()
  add_dependencies(${ZEPHYR_CURRENT_LIBRARY} ${_zg_PREFIX}_codegen)

  # The user's `app` library also #includes the generated <prefix>_interface.h
  # transitively (via the user's <prefix>.h). Without an explicit dependency,
  # ninja may try to compile `main.c` before the codegen finishes, which
  # surfaces on slower hosts as a missing-header build error. Defer the
  # `add_dependencies` call so it runs after the user's CMakeLists has
  # declared the `app` target.
  #
  # `DEFER CALL` evaluates variable references at the time the deferred call
  # runs, when `_zg_PREFIX` is already out of scope. Use `EVAL CODE` to
  # substitute the prefix into the deferred command at registration time.
  cmake_language(EVAL CODE
    "cmake_language(DEFER DIRECTORY \${CMAKE_SOURCE_DIR} \
       CALL add_dependencies app ${_zg_PREFIX}_codegen)")
endfunction()
