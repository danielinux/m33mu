TOOLPREFIX ?= arm-none-eabi-
CC := $(TOOLPREFIX)gcc
OBJCOPY := $(TOOLPREFIX)objcopy
OBJDUMP := $(TOOLPREFIX)objdump
SIZE := $(TOOLPREFIX)size

CPU_FLAGS := -mcpu=cortex-m33 -mthumb -mgeneral-regs-only

CFLAGS_COMMON := $(CPU_FLAGS) -ffreestanding -nostdlib -fno-builtin -O0 -g
CFLAGS_COMMON += -Wall -Wextra
CFLAGS_COMMON += -I.

LDFLAGS_COMMON :=

SECURE_ELF := build/secure.elf
SECURE_BIN := build/secure.bin
SECURE_IMPLIB := build/secure_cmse_import.o

NONSECURE_ELF := build/nonsecure.elf
NONSECURE_BIN := build/nonsecure.bin

.PHONY: all clean disasm

all: $(SECURE_ELF) $(NONSECURE_ELF) $(SECURE_BIN) $(NONSECURE_BIN)
	@$(SIZE) $(SECURE_ELF) $(NONSECURE_ELF)

build:
	@mkdir -p build

$(SECURE_ELF): build secure/secure_main.c secure/secure.ld common/memory_map.h
	$(CC) $(CFLAGS_COMMON) -mcmse \
	  -Wl,-Tsecure/secure.ld \
	  -Wl,--cmse-implib \
	  -Wl,--out-implib=$(SECURE_IMPLIB) \
	  -o $@ secure/secure_main.c -lgcc

$(NONSECURE_ELF): build nonsecure/nonsecure_main.c nonsecure/nonsecure.ld common/memory_map.h $(SECURE_IMPLIB)
	$(CC) $(CFLAGS_COMMON) \
	  -Wl,-Tnonsecure/nonsecure.ld \
	  -o $@ nonsecure/nonsecure_main.c $(SECURE_IMPLIB) -lgcc

$(SECURE_BIN): $(SECURE_ELF)
	$(OBJCOPY) -O binary $< $@

$(NONSECURE_BIN): $(NONSECURE_ELF)
	$(OBJCOPY) -O binary $< $@

disasm: $(SECURE_ELF) $(NONSECURE_ELF)
	@echo "== secure =="
	@$(OBJDUMP) -d $(SECURE_ELF) | head -n 120
	@echo "== nonsecure =="
	@$(OBJDUMP) -d $(NONSECURE_ELF) | head -n 120

clean:
	rm -rf build
