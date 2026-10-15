CC=arm-none-eabi-gcc
OBJCOPY ?= arm-none-eabi-objcopy

COMMON := ../_signal_common

# h5f4 (default), h563, h533, u585 or l552; MAIN_SRC lets a test reuse another's main.c.
SIGNAL_TARGET ?= h5f4
MAIN_SRC ?= main.c

ifeq ($(SIGNAL_TARGET),h5f4)
LDSCRIPT := $(COMMON)/target.ld
TARGET_DEF :=
else ifeq ($(SIGNAL_TARGET),h563)
LDSCRIPT := $(COMMON)/target-h563.ld
TARGET_DEF :=
else ifeq ($(SIGNAL_TARGET),h533)
LDSCRIPT := $(COMMON)/target-h533.ld
TARGET_DEF :=
else ifeq ($(SIGNAL_TARGET),u585)
LDSCRIPT := $(COMMON)/target-u585.ld
TARGET_DEF := -DSIGNAL_TARGET_U585
else ifeq ($(SIGNAL_TARGET),l552)
LDSCRIPT := $(COMMON)/target-l552.ld
TARGET_DEF := -DSIGNAL_TARGET_L552
else
$(error unknown SIGNAL_TARGET $(SIGNAL_TARGET))
endif

CFLAGS := -mcpu=cortex-m33 -mthumb -mcmse -Os -ffreestanding -fdata-sections -ffunction-sections -g -ggdb -I$(COMMON) $(TARGET_DEF)
LDFLAGS := -nostdlib -T $(LDSCRIPT) -Wl,-gc-sections

OBJS := ivt.o syscalls.o main.o

all: app.bin

app.elf: $(OBJS) $(LDSCRIPT)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -Wl,--start-group -lc -lm -lgcc -lnosys -Wl,--end-group -o $@

app.bin: app.elf
	$(OBJCOPY) -O binary $< $@

ivt.o: $(COMMON)/ivt.c
	$(CC) $(CFLAGS) -c $< -o $@

syscalls.o: $(COMMON)/syscalls.c
	$(CC) $(CFLAGS) -c $< -o $@

main.o: $(MAIN_SRC) $(COMMON)/signal_test.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) app.elf app.bin

.PHONY: all clean
