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
# separately; the public H563 session bridge is covered by web_session_test.
# Exception entry/unstack and UART adapters remain future work; the public
# worker already supports timestamped GPIO input and output events.
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

add_executable(web_session_wasm_smoke
  "${CMAKE_CURRENT_LIST_DIR}/../../tests/web_session_test.c"
)
target_link_libraries(web_session_wasm_smoke PRIVATE m33mu_wasm_core)
target_link_options(web_session_wasm_smoke PRIVATE
  -sENVIRONMENT=node
  -sSTACK_SIZE=1048576
)

add_executable(m33mu_web_api web_wasm_abi.c)
target_link_libraries(m33mu_web_api PRIVATE m33mu_wasm_core)
target_link_options(m33mu_web_api PRIVATE
  --no-entry
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createM33muModule
  -sENVIRONMENT=web,worker,node
  -sEXPORTED_FUNCTIONS=_malloc,_free,_mm_web_abi_create,_mm_web_abi_load,_mm_web_abi_step,_mm_web_abi_run_slice,_mm_web_abi_run_until,_mm_web_abi_last_deadline_reached,_mm_web_abi_last_pad_changed,_mm_web_abi_cancel_gpio_inputs,_mm_web_abi_pad_state,_mm_web_abi_gpio_output_config,_mm_web_abi_has_pending_deadline,_mm_web_abi_reset,_mm_web_abi_gpio_input,_mm_web_abi_gpio_output_next,_mm_web_abi_gpio_output_pad,_mm_web_abi_gpio_output_level,_mm_web_abi_gpio_output_time_low,_mm_web_abi_gpio_output_time_high,_mm_web_abi_last_error,_mm_web_abi_last_instruction_count,_mm_web_abi_last_stopped,_mm_web_abi_last_system_reset,_mm_web_abi_last_lockup,_mm_web_abi_last_lockup_pc,_mm_web_abi_cpu_reg,_mm_web_abi_cycle_low,_mm_web_abi_cycle_high,_mm_web_abi_xpsr,_mm_web_abi_msp_s,_mm_web_abi_msp_ns,_mm_web_abi_psp_s,_mm_web_abi_psp_ns,_mm_web_abi_security_state,_mm_web_abi_mode,_mm_web_abi_sleeping,_mm_web_abi_cpu_hz,_mm_web_abi_virtual_time_low,_mm_web_abi_virtual_time_high,_mm_web_abi_destroy,_mm_web_abi_uart_count,_mm_web_abi_uart_label,_mm_web_abi_uart_base,_mm_web_abi_uart_read,_mm_web_abi_uart_overflow,_mm_web_abi_uart_write
  -sEXPORTED_RUNTIME_METHODS=cwrap,HEAPU8
)
