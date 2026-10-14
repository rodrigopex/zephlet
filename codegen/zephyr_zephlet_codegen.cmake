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
#           <prefix>.zephlet.options          ← optional: frontend opt-ins
#           <prefix>.c                       ← user strong overrides
#           <prefix>.h                       ← user-owned types / init_fn
#
#       Emitted into build dir:
#           ${CMAKE_BINARY_DIR}/modules/<prefix>/<prefix>_interface.{h,c}
#           ... plus the per-frontend <prefix>_{coap,lua}_interface.{h,c}
#           and the LuaLS annotations <prefix>_lua_meta.lua
#
# Adapters in v0.3 are NOT generated. They are written by hand using
# the ZEPHLET_ADAPTER_DEFINE macro (see zephlet.h) under a Kconfig +
# CMake guard that depends on the participating zephlets.

# Fold nanopb's generated-header barrier into Zephyr's, so every Zephyr
# library -- not only the app that called zephyr_nanopb_sources() -- is
# ordered after protoc. Deferred to the end of the app's directory,
# because `include(nanopb)` creates the target after this file's
# functions have already run.
function(_zephlet_order_nanopb_headers)
  if(TARGET nanopb_generated_headers)
    add_dependencies(zephyr_generated_headers nanopb_generated_headers)
  endif()
endfunction()

function(zephyr_zephlet_generate)
  cmake_parse_arguments(_zg "" "TYPE;PREFIX" "SOURCES;INCLUDE_DIRS" ${ARGN})

  if(NOT _zg_TYPE OR NOT _zg_PREFIX)
    message(FATAL_ERROR "zephyr_zephlet_generate: TYPE and PREFIX are required")
  endif()

  set(CODEGEN_SCRIPT
      "${ZEPHYR_ZEPHLET_MODULE_DIR}/codegen/generate_zephlet.py")
  set(TEMPLATE_GLOB
      "${ZEPHYR_ZEPHLET_MODULE_DIR}/codegen/templates/zephlet_*.jinja")
  file(GLOB _zg_TEMPLATES ${TEMPLATE_GLOB})

  set(_zg_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/${_zg_PREFIX}.proto")
  set(_zg_OPTIONS "${CMAKE_CURRENT_SOURCE_DIR}/${_zg_PREFIX}.zephlet.options")
  # The options file is optional. CONFIGURE_DEPENDS re-runs CMake when it is
  # added or removed, so the codegen picks the change up.
  file(GLOB _zg_OPTIONS_DEP CONFIGURE_DEPENDS "${_zg_OPTIONS}")
  set(_zg_GEN_DIR "${CMAKE_BINARY_DIR}/modules/${_zg_PREFIX}")
  set(_zg_GEN_H "${_zg_GEN_DIR}/${_zg_PREFIX}_interface.h")
  set(_zg_GEN_C "${_zg_GEN_DIR}/${_zg_PREFIX}_interface.c")
  set(_zg_GEN_COAP_H "${_zg_GEN_DIR}/${_zg_PREFIX}_coap_interface.h")
  set(_zg_GEN_COAP_C "${_zg_GEN_DIR}/${_zg_PREFIX}_coap_interface.c")
  set(_zg_GEN_LUA_H "${_zg_GEN_DIR}/${_zg_PREFIX}_lua_interface.h")
  set(_zg_GEN_LUA_C "${_zg_GEN_DIR}/${_zg_PREFIX}_lua_interface.c")
  set(_zg_GEN_LUA_META "${_zg_GEN_DIR}/${_zg_PREFIX}_lua_meta.lua")
  set(_zg_OUTPUTS ${_zg_GEN_H} ${_zg_GEN_C} ${_zg_GEN_COAP_H} ${_zg_GEN_COAP_C}
                  ${_zg_GEN_LUA_H} ${_zg_GEN_LUA_C} ${_zg_GEN_LUA_META})

  # Register proto for nanopb (top-level CMakeLists consumes the list).
  set_property(GLOBAL APPEND PROPERTY PROTO_FILES_LIST "${_zg_PROTO}")

  file(MAKE_DIRECTORY "${_zg_GEN_DIR}")
  # Whether the options file exists is an input too. A removed file just
  # leaves DEPENDS, with nothing newer than the outputs, so Ninja would keep
  # the frontends it enabled. This stamp is rewritten only when that changes.
  if(_zg_OPTIONS_DEP)
    set(_zg_OPTIONS_STATE "present")
  else()
    set(_zg_OPTIONS_STATE "absent")
  endif()
  set(_zg_OPTIONS_STAMP "${_zg_GEN_DIR}/${_zg_PREFIX}.zephlet.options.state")
  file(CONFIGURE OUTPUT "${_zg_OPTIONS_STAMP}"
       CONTENT "${_zg_PREFIX}.zephlet.options: ${_zg_OPTIONS_STATE}\n")
  add_custom_command(
    OUTPUT ${_zg_OUTPUTS}
    COMMAND ${PYTHON_EXECUTABLE} ${CODEGEN_SCRIPT}
        --proto ${_zg_PROTO}
        --output-dir ${_zg_GEN_DIR}
        --type ${_zg_TYPE}
        --prefix ${_zg_PREFIX}
        --options ${_zg_OPTIONS}
    DEPENDS ${_zg_PROTO} ${_zg_OPTIONS_DEP} ${_zg_OPTIONS_STAMP} ${CODEGEN_SCRIPT}
            ${_zg_TEMPLATES}
    COMMENT "zephlet v0.3: generating ${_zg_PREFIX}_interface"
    VERBATIM)

  add_custom_target(${_zg_PREFIX}_codegen DEPENDS ${_zg_OUTPUTS})

  # Any Zephyr library may #include the generated <prefix>_interface.h,
  # directly or transitively: the zephlet's own library, the user's
  # `app`, and coordinator libraries that live in other modules. Zephyr
  # gives every registered library an ordering dependency on
  # `zephyr_generated_headers` (see zephyr/CMakeLists.txt), so hooking
  # the codegen target there covers all of them at once, including
  # consumers that do not exist yet.
  #
  # Without it ninja is free to compile such a TU before the generator
  # has run. That surfaces as an intermittent missing-header build
  # error whose outcome depends on host core count, ninja -j and the
  # ccache hit rate, so it reproduces on some build machines only.
  add_dependencies(zephyr_generated_headers ${_zg_PREFIX}_codegen)

  # That header also #includes the nanopb header for this zephlet's
  # .proto, and Zephyr hooks `nanopb_generated_headers` to the single
  # target handed to zephyr_nanopb_sources() -- the app. Every other
  # library including the interface header races protoc the same way,
  # so fold nanopb's barrier into Zephyr's too. Register the hook once
  # per build rather than once per zephlet.
  get_property(_zg_nanopb_hooked GLOBAL PROPERTY _ZEPHLET_NANOPB_HOOKED)
  if(NOT _zg_nanopb_hooked)
    set_property(GLOBAL PROPERTY _ZEPHLET_NANOPB_HOOKED TRUE)
    cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR}
                   CALL _zephlet_order_nanopb_headers)
  endif()

  zephyr_include_directories(
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${_zg_GEN_DIR}
      ${CMAKE_BINARY_DIR}
      ${_zg_INCLUDE_DIRS})

  zephyr_library()
  zephyr_library_sources(${_zg_GEN_C})
  # The CoAP interface TU is compiled only when the CoAP frontend is on.
  # When the zephlet does not set the `coap` option, the generated file is an
  # empty stub (zero symbols) — see codegen/generate_zephlet.py. The
  # opt-in decision lives entirely in Python; CMake just gates on
  # Kconfig.
  zephyr_library_sources_ifdef(CONFIG_ZEPHLETS_COAP ${_zg_GEN_COAP_C})
  # Same for the Lua bindings. The LuaLS annotations are never compiled; the
  # frontend's post-build step collects every *_lua_meta.lua it finds.
  zephyr_library_sources_ifdef(CONFIG_ZEPHLETS_LUA ${_zg_GEN_LUA_C})
  foreach(_src IN LISTS _zg_SOURCES)
    zephyr_library_sources("${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
  endforeach()
endfunction()
