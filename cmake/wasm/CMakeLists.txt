cmake_minimum_required(VERSION 3.20)

project(m33mu_wasm_core LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  message(FATAL_ERROR "Configure this project with emcmake")
endif()

file(GLOB M33MU_WASM_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/../../src/*.c"
  "${CMAKE_CURRENT_LIST_DIR}/../../src/m33mu/*.c"
  "${CMAKE_CURRENT_LIST_DIR}/../../cpu/*.c"
  "${CMAKE_CURRENT_LIST_DIR}/../../cpu/*/*.c"
)

# Host-facing modules stay outside the browser core. A future session API will
# provide browser-owned storage and explicit GPIO/UART callbacks.
set(M33MU_WASM_HOST_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/../../src/main.c"
  "${CMAKE_CURRENT_LIST_DIR}/../../src/gdbstub.c"
  "${CMAKE_CURRENT_LIST_DIR}/../../src/covdump.c"
  "${CMAKE_CURRENT_LIST_DIR}/../../src/m33mu/capstone.c"
  "${CMAKE_CURRENT_LIST_DIR}/../../src/m33mu/eth_backend.c"
  "${CMAKE_CURRENT_LIST_DIR}/../../src/m33mu/usbdev.c"
  "${CMAKE_CURRENT_LIST_DIR}/../../src/m33mu/tpm_tis.c"
)
list(REMOVE_ITEM M33MU_WASM_SOURCES ${M33MU_WASM_HOST_SOURCES})

add_library(m33mu_wasm_core STATIC ${M33MU_WASM_SOURCES})
target_include_directories(m33mu_wasm_core PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/../../include"
  "${CMAKE_CURRENT_LIST_DIR}/../../cpu"
)
target_compile_features(m33mu_wasm_core PUBLIC c_std_11)
target_compile_options(m33mu_wasm_core PRIVATE
  -Wall -Wextra -Werror -Wdeclaration-after-statement -O2
)
