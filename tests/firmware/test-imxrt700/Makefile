# i.MX RT700 emulator test firmware.
#
#   app.bin          XIP image: 16 KB FCB + CPU0 image at XSPI0 offset 0x4000
#   app-ramload.bin  same tests as a load-to-RAM image (ROM copies it to 0x30100000)
#
# Both embed the CPU1 image (core1/) and the non-secure test code (ns/).

CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy

CPU     := -mcpu=cortex-m33 -mthumb -mfloat-abi=soft
CFLAGS  := $(CPU) -Os -g -ffreestanding -fdata-sections -ffunction-sections -Wall -Wextra -I.
LDLIBS  := -Wl,--start-group -lc -lm -lgcc -lnosys -Wl,--end-group

APP_SRCS := main.c veneers.c ivt.c syscalls.c blobs.S

all: app.bin app-ramload.bin

core1/core1.elf: core1/main.c core1/core1.ld rt700.h
	$(CC) $(CFLAGS) -nostdlib -T core1/core1.ld -Wl,-gc-sections core1/main.c -lgcc -o $@

ns/ns.elf: ns/main.c ns/ns.ld
	$(CC) $(CFLAGS) -nostdlib -T ns/ns.ld -Wl,-gc-sections ns/main.c -lgcc -o $@

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@

app_xip.elf: $(APP_SRCS) app_xip.ld sections.ld rt700.h core1/core1.bin ns/ns.bin
	$(CC) $(CFLAGS) -mcmse -nostartfiles -T app_xip.ld -Wl,-gc-sections $(APP_SRCS) $(LDLIBS) -o $@

app_ram.elf: $(APP_SRCS) app_ram.ld sections.ld rt700.h core1/core1.bin ns/ns.bin
	$(CC) $(CFLAGS) -mcmse -nostartfiles -T app_ram.ld -Wl,-gc-sections $(APP_SRCS) $(LDLIBS) -o $@

# Flash configuration block: "FCFB" tag, padded to the 16 KB the ROM reserves.
fcb.bin:
	printf 'FCFB' > $@
	truncate -s 16384 $@

app.bin: fcb.bin app_xip.bin
	cat fcb.bin app_xip.bin > $@

app-ramload.bin: fcb.bin app_ram.bin
	cat fcb.bin app_ram.bin > $@

.PRECIOUS: %.elf core1/core1.bin ns/ns.bin

clean:
	rm -f *.o *.elf *.bin core1/*.elf core1/*.bin ns/*.elf ns/*.bin

.PHONY: all clean
