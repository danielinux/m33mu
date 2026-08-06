# Firmware for the --covdump happy path.
#
# NOT part of the default build or test targets: it needs a clang that can
# target bare-metal Thumb (and, for the full check, llvm-profdata/llvm-cov).
# Build and run it explicitly:
#
#   make -C tests/firmware/test-covdump check
#
# or, from a configured build tree:  cmake --build build --target covdump-test

# CC has a built-in default in make, so ?= would not take effect here.
ifeq ($(origin CC),default)
CC := clang
endif
TARGET  ?= thumbv8m.main-none-eabi
M33MU   ?= ../../../build/m33mu
CPU     ?= stm32h563

BASE_CFLAGS := --target=$(TARGET) -mcpu=cortex-m33 -Os -g -ffreestanding \
               -fno-builtin -nostdlib -nostdlibinc
# -fcoverage-mcdc is what puts the MC/DC bitmaps in __llvm_prf_bits.
# Only main.c is instrumented: ivt.c is what zeroes .bss, and the counters live
# there, so instrumenting the startup makes its own counters race the zero loop
# and leaves garbage in the profile. Same rule as the coverage campaign's lane,
# where only the file under measurement is built by clang.
COV_CFLAGS  := -fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc
# lld: the host ld cannot link this target.
LDFLAGS := --target=$(TARGET) -fuse-ld=lld -nostdlib -T target.ld \
           -Wl,--build-id=none -Wl,-e,Reset_Handler

SRCS := ivt.c main.c
OBJS := $(SRCS:.c=.o)

all: app.elf

app.elf: $(OBJS) target.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

main.o: main.c
	$(CC) $(BASE_CFLAGS) $(COV_CFLAGS) -c $< -o $@

ivt.o: ivt.c
	$(CC) $(BASE_CFLAGS) -c $< -o $@

# Run the firmware under the emulator and verify the dump.
check: app.elf
	$(M33MU) --cpu $(CPU) --expect-bkpt 0x7f --timeout 30 \
	         --covdump ./cov app.elf
	./verify.sh ./cov app.elf

clean:
	rm -f $(OBJS) app.elf cov.cnts.bin cov.bits.bin cov.json cov.profraw \
	      cov.profdata cov-export.json

.PHONY: all check clean
