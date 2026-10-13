CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy

# Cortex-M23 is ARMv8-M Baseline: no FPU, no -mfpu/-mfloat-abi.
CFLAGS := -mcpu=cortex-m23 -mthumb \
          -Os -g -ggdb \
          -ffreestanding -fdata-sections -ffunction-sections \
          -I.

LDFLAGS := -nostdlib -T target.ld -Wl,-gc-sections
LDLIBS  := -Wl,--start-group -lc -lm -lgcc -lnosys -Wl,--end-group

TESTS := test_uart test_fmc test_scu

COMMON_SRCS := ivt.c syscalls.c emu_board.c
COMMON_OBJS := $(COMMON_SRCS:.c=.o)

all: $(addsuffix .bin,$(TESTS))

%.elf: %.o $(COMMON_OBJS) target.ld
	$(CC) $(CFLAGS) $(COMMON_OBJS) $< $(LDFLAGS) $(LDLIBS) -o $@

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PRECIOUS: %.elf %.o

clean:
	rm -f *.o *.elf *.bin

.PHONY: all clean
