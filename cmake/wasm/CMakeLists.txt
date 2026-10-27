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
  # The optional ONNX Runtime backend links a native host library. N6's
  # register-file model is included through cpu/stm32n657/*.c above.
  "${CMAKE_CURRENT_LIST_DIR}/../../src/npu_onnx.c"
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

# Optional wolfCrypt for the crypto peripheral models (HASH/AES/PKA/RNG,
# CASPER/HASHCRYPT, CryptoCell, ...) and the C-native secure elements.
# Point M33MU_WOLFSSL_SRC at a wolfSSL source tree; nothing is vendored.
set(M33MU_WOLFSSL_SRC "" CACHE PATH "wolfSSL source tree to compile for WASM")
if(M33MU_WOLFSSL_SRC)
  set(_wc "${M33MU_WOLFSSL_SRC}/wolfcrypt/src")
  add_library(m33mu_wasm_wolfcrypt STATIC
    ${_wc}/aes.c ${_wc}/sha.c ${_wc}/sha256.c ${_wc}/sha512.c ${_wc}/sha3.c
    ${_wc}/hmac.c ${_wc}/hash.c ${_wc}/kdf.c ${_wc}/random.c ${_wc}/ecc.c
    ${_wc}/rsa.c ${_wc}/sp_int.c ${_wc}/wolfmath.c ${_wc}/memory.c
    ${_wc}/logging.c ${_wc}/error.c ${_wc}/coding.c ${_wc}/asn.c ${_wc}/cmac.c
    ${_wc}/wc_port.c ${_wc}/wc_encrypt.c
    "${CMAKE_CURRENT_LIST_DIR}/wolfssl/wasm_rng_seed.c")
  target_include_directories(m33mu_wasm_wolfcrypt BEFORE PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/wolfssl" "${M33MU_WOLFSSL_SRC}")
  target_compile_definitions(m33mu_wasm_wolfcrypt PUBLIC WOLFSSL_USER_SETTINGS)
  target_compile_options(m33mu_wasm_wolfcrypt PRIVATE -O2 -w)
  target_compile_definitions(m33mu_wasm_core PUBLIC M33MU_HAS_WOLFSSL=1)
  target_link_libraries(m33mu_wasm_core PUBLIC m33mu_wasm_wolfcrypt)
endif()

# Optional Rust secure-element simulators (ATECC608, SE050, STSAFE-A120,
# TROPIC01) built for wasm32-unknown-emscripten from third_party. Needs cargo
# and `rustup target add wasm32-unknown-emscripten`.
option(M33MU_WASM_RUST_SE "Link the Rust secure-element simulators" OFF)
if(M33MU_WASM_RUST_SE)
  find_program(M33MU_CARGO cargo REQUIRED)
  set(_rs_ws "${CMAKE_CURRENT_LIST_DIR}/../../third_party/wolfssl-simulators")
  set(_rs_out "${CMAKE_BINARY_DIR}/rust/wasm32-unknown-emscripten/release")
  set(_rs_libs "${_rs_out}/libatecc608_sim.a" "${_rs_out}/libse050_sim.a"
               "${_rs_out}/libstsafe_a120_sim.a" "${_rs_out}/libtropic01_sim.a")
  file(GLOB_RECURSE _rs_srcs CONFIGURE_DEPENDS "${_rs_ws}/*/src/*.rs" "${_rs_ws}/*/Cargo.toml")
  add_custom_command(OUTPUT ${_rs_libs}
    COMMAND "${M33MU_CARGO}" build --release --target wasm32-unknown-emscripten
            --target-dir "${CMAKE_BINARY_DIR}/rust"
    WORKING_DIRECTORY "${_rs_ws}"
    DEPENDS ${_rs_srcs}
    COMMENT "Building Rust secure-element simulators for WASM"
    VERBATIM)
  add_custom_target(m33mu_wasm_rust_se DEPENDS ${_rs_libs})
  add_dependencies(m33mu_wasm_core m33mu_wasm_rust_se)
  target_compile_definitions(m33mu_wasm_core PUBLIC M33MU_HAS_RUST_PLUGINS=1)
  target_link_libraries(m33mu_wasm_core PUBLIC ${_rs_libs})
endif()

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
  -sEXPORTED_FUNCTIONS=_malloc,_free,_mm_web_abi_version,_mm_web_abi_features,_mm_web_abi_create,_mm_web_abi_create_cpu,_mm_web_abi_read_memory,_mm_web_abi_write_memory,_mm_web_abi_load,_mm_web_abi_step,_mm_web_abi_run_slice,_mm_web_abi_run_until,_mm_web_abi_last_deadline_reached,_mm_web_abi_last_pad_changed,_mm_web_abi_cancel_gpio_inputs,_mm_web_abi_pad_state,_mm_web_abi_gpio_output_config,_mm_web_abi_has_pending_deadline,_mm_web_abi_reset,_mm_web_abi_gpio_input,_mm_web_abi_analog_input,_mm_web_abi_analog_point,_mm_web_abi_gpio_output_next,_mm_web_abi_gpio_output_pad,_mm_web_abi_gpio_output_level,_mm_web_abi_gpio_output_time_low,_mm_web_abi_gpio_output_time_high,_mm_web_abi_last_error,_mm_web_abi_last_instruction_count,_mm_web_abi_last_stopped,_mm_web_abi_last_system_reset,_mm_web_abi_last_lockup,_mm_web_abi_last_lockup_pc,_mm_web_abi_cpu_reg,_mm_web_abi_cycle_low,_mm_web_abi_cycle_high,_mm_web_abi_xpsr,_mm_web_abi_msp_s,_mm_web_abi_msp_ns,_mm_web_abi_psp_s,_mm_web_abi_psp_ns,_mm_web_abi_security_state,_mm_web_abi_mode,_mm_web_abi_sleeping,_mm_web_abi_cpu_hz,_mm_web_abi_virtual_time_low,_mm_web_abi_virtual_time_high,_mm_web_abi_destroy,_mm_web_abi_uart_count,_mm_web_abi_uart_label,_mm_web_abi_uart_base,_mm_web_abi_uart_read,_mm_web_abi_uart_overflow,_mm_web_abi_uart_write,_mm_web_abi_enable_bus_host,_mm_web_abi_attach_device,_mm_web_abi_detach_devices,_mm_web_abi_set_breakpoints,_mm_web_abi_last_breakpoint,_mm_web_abi_last_breakpoint_pc
  -sEXPORTED_RUNTIME_METHODS=cwrap,HEAPU8
  # Target RAM/flash backing ranges from a few hundred KiB (H5) to several
  # MiB (RP2350 flash, i.MX RT700 SRAM).
  -sALLOW_MEMORY_GROWTH=1
  -sMAXIMUM_MEMORY=1073741824
)
