CC=arm-none-eabi-gcc
OBJCOPY ?= arm-none-eabi-objcopy

ROOT := ../test-stm32h563

CFLAGS := -mcpu=cortex-m33 -mthumb -mcmse -Os -ffreestanding -fdata-sections -ffunction-sections -g -ggdb
LDFLAGS := -nostdlib -T $(ROOT)/target.ld -Wl,-gc-sections

OBJS := ivt.o main.o

all: app.bin

app.elf: $(OBJS) $(ROOT)/target.ld
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -Wl,--start-group -lgcc -Wl,--end-group -o $@

app.bin: app.elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) app.elf app.bin

.PHONY: all clean
