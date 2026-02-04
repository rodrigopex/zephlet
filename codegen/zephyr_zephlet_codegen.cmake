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
