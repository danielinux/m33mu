cmake_minimum_required(VERSION 3.20)

project(m33mu LANGUAGES C)

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
  "${CMAKE_SOURCE_DIR}/cpu/*/*.c"
  "${CMAKE_SOURCE_DIR}/tui/*.c"
)

# Build library = everything except main (like OBJ_NO_MAIN)
set(M33MU_MAIN_SRC "${CMAKE_SOURCE_DIR}/src/main.c")
list(REMOVE_ITEM M33MU_SOURCES "${M33MU_MAIN_SRC}")
set(M33MU_CAPSTONE_SRC "${CMAKE_SOURCE_DIR}/src/m33mu/capstone.c")
set(M33MU_CAPSTONE_STUB_SRC "${CMAKE_SOURCE_DIR}/src/m33mu/capstone_stub.c")
set(M33MU_TPM_TIS_SRC "${CMAKE_SOURCE_DIR}/src/m33mu/tpm_tis.c")
set(M33MU_TERMBOX2_SRC "${CMAKE_SOURCE_DIR}/tui/termbox2.c")
set(M33MU_TUI_SRC "${CMAKE_SOURCE_DIR}/tui/tui.c")
set(M33MU_TUI_STUB_SRC "${CMAKE_SOURCE_DIR}/tui/tui_stub.c")
list(REMOVE_ITEM M33MU_SOURCES "${M33MU_TERMBOX2_SRC}")
list(REMOVE_ITEM M33MU_SOURCES "${M33MU_TUI_SRC}")
list(REMOVE_ITEM M33MU_SOURCES "${M33MU_TUI_STUB_SRC}")

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
# 4) ncurses detection (TUI)
# -----------------------------------------------------------------------------
set(M33MU_HAS_NCURSES FALSE)
set(M33MU_USE_NCURSESW FALSE)

if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_NCURSESW QUIET ncursesw)
endif()

if(PC_NCURSESW_FOUND)
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
# Core library (everything except main)
# -----------------------------------------------------------------------------
add_library(m33mu_lib STATIC ${M33MU_SOURCES})

target_include_directories(m33mu_lib PUBLIC ${M33MU_INCLUDE_DIRS})

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

# Optional dependency wiring
if(M33MU_HAS_CAPSTONE)
  target_compile_definitions(m33mu_lib PUBLIC M33MU_USE_LIBCAPSTONE=1)
  target_link_libraries(m33mu_lib PUBLIC Capstone::capstone)
endif()

if(M33MU_HAS_LIBTPMS)
  target_compile_definitions(m33mu_lib PUBLIC M33MU_HAS_LIBTPMS=1)
  target_link_libraries(m33mu_lib PUBLIC libtpms::tpms)
endif()

if(M33MU_HAS_NCURSES)
  target_compile_definitions(m33mu_lib PUBLIC M33MU_HAS_NCURSES=1)
  if(M33MU_USE_NCURSESW)
    target_compile_definitions(m33mu_lib PUBLIC M33MU_USE_NCURSESW=1)
    target_include_directories(m33mu_lib PUBLIC ${PC_NCURSESW_INCLUDE_DIRS})
    target_link_libraries(m33mu_lib PUBLIC ${PC_NCURSESW_LIBRARIES})
  else()
    target_include_directories(m33mu_lib PUBLIC ${CURSES_INCLUDE_DIRS})
    target_link_libraries(m33mu_lib PUBLIC ${CURSES_LIBRARIES})
  endif()
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
  endforeach()
endif()

# -----------------------------------------------------------------------------
# Firmware build/run automation (Makefile-based)
# -----------------------------------------------------------------------------
add_custom_target(firmware-build
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-stm32h563 app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-rtos-exceptions app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-systick-wfi app.bin
  COMMAND ${CMAKE_MAKE_PROGRAM} -C tests/firmware/test-tz-bxns-cmse-sau-mpu clean all
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(test-firmware
  COMMAND /bin/sh -c "$<TARGET_FILE:m33mu> tests/firmware/test-stm32h563/app.bin || true"
  COMMAND /bin/sh -c "$<TARGET_FILE:m33mu> tests/firmware/test-rtos-exceptions/app.bin || true"
  COMMAND /bin/sh -c "$<TARGET_FILE:m33mu> tests/firmware/test-systick-wfi/app.bin || true"
  COMMAND /bin/sh -c "$<TARGET_FILE:m33mu> tests/firmware/test-tz-bxns-cmse-sau-mpu/build/secure.bin tests/firmware/test-tz-bxns-cmse-sau-mpu/build/nonsecure.bin:0x2000 || true"
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
if(M33MU_HAS_NCURSES)
  set(M33MU_STATUS_NCURSES "${M33MU_COLOR_GREEN}detected${M33MU_COLOR_RESET}")
else()
  set(M33MU_STATUS_NCURSES "${M33MU_COLOR_RED}not found${M33MU_COLOR_RESET}")
endif()
message(STATUS "${M33MU_COLOR_YELLOW}  capstone: ${M33MU_STATUS_CAPSTONE}${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_YELLOW}  TPM/libtpms: ${M33MU_STATUS_TPM}${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_YELLOW}  ncurses: ${M33MU_STATUS_NCURSES}${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_YELLOW}  tests enabled: ${M33MU_BUILD_TESTS}${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_MAGENTA}  --------------------------------------------${M33MU_COLOR_RESET}")
message(STATUS "${M33MU_COLOR_YELLOW}  features: capstone=${M33MU_HAS_CAPSTONE} tpm=${M33MU_HAS_LIBTPMS} tui=${M33MU_HAS_NCURSES}${M33MU_COLOR_RESET}")

if(M33MU_CONFIG_WARNINGS)
  foreach(msg IN LISTS M33MU_CONFIG_WARNINGS)
    message(WARNING "${msg}")
  endforeach()
endif()
