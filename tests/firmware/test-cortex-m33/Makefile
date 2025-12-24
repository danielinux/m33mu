CC=arm-none-eabi-gcc
OBJCOPY ?= arm-none-eabi-objcopy

CFLAGS := -mcpu=cortex-m33 -mthumb -mcmse -Os -ffreestanding -fdata-sections -ffunction-sections -g -ggdb
ifeq ($(ISA_SWEEPER),1)
CFLAGS += -DISA_SWEEPER
endif
LDFLAGS := -nostdlib -T target.ld -Wl,-gc-sections

SRCS := ivt.c main.c syscalls.c
OBJS := $(SRCS:.c=.o)

all: app.bin

app.elf: $(OBJS) target.ld
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -Wl,--start-group -lc -lm -lgcc -lnosys -Wl,--end-group -o $@

app.bin: app.elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) app.elf app.bin

.PHONY: all clean
