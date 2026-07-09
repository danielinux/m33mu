cmake_minimum_required(VERSION 3.20)

project(m33mu LANGUAGES C)

include(GNUInstallDirs)

# -----------------------------------------------------------------------------
# 1) Disallow in-source builds
# -----------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "In-source builds are not allowed.\n"
    "Please use an out-of-source build directory, e.g.:\n"
    "  cmake -S . -B build\n"
    "  cmake --build build\n"
    "Optionally run tests with:\n"
    "  ctest --test-dir build\n"
  )
endif()

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
option(M33MU_BUILD_TESTS        "Build tests in tests/"                 ON)
option(M33MU_ENABLE_CAPSTONE    "Enable capstone integration if found"  ON)
option(M33MU_ENABLE_TPM_LIBTPMS "Enable libtpms integration if found"   ON)
option(M33MU_ENABLE_WOLFSSL     "Enable wolfSSL integration if found"   ON)
option(M33MU_ENABLE_LIBDW       "Enable libdw/libdwfl symbolization if found" ON)
option(M33MU_ENABLE_RUST_PLUGINS "Enable Rust simulator plugins (ATECC608/SE050/STSAFE) if cargo is found" ON)

# -----------------------------------------------------------------------------
# Compiler flags (match Makefile)
# -----------------------------------------------------------------------------
include(CheckCCompilerFlag)


set(M33MU_COMMON_WARN_FLAGS
  -Wall -Wextra -Werror -Wdeclaration-after-statement
)
set(M33MU_COMMON_OPT_FLAGS
  -O2
)

# -----------------------------------------------------------------------------
# Include dirs (match Makefile)
# -----------------------------------------------------------------------------
set(M33MU_INCLUDE_DIRS
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/cpu"
  "${CMAKE_SOURCE_DIR}/tui"
)

# -----------------------------------------------------------------------------
# Source collection (match Makefile globs)
# -----------------------------------------------------------------------------
file(GLOB M33MU_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/*.c"
  "${CMAKE_SOURCE_DIR}/src/m33mu/*.c"
  "${CMAKE_SOURCE_DIR}/cpu/*.c"
  "${CMAKE_SOURCE_DIR}/cpu/*/*.c"
  "${CMAKE_SOURCE_DIR}/tui/*.c"
)

# Build library = everything except main (like OBJ_NO_MAIN)
set(M33MU_MAIN_SRC "${CMAKE_SOURCE_DIR}/src/main.c")
list(REMOVE_ITEM M33MU_SOURCES "${M33MU_MAIN_SRC}")
set(M33MU_CAPSTONE_SRC "${CMAKE_SOURCE_DIR}/src/m33mu/capstone.c")
set(M33MU_CAPSTONE_STUB_SRC "${CMAKE_SOURCE_DIR}/src/m33mu/capstone_stub.c")
set(M33MU_TPM_TIS_SRC "${CMAKE_SOURCE_DIR}/src/m33mu/tpm_tis.c")
set(M33MU_UNICORN_SRC "${CMAKE_SOURCE_DIR}/src/m33mu/unicorn.c")
set(M33MU_UNICORN_STUB_SRC "${CMAKE_SOURCE_DIR}/src/m33mu/unicorn_stub.c")
set(M33MU_TERMBOX2_SRC "${CMAKE_SOURCE_DIR}/tui/termbox2.c")
set(M33MU_TUI_SRC "${CMAKE_SOURCE_DIR}/tui/tui.c")
set(M33MU_TUI_STUB_SRC "${CMAKE_SOURCE_DIR}/tui/tui_stub.c")
list(REMOVE_ITEM M33MU_SOURCES "${M33MU_TERMBOX2_SRC}")
list(REMOVE_ITEM M33MU_SOURCES "${M33MU_TUI_SRC}")
list(REMOVE_ITEM M33MU_SOURCES "${M33MU_TUI_STUB_SRC}")
list(REMOVE_ITEM M33MU_SOURCES "${M33MU_UNICORN_SRC}")
list(REMOVE_ITEM M33MU_SOURCES "${M33MU_UNICORN_STUB_SRC}")

# -----------------------------------------------------------------------------
# 2) Optional libcapstone detection + define
# -----------------------------------------------------------------------------
set(M33MU_CONFIG_WARNINGS "")
set(M33MU_HAS_CAPSTONE FALSE)

include(FindPkgConfig)

if(M33MU_ENABLE_CAPSTONE)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_CAPSTONE QUIET capstone)
  endif()

  find_path(CAPSTONE_INCLUDE_DIR
    NAMES capstone/capstone.h
    HINTS ${PC_CAPSTONE_INCLUDE_DIRS}
  )
  find_library(CAPSTONE_LIBRARY
    NAMES capstone
    HINTS ${PC_CAPSTONE_LIBRARY_DIRS}
  )

  if(CAPSTONE_INCLUDE_DIR AND CAPSTONE_LIBRARY)
    set(M33MU_HAS_CAPSTONE TRUE)
    add_library(Capstone::capstone UNKNOWN IMPORTED)
    set_target_properties(Capstone::capstone PROPERTIES
      IMPORTED_LOCATION "${CAPSTONE_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${CAPSTONE_INCLUDE_DIR}"
    )
    list(REMOVE_ITEM M33MU_SOURCES "${M33MU_CAPSTONE_STUB_SRC}")
  else()
    # If your code has a dedicated capstone translation unit that requires headers,
    # removing it here keeps the build working without capstone installed.
    list(REMOVE_ITEM M33MU_SOURCES "${M33MU_CAPSTONE_SRC}")

    list(APPEND M33MU_CONFIG_WARNINGS
      "libcapstone not found: disassembly/backtrace features will be limited. "
      "Install libcapstone to enable full bug reports."
    )
  endif()
else()
  list(REMOVE_ITEM M33MU_SOURCES "${M33MU_CAPSTONE_SRC}")
endif()

# -----------------------------------------------------------------------------
# 3) Optional libtpms detection + warning
# -----------------------------------------------------------------------------
set(M33MU_HAS_LIBTPMS FALSE)

if(M33MU_ENABLE_TPM_LIBTPMS)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_LIBTPMS QUIET libtpms)
  endif()

  # libtpms headers live under libtpms/
  find_path(LIBTPMS_INCLUDE_DIR
    NAMES libtpms/tpm_library.h
    HINTS ${PC_LIBTPMS_INCLUDE_DIRS}
  )
  # Common library name
  find_library(LIBTPMS_LIBRARY
    NAMES tpms
    HINTS ${PC_LIBTPMS_LIBRARY_DIRS}
  )

  if(LIBTPMS_INCLUDE_DIR AND LIBTPMS_LIBRARY)
    set(M33MU_HAS_LIBTPMS TRUE)
    add_library(libtpms::tpms UNKNOWN IMPORTED)
    set_target_properties(libtpms::tpms PROPERTIES
      IMPORTED_LOCATION "${LIBTPMS_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBTPMS_INCLUDE_DIR}"
    )
  else()
    list(REMOVE_ITEM M33MU_SOURCES "${M33MU_TPM_TIS_SRC}")
    list(APPEND M33MU_CONFIG_WARNINGS
      "libtpms not found: TPM emulation disabled."
    )
  endif()
else()
  list(REMOVE_ITEM M33MU_SOURCES "${M33MU_TPM_TIS_SRC}")
endif()

# -----------------------------------------------------------------------------
# 4) Optional libwolfssl detection + warning
# -----------------------------------------------------------------------------
set(M33MU_HAS_WOLFSSL FALSE)

if(M33MU_ENABLE_WOLFSSL)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_WOLFSSL QUIET wolfssl)
    set(_WOLFSSL_CFLAGS "${PC_WOLFSSL_CFLAGS_OTHER}")
    set(_WOLFSSL_LFLAGS "${PC_WOLFSSL_LDFLAGS_OTHER}")
  endif()

  if(_WOLFSSL_CFLAGS)
    string(REGEX MATCHALL "-I[^ ]+" _WOLFSSL_INCLUDE_FLAGS "${_WOLFSSL_CFLAGS}")
    foreach(_inc_flag IN LISTS _WOLFSSL_INCLUDE_FLAGS)
      string(REPLACE "-I" "" _inc_dir "${_inc_flag}")
      list(APPEND _WOLFSSL_INCLUDE_DIRS "${_inc_dir}")
    endforeach()
  endif()
  if(_WOLFSSL_LFLAGS)
    string(REGEX MATCHALL "-L[^ ]+" _WOLFSSL_LIBRARY_FLAGS "${_WOLFSSL_LFLAGS}")
    foreach(_lib_flag IN LISTS _WOLFSSL_LIBRARY_FLAGS)
      string(REPLACE "-L" "" _lib_dir "${_lib_flag}")
      list(APPEND _WOLFSSL_LIBRARY_DIRS "${_lib_dir}")
    endforeach()
  endif()
  if(EXISTS "/usr/include/wolfssl/options.h")
    list(PREPEND _WOLFSSL_INCLUDE_DIRS "/usr/include")
  endif()
  if(EXISTS "/usr/lib/x86_64-linux-gnu/libwolfssl.so" OR
     EXISTS "/usr/lib/x86_64-linux-gnu/libwolfssl.a")
    list(PREPEND _WOLFSSL_LIBRARY_DIRS "/usr/lib/x86_64-linux-gnu")
  endif()

  if(EXISTS "/usr/include/wolfssl/options.h")
    set(WOLFSSL_INCLUDE_DIR "/usr/include" CACHE PATH "" FORCE)
  endif()
  if(NOT WOLFSSL_INCLUDE_DIR)
    find_path(WOLFSSL_INCLUDE_DIR
      NAMES wolfssl/options.h wolfssl/ssl.h
      HINTS ${_WOLFSSL_INCLUDE_DIRS} ${PC_WOLFSSL_INCLUDE_DIRS}
    )
  endif()
  if(EXISTS "/usr/lib/x86_64-linux-gnu/libwolfssl.so")
    set(WOLFSSL_LIBRARY_SHARED "/usr/lib/x86_64-linux-gnu/libwolfssl.so" CACHE FILEPATH "" FORCE)
  else()
    find_library(WOLFSSL_LIBRARY_SHARED
      NAMES wolfssl
      HINTS ${_WOLFSSL_LIBRARY_DIRS} ${PC_WOLFSSL_LIBRARY_DIRS}
    )
  endif()
  set(_WOLFSSL_SUFFIXES_SAVE ${CMAKE_FIND_LIBRARY_SUFFIXES})
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
  if(EXISTS "/usr/lib/x86_64-linux-gnu/libwolfssl.a")
    set(WOLFSSL_LIBRARY_STATIC "/usr/lib/x86_64-linux-gnu/libwolfssl.a" CACHE FILEPATH "" FORCE)
  else()
    find_library(WOLFSSL_LIBRARY_STATIC
      NAMES wolfssl
      HINTS ${_WOLFSSL_LIBRARY_DIRS} ${PC_WOLFSSL_LIBRARY_DIRS}
    )
  endif()
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${_WOLFSSL_SUFFIXES_SAVE})
  unset(_WOLFSSL_SUFFIXES_SAVE)
  if(WOLFSSL_LIBRARY_SHARED)
    set(WOLFSSL_LIBRARY ${WOLFSSL_LIBRARY_SHARED})
  else()
    set(WOLFSSL_LIBRARY ${WOLFSSL_LIBRARY_STATIC})
  endif()

  if(WOLFSSL_INCLUDE_DIR AND WOLFSSL_LIBRARY)
    set(M33MU_HAS_WOLFSSL TRUE)
    add_library(wolfssl::wolfssl UNKNOWN IMPORTED)
    set_target_properties(wolfssl::wolfssl PROPERTIES
      IMPORTED_LOCATION "${WOLFSSL_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${WOLFSSL_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES "m"
    )
  else()
    list(APPEND M33MU_CONFIG_WARNINGS
      "libwolfssl not found: wolfSSL integration disabled."
    )
  endif()
endif()

# -----------------------------------------------------------------------------
# 5) Optional vde-2 detection
# -----------------------------------------------------------------------------
set(M33MU_HAS_VDE FALSE)
set(M33MU_HAS_VDE_SWITCH FALSE)
set(VDE_SWITCH_PROGRAM "")

if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_VDE QUIET vdeplug)
endif()

find_program(VDE_SWITCH_PROGRAM
  NAMES vde_switch
  HINTS /usr/bin /usr/local/bin
  NO_CMAKE_FIND_ROOT_PATH
)
if(NOT VDE_SWITCH_PROGRAM)
  if(EXISTS "/usr/bin/vde_switch")
    set(VDE_SWITCH_PROGRAM "/usr/bin/vde_switch")
  elseif(EXISTS "/usr/local/bin/vde_switch")
    set(VDE_SWITCH_PROGRAM "/usr/local/bin/vde_switch")
  endif()
endif()
if(VDE_SWITCH_PROGRAM)
  set(M33MU_HAS_VDE_SWITCH TRUE)
endif()

find_path(VDE_INCLUDE_DIR
  NAMES libvdeplug.h vdeplug.h
  HINTS ${PC_VDE_INCLUDE_DIRS}
)
find_library(VDE_LIBRARY
  NAMES vdeplug
  HINTS ${PC_VDE_LIBRARY_DIRS}
)

if(VDE_INCLUDE_DIR AND VDE_LIBRARY)
  set(M33MU_HAS_VDE TRUE)
  add_library(vdeplug::vdeplug UNKNOWN IMPORTED)
  set_target_properties(vdeplug::vdeplug PROPERTIES
    IMPORTED_LOCATION "${VDE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${VDE_INCLUDE_DIR}"
  )
elseif(M33MU_HAS_VDE_SWITCH)
  list(APPEND M33MU_CONFIG_WARNINGS
    "vde_switch found but libvdeplug headers/libs missing: VDE backend disabled."
  )
endif()
if(M33MU_HAS_VDE AND NOT M33MU_HAS_VDE_SWITCH)
  list(APPEND M33MU_CONFIG_WARNINGS
    "vde_switch not found: VDE backend may be unusable at runtime."
  )
endif()

# -----------------------------------------------------------------------------
# 6) ncurses detection (TUI)
# -----------------------------------------------------------------------------
set(M33MU_HAS_NCURSES FALSE)
set(M33MU_USE_NCURSESW FALSE)

if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_NCURSESW QUIET ncursesw)
endif()

find_path(NCURSESW_INCLUDE_DIR
  NAMES curses.h
  HINTS ${PC_NCURSESW_INCLUDE_DIRS}
)
find_library(NCURSESW_LIBRARY
  NAMES ncursesw cursesw
  HINTS ${PC_NCURSESW_LIBRARY_DIRS}
)

if(NCURSESW_INCLUDE_DIR AND NCURSESW_LIBRARY)
  set(M33MU_HAS_NCURSES TRUE)
  set(M33MU_USE_NCURSESW TRUE)
else()
  find_package(Curses)
  if(CURSES_FOUND)
    set(M33MU_HAS_NCURSES TRUE)
  endif()
endif()
if(M33MU_HAS_NCURSES)
  list(APPEND M33MU_SOURCES "${M33MU_TUI_SRC}")
else()
  list(APPEND M33MU_SOURCES "${M33MU_TUI_STUB_SRC}")
endif()

# -----------------------------------------------------------------------------
# 9) Optional Rust simulator plugins (ATECC608/SE050/STSAFE-A120/TROPIC01)
# -----------------------------------------------------------------------------
set(M33MU_HAS_RUST_PLUGINS FALSE)

set(MM_RUST_PLUGIN_SRCS
  "${CMAKE_SOURCE_DIR}/src/m33mu/atecc608.c"
  "${CMAKE_SOURCE_DIR}/src/m33mu/se050.c"
  "${CMAKE_SOURCE_DIR}/src/m33mu/stsafe.c"
  "${CMAKE_SOURCE_DIR}/src/m33mu/tropic01.c"
)

if(M33MU_ENABLE_RUST_PLUGINS)
  find_program(CARGO_EXECUTABLE cargo)
  if(CARGO_EXECUTABLE)
    set(M33MU_HAS_RUST_PLUGINS TRUE)
    include(cmake/RustPlugin.cmake)
    mm_rust_plugins_build()
  else()
    foreach(_f IN LISTS MM_RUST_PLUGIN_SRCS)
      list(REMOVE_ITEM M33MU_SOURCES "${_f}")
    endforeach()
    list(APPEND M33MU_CONFIG_WARNINGS
      "cargo not found: Rust simulator plugins (ATECC608, SE050, STSAFE-A120, TROPIC01) disabled."
    )
  endif()
else()
  foreach(_f IN LISTS MM_RUST_PLUGIN_SRCS)
    list(REMOVE_ITEM M33MU_SOURCES "${_f}")
  endforeach()
endif()

# -----------------------------------------------------------------------------
# Core library (everything except main)
# -----------------------------------------------------------------------------
add_library(m33mu_lib STATIC ${M33MU_SOURCES})

target_include_directories(m33mu_lib PUBLIC ${M33MU_INCLUDE_DIRS})
target_link_libraries(m33mu_lib PUBLIC m)

set_target_properties(m33mu_lib PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS OFF
)

target_compile_options(m33mu_lib PRIVATE ${M33MU_COMMON_WARN_FLAGS} ${M33MU_COMMON_OPT_FLAGS})
if(M33MU_HAS_CAPSTONE)
  set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/src/m33mu/capstone.c"
    PROPERTIES COMPILE_FLAGS "-std=c99"
  )
endif()

set_source_files_properties(
  "${CMAKE_SOURCE_DIR}/src/execute.c"
  PROPERTIES COMPILE_FLAGS "-std=gnu11"
)

# Optional dependency wiring
if(M33MU_HAS_CAPSTONE)
  target_compile_definitions(m33mu_lib PUBLIC M33MU_USE_LIBCAPSTONE=1)
  target_link_libraries(m33mu_lib PUBLIC Capstone::capstone)
endif()

if(M33MU_HAS_LIBTPMS)
  target_compile_definitions(m33mu_lib PUBLIC M33MU_HAS_LIBTPMS=1)
  target_link_libraries(m33mu_lib PUBLIC libtpms::tpms)
endif()

if(M33MU_HAS_WOLFSSL)
  target_compile_definitions(m33mu_lib PUBLIC M33MU_HAS_WOLFSSL=1)
  target_link_libraries(m33mu_lib PUBLIC wolfssl::wolfssl)
  target_compile_definitions(m33mu_lib PRIVATE WOLFSSL_SP_INT_NEGATIVE)
  target_include_directories(m33mu_lib SYSTEM BEFORE PRIVATE ${WOLFSSL_INCLUDE_DIR})
  target_compile_options(m33mu_lib PRIVATE -isystem ${WOLFSSL_INCLUDE_DIR})
endif()

if(M33MU_HAS_VDE)
  target_compile_definitions(m33mu_lib PUBLIC M33MU_HAS_VDE=1)
  target_link_libraries(m33mu_lib PUBLIC vdeplug::vdeplug)
endif()

if(M33MU_HAS_NCURSES)
  target_compile_definitions(m33mu_lib PUBLIC M33MU_HAS_NCURSES=1)
  if(M33MU_USE_NCURSESW)
    target_compile_definitions(m33mu_lib PUBLIC M33MU_USE_NCURSESW=1)
    target_include_directories(m33mu_lib PUBLIC ${NCURSESW_INCLUDE_DIR})
    target_link_libraries(m33mu_lib PUBLIC ${NCURSESW_LIBRARY})
    if(PC_NCURSESW_LDFLAGS_OTHER)
      target_link_options(m33mu_lib PUBLIC ${PC_NCURSESW_LDFLAGS_OTHER})
    endif()
  else()
    target_include_directories(m33mu_lib PUBLIC ${CURSES_INCLUDE_DIRS})
    target_link_libraries(m33mu_lib PUBLIC ${CURSES_LIBRARIES})
  endif()
endif()

# -----------------------------------------------------------------------------
# 7) Optional libelf detection (ELF loader)
# -----------------------------------------------------------------------------
set(M33MU_HAS_LIBELF FALSE)
find_path(LIBELF_INCLUDE_DIR
  NAMES libelf.h
)
find_library(LIBELF_LIBRARY
  NAMES elf
)
if(LIBELF_INCLUDE_DIR AND LIBELF_LIBRARY)
  set(M33MU_HAS_LIBELF TRUE)
  add_library(libelf::libelf UNKNOWN IMPORTED)
  set_target_properties(libelf::libelf PROPERTIES
    IMPORTED_LOCATION "${LIBELF_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBELF_INCLUDE_DIR}"
  )
  target_compile_definitions(m33mu_lib PUBLIC M33MU_HAS_LIBELF=1)
  target_link_libraries(m33mu_lib PUBLIC libelf::libelf)
else()
  list(APPEND M33MU_CONFIG_WARNINGS
    "libelf not found: ELF loading disabled (symbolization/call trace may be limited)."
  )
endif()

# -----------------------------------------------------------------------------
# 8) Optional libdw/libdwfl detection (symbolization)
# -----------------------------------------------------------------------------
set(M33MU_HAS_LIBDW FALSE)
if(M33MU_ENABLE_LIBDW)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_LIBDW QUIET libdw)
  endif()
  find_path(LIBDW_INCLUDE_DIR
    NAMES elfutils/libdwfl.h
    HINTS ${PC_LIBDW_INCLUDE_DIRS}
  )
  find_library(LIBDW_LIBRARY
    NAMES dw
    HINTS ${PC_LIBDW_LIBRARY_DIRS}
  )
  if(LIBDW_INCLUDE_DIR AND LIBDW_LIBRARY AND LIBELF_LIBRARY)
    set(M33MU_HAS_LIBDW TRUE)
    add_library(libdw::libdw UNKNOWN IMPORTED)
    set_target_properties(libdw::libdw PROPERTIES
      IMPORTED_LOCATION "${LIBDW_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBDW_INCLUDE_DIR}"
    )
  else()
  list(APPEND M33MU_CONFIG_WARNINGS
    "libdw/libdwfl not found: symbolization disabled (call trace + ELF symbol names unavailable)."
  )
  endif()
endif()

if(M33MU_HAS_LIBDW)
  target_compile_definitions(m33mu_lib PUBLIC M33MU_HAS_LIBDW=1)
  target_link_libraries(m33mu_lib PUBLIC libdw::libdw libelf::libelf)
endif()

if(M33MU_HAS_RUST_PLUGINS)
  target_compile_definitions(m33mu_lib PUBLIC M33MU_HAS_RUST_PLUGINS=1)
  target_link_libraries(m33mu_lib PUBLIC
    atecc608_sim se050_sim stsafe_a120_sim tropic01_sim)
  # Rust stdlib runtime requirements on Linux (m must follow Rust .a files)
  target_link_libraries(m33mu_lib PUBLIC pthread dl m)
endif()

# -----------------------------------------------------------------------------
# Executable
# -----------------------------------------------------------------------------
add_executable(m33mu "${CMAKE_SOURCE_DIR}/src/main.c")
target_link_libraries(m33mu PRIVATE m33mu_lib)
set_target_properties(m33mu PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS OFF
)
target_compile_options(m33mu PRIVATE ${M33MU_COMMON_WARN_FLAGS} ${M33MU_COMMON_OPT_FLAGS})

install(TARGETS m33mu
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES "${CMAKE_SOURCE_DIR}/m33mu.1"
  DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
)

# -----------------------------------------------------------------------------
# Tests (ctest)
# -----------------------------------------------------------------------------
if(M33MU_BUILD_TESTS)
  enable_testing()
  file(GLOB M33MU_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tests/*.c"
  )
  foreach(test_src IN LISTS M33MU_TEST_SOURCES)
    get_filename_component(test_name "${test_src}" NAME_WE)
    if("${test_name}" STREQUAL "tpm_nv_path_test" AND NOT M33MU_HAS_LIBTPMS)
      continue()
    endif()
    add_executable("${test_name}" "${test_src}")
    target_link_libraries("${test_name}" PRIVATE m33mu_lib)
    set_target_properties("${test_name}" PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED YES
      C_EXTENSIONS OFF
    )
    target_compile_options("${test_name}" PRIVATE ${M33MU_COMMON_WARN_FLAGS} ${M33MU_COMMON_OPT_FLAGS})
    add_test(NAME "${test_name}" COMMAND "${test_name}")
    set_tests_properties("${test_name}" PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    if("${test_name}" STREQUAL "decode_capstone_fuzz_full32_test")
      set_tests_properties("${test_name}" PROPERTIES DISABLED TRUE)
    endif()
    if("${test_name}" STREQUAL "ta100_nv_null_test")
      target_link_libraries("${test_name}" PRIVATE m)
    endif()
  endforeach()
endif()

# -----------------------------------------------------------------------------
# Firmware build/run automation (Makefile-based)
# -----------------------------------------------------------------------------
add_custom_target(firmware-build
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32h563 app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32h563-dualbank app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32u585-dualbank app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32u585-swap-reset app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32h563-uart-echo app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32h563-wolfssl-iotsafe app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32u585 app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32l552 app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-mcxw app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-mcxn947 app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-nrf5340 app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-nrf54lm20 app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-pic32ck all
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-pic32ck-sdk clean all
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-fpu-instructions app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-lpc55s69 app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-rtos-exceptions app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-rw612-sdk all
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-se-suite app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-ta100 app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-tz-bxns-cmse-sau-mpu clean all
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-lpc55s69-hashcrypt app.bin
  # Excluded: test-lpc55s69-sdk depends on an external NXP MCUXpresso SDK tree.
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32u585-otfdec app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-lpc55s69-casper app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-nrf5340-cryptocell app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32u585-wolfssl-pka app.bin
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(test-firmware
  COMMAND /bin/sh -c "$<TARGET_FILE:m33mu> tests/firmware/test-stm32h563/app.bin || true"
  COMMAND /bin/sh -c "$<TARGET_FILE:m33mu> tests/firmware/test-stm32h563-dualbank/app.bin --dualbank || true"
  COMMAND /bin/sh -c "$<TARGET_FILE:m33mu> --cpu stm32u585 tests/firmware/test-stm32u585-dualbank/app.bin --dualbank || true"
  COMMAND /bin/sh -c "$<TARGET_FILE:m33mu> --cpu stm32u585 tests/firmware/test-stm32u585-swap-reset/app.bin tests/firmware/test-stm32u585-swap-reset/app.bin:0x100000 --dualbank || true"
  COMMAND /bin/sh -c "$<TARGET_FILE:m33mu> tests/firmware/test-rtos-exceptions/app.bin || true"
  COMMAND /bin/sh -c "$<TARGET_FILE:m33mu> tests/firmware/test-tz-bxns-cmse-sau-mpu/build/secure.bin tests/firmware/test-tz-bxns-cmse-sau-mpu/build/nonsecure.bin:0x2000 || true"
  COMMAND /bin/sh -c "$<TARGET_FILE:m33mu> --cpu pic32ck tests/firmware/test-pic32ck-sdk/test_fcw.bin || true"
  COMMAND /bin/sh -c "$<TARGET_FILE:m33mu> --cpu pic32ck tests/firmware/test-pic32ck-sdk/test_idau.bin || true"
  DEPENDS m33mu firmware-build
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(test-stm32h5
  COMMAND $<TARGET_FILE:m33mu> tests/firmware/test-stm32h563/app.bin
          --uart-stdout
          --spiflash:SPI1:file=tests/firmware/test-stm32h563/spi_flash.bin:size=2097152:mmap=0x60000000:cs=PB0
          $<$<BOOL:${M33MU_HAS_LIBTPMS}>:--tpm:SPI1:cs=PB1:file=tests/firmware/test-stm32h563/tpm_nv>
  DEPENDS m33mu
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(test-stm32h5-tui
  COMMAND $<TARGET_FILE:m33mu> tests/firmware/test-stm32h563/app.bin --tui
          --spiflash:SPI1:file=tests/firmware/test-stm32h563/spi_flash.bin:size=2097152:mmap=0x60000000:cs=PB0
          $<$<BOOL:${M33MU_HAS_LIBTPMS}>:--tpm:SPI1:cs=PB1:file=tests/firmware/test-stm32h563/tpm_nv>
  DEPENDS m33mu
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(test-stm32u5
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32u585 app.bin
  COMMAND $<TARGET_FILE:m33mu> --cpu stm32u585 tests/firmware/test-stm32u585/app.bin
          --uart-stdout
          --spiflash:SPI1:file=tests/firmware/test-stm32u585/spi_flash.bin:size=2097152:mmap=0x60000000:cs=PB0
          $<$<BOOL:${M33MU_HAS_LIBTPMS}>:--tpm:SPI1:cs=PB1:file=tests/firmware/test-stm32u585/tpm_nv>
  DEPENDS m33mu
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(test-stm32u5-tui
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32u585 app.bin
  COMMAND $<TARGET_FILE:m33mu> --cpu stm32u585 tests/firmware/test-stm32u585/app.bin --tui
          --spiflash:SPI1:file=tests/firmware/test-stm32u585/spi_flash.bin:size=2097152:mmap=0x60000000:cs=PB0
          $<$<BOOL:${M33MU_HAS_LIBTPMS}>:--tpm:SPI1:cs=PB1:file=tests/firmware/test-stm32u585/tpm_nv>
  DEPENDS m33mu
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(test-stm32l5
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32l552 app.bin
  COMMAND $<TARGET_FILE:m33mu> --cpu stm32l552 tests/firmware/test-stm32l552/app.bin
          --uart-stdout
          --spiflash:SPI1:file=tests/firmware/test-stm32l552/spi_flash.bin:size=2097152:mmap=0x60000000:cs=PB0
          $<$<BOOL:${M33MU_HAS_LIBTPMS}>:--tpm:SPI1:cs=PB1:file=tests/firmware/test-stm32l552/tpm_nv>
  DEPENDS m33mu
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(test-stm32l5-tui
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32l552 app.bin
  COMMAND $<TARGET_FILE:m33mu> --cpu stm32l552 tests/firmware/test-stm32l552/app.bin --tui
          --spiflash:SPI1:file=tests/firmware/test-stm32l552/spi_flash.bin:size=2097152:mmap=0x60000000:cs=PB0
          $<$<BOOL:${M33MU_HAS_LIBTPMS}>:--tpm:SPI1:cs=PB1:file=tests/firmware/test-stm32l552/tpm_nv>
  DEPENDS m33mu
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(test-mcxw
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-mcxw app.bin
  COMMAND $<TARGET_FILE:m33mu> --cpu mcxw71c tests/firmware/test-mcxw/app.bin
          --uart-stdout
          --spiflash:SPI0:file=tests/firmware/test-mcxw/spi_flash.bin:size=2097152:mmap=0x01000000:cs=PA0
  DEPENDS m33mu
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(test-mcxw-tui
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-mcxw app.bin
  COMMAND $<TARGET_FILE:m33mu> --cpu mcxw71c tests/firmware/test-mcxw/app.bin --tui
          --spiflash:SPI0:file=tests/firmware/test-mcxw/spi_flash.bin:size=2097152:mmap=0x01000000:cs=PA0
  DEPENDS m33mu
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(test-mcxn947
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-mcxn947 app.bin
  COMMAND $<TARGET_FILE:m33mu> --cpu mcxn947 tests/firmware/test-mcxn947/app.bin
          --uart-stdout
  DEPENDS m33mu
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Running MCXN947 emulator test with FLEXCOMM peripherals"
)

add_custom_target(test-nrf5340
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-nrf5340 app.bin
  COMMAND $<TARGET_FILE:m33mu> --cpu nrf5340 tests/firmware/test-nrf5340/app.bin
          --uart-stdout
          --spiflash:SPI0:file=tests/firmware/test-nrf5340/spi_flash.bin:size=2097152:mmap=0x10000000:cs=PA0
  DEPENDS m33mu
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(test-nrf53-tui
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-nrf5340 app.bin
  COMMAND $<TARGET_FILE:m33mu> --cpu nrf5340 tests/firmware/test-nrf5340/app.bin --tui
          --spiflash:SPI0:file=tests/firmware/test-nrf5340/spi_flash.bin:size=2097152:mmap=0x10000000:cs=PA0
  DEPENDS m33mu
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
string(ASCII 27 M33MU_ESC)
set(M33MU_COLOR_YELLOW "${M33MU_ESC}[33m")
set(M33MU_COLOR_MAGENTA "${M33MU_ESC}[35m")
set(M33MU_COLOR_GREEN "${M33MU_ESC}[32m")
set(M33MU_COLOR_RED "${M33MU_ESC}[31m")
set(M33MU_COLOR_RESET "${M33MU_ESC}[0m")
message(STATUS "${M33MU_COLOR_YELLOW}m33mu configuration summary:${M33MU_COLOR_RESET}")
if(M33MU_HAS_CAPSTONE)
  set(M33MU_STATUS_CAPSTONE "${M33MU_COLOR_GREEN}detected${M33MU_COLOR_RESET}")
else()
  set(M33MU_STATUS_CAPSTONE "${M33MU_COLOR_RED}not found${M33MU_COLOR_RESET}")
endif()
if(M33MU_HAS_LIBTPMS)
  set(M33MU_STATUS_TPM "${M33MU_COLOR_GREEN}detected${M33MU_COLOR_RESET}")
else()
  set(M33MU_STATUS_TPM "${M33MU_COLOR_RED}not found${M33MU_COLOR_RESET}")
endif()
if(M33MU_HAS_WOLFSSL)
  set(M33MU_STATUS_WOLFSSL "${M33MU_COLOR_GREEN}detected${M33MU_COLOR_RESET}")
else()
  set(M33MU_STATUS_WOLFSSL "${M33MU_COLOR_RED}not found${M33MU_COLOR_RESET}")
endif()
if(M33MU_HAS_VDE)
  set(M33MU_STATUS_VDE "${M33MU_COLOR_GREEN}detected${M33MU_COLOR_RESET}")
else()
  set(M33MU_STATUS_VDE "${M33MU_COLOR_RED}not found${M33MU_COLOR_RESET}")
endif()
if(M33MU_HAS_VDE_SWITCH)
  set(M33MU_STATUS_VDE_SWITCH "${M33MU_COLOR_GREEN}vde_switch${M33MU_COLOR_RESET}")
else()
  set(M33MU_STATUS_VDE_SWITCH "${M33MU_COLOR_RED}no vde_switch${M33MU_COLOR_RESET}")
endif()
if(M33MU_HAS_NCURSES)
  set(M33MU_STATUS_NCURSES "${M33MU_COLOR_GREEN}detected${M33MU_COLOR_RESET}")
else()
  set(M33MU_STATUS_NCURSES "${M33MU_COLOR_RED}not found${M33MU_COLOR_RESET}")
endif()
if(M33MU_HAS_LIBELF)
  set(M33MU_STATUS_LIBELF "${M33MU_COLOR_GREEN}detected${M33MU_COLOR_RESET}")
else()
  set(M33MU_STATUS_LIBELF "${M33MU_COLOR_RED}not found${M33MU_COLOR_RESET}")
endif()
if(M33MU_HAS_LIBDW)
  set(M33MU_STATUS_LIBDW "${M33MU_COLOR_GREEN}detected${M33MU_COLOR_RESET}")
else()
  set(M33MU_STATUS_LIBDW "${M33MU_COLOR_RED}not found${M33MU_COLOR_RESET}")
endif()
message(STATUS "${M33MU_COLOR_YELLOW}  capstone: ${M33MU_STATUS_CAPSTONE}${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_YELLOW}  TPM/libtpms: ${M33MU_STATUS_TPM}${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_YELLOW}  wolfSSL: ${M33MU_STATUS_WOLFSSL}${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_YELLOW}  vde-2: ${M33MU_STATUS_VDE} (${M33MU_STATUS_VDE_SWITCH})${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_YELLOW}  ncurses: ${M33MU_STATUS_NCURSES}${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_YELLOW}  libelf: ${M33MU_STATUS_LIBELF}${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_YELLOW}  libdw/libdwfl: ${M33MU_STATUS_LIBDW}${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_YELLOW}  tests enabled: ${M33MU_BUILD_TESTS}${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_MAGENTA}  --------------------------------------------${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_YELLOW}  features: capstone=${M33MU_HAS_CAPSTONE} tpm=${M33MU_HAS_LIBTPMS} wolfssl=${M33MU_HAS_WOLFSSL} vde=${M33MU_HAS_VDE} tui=${M33MU_HAS_NCURSES} libelf=${M33MU_HAS_LIBELF} libdw=${M33MU_HAS_LIBDW}${M33MU_COLOR_RESET}")

if(M33MU_CONFIG_WARNINGS)
  foreach(msg IN LISTS M33MU_CONFIG_WARNINGS)
    message(WARNING "${msg}")
  endforeach()
endif()
