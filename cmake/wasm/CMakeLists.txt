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
list(APPEND M33MU_WASM_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/wasm_host_stubs.c"
)

# Host-facing modules stay outside the browser core. The production session
# owner is compiled below. A Node-linked session regression smoke is built
# separately; browser worker messaging and GPIO/UART adapters remain future
# work.
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

# Run the same production-session regression suite under Node/WASM. This is a
# session API smoke, not a browser worker or UI test.
add_executable(execution_state_wasm_smoke
  "${CMAKE_CURRENT_LIST_DIR}/../../tests/execution_state_test.c"
)
target_link_libraries(execution_state_wasm_smoke PRIVATE m33mu_wasm_core)
target_link_options(execution_state_wasm_smoke PRIVATE
  -sENVIRONMENT=node
  -sSTACK_SIZE=1048576
)
