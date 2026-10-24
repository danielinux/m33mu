# Secure element suite on NUCLEO-H563ZI header pins (m33mu-lab example):
# test-se-suite/main.c built with -DSE_NUCLEO_HEADERS.
CC=arm-none-eabi-gcc
OBJCOPY ?= arm-none-eabi-objcopy
SUITE := ../test-se-suite

CFLAGS := -mcpu=cortex-m33 -mthumb -mcmse -Os -ffreestanding -fdata-sections -ffunction-sections -g -ggdb -DSE_NUCLEO_HEADERS
LDFLAGS := -nostdlib -T $(SUITE)/target.ld -Wl,-gc-sections

OBJS := ivt.o main.o syscalls.o

all: app.bin

app.elf: $(OBJS) $(SUITE)/target.ld
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -Wl,--start-group -lc -lm -lgcc -lnosys -Wl,--end-group -o $@

app.bin: app.elf
	$(OBJCOPY) -O binary $< $@

%.o: $(SUITE)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) app.elf app.bin
