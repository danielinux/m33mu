# Makefile for m33mu

CC ?= cc

# Detect compiler support for -Wmixed-declarations; fall back to -Wdeclaration-after-statement.
SUPPORTS_MIXED_DECL := $(shell printf 'int main(void){return 0;}\n' | $(CC) -std=c90 -Wmixed-declarations -Werror -x c -c -o /dev/null - 2>/dev/null && echo yes)
MIXED_DECL_FLAG :=
ifeq ($(SUPPORTS_MIXED_DECL),yes)
MIXED_DECL_FLAG := -Wmixed-declarations
endif

STD_FLAGS := -std=c90
WARN_FLAGS := -Wall -Wextra -Werror -Wdeclaration-after-statement $(MIXED_DECL_FLAG)

override CFLAGS += $(STD_FLAGS) $(WARN_FLAGS)
override CFLAGS += -Iinclude -Icpu -Itui
override LDFLAGS += -lcapstone

DEPFLAGS = -MMD -MP -MF $(@:.o=.d)

BUILD_DIR := build
BIN_DIR := bin
TARGET := $(BIN_DIR)/m33mu

SRC := $(wildcard src/*.c src/m33mu/*.c cpu/*/*.c tui/*.c)
OBJ := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(filter src/%.c,$(SRC)))
OBJ += $(patsubst cpu/%.c,$(BUILD_DIR)/cpu/%.o,$(filter cpu/%.c,$(SRC)))
OBJ += $(patsubst tui/%.c,$(BUILD_DIR)/tui/%.o,$(filter tui/%.c,$(SRC)))
OBJ_NO_MAIN := $(filter-out $(BUILD_DIR)/main.o,$(OBJ))

TEST_SRC := $(wildcard tests/*.c)
TEST_OBJ := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(TEST_SRC))
TEST_NAMES := $(patsubst tests/%.c,%,$(TEST_SRC))
TEST_BINS := $(patsubst %,$(BIN_DIR)/%,$(TEST_NAMES))

FIRMWARE_DIR := tests/firmware
FIRMWARE_SIMPLE := test-cortex-m33 test-rtos-exceptions test-systick-wfi
FIRMWARE_SIMPLE_BINS := $(FIRMWARE_SIMPLE:%=$(FIRMWARE_DIR)/%/app.bin)
FIRMWARE_TZ_SEC := $(FIRMWARE_DIR)/test-tz-bxns-cmse-sau-mpu/build/secure.bin
FIRMWARE_TZ_NS := $(FIRMWARE_DIR)/test-tz-bxns-cmse-sau-mpu/build/nonsecure.bin
FIRMWARE_BINS := $(FIRMWARE_SIMPLE_BINS) $(FIRMWARE_TZ_SEC) $(FIRMWARE_TZ_NS)
FIRMWARE_TIMEOUT ?= 5

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(BIN_DIR)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/cpu/%.o: cpu/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/tui/%.o: tui/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/tui/termbox2.o: CFLAGS += -std=c99 -Wno-endif-labels -Wno-unused-function -Wno-comment -DTB_OPT_ATTR_W=32
$(BUILD_DIR)/tui/tui.o: CFLAGS += -std=c99 -Wno-endif-labels -Wno-comment -DTB_OPT_ATTR_W=32
$(BUILD_DIR)/m33mu/capstone.o: CFLAGS += -std=c99

$(BUILD_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BIN_DIR)/%: $(BUILD_DIR)/tests/%.o $(OBJ_NO_MAIN)
	@mkdir -p $(BIN_DIR)
	$(CC) $^ $(LDFLAGS) -o $@

test: $(TEST_BINS)
	@set -e; \
	for t in $(TEST_BINS); do \
		echo "$$t"; \
		"$$t"; \
	done

# Firmware builds (arm-none-eabi toolchain expected).
$(FIRMWARE_DIR)/%/app.bin:
	$(MAKE) -C $(dir $@) app.bin

$(FIRMWARE_TZ_SEC) $(FIRMWARE_TZ_NS):
	$(MAKE) -C $(FIRMWARE_DIR)/test-tz-bxns-cmse-sau-mpu all

.PHONY: firmware-build test-firmware test-m33

firmware-build: $(FIRMWARE_BINS)

test-firmware: $(TARGET) firmware-build
	@echo "Running firmware: test-cortex-m33"; \
	timeout $(FIRMWARE_TIMEOUT)s $(TARGET) $(FIRMWARE_DIR)/test-cortex-m33/app.bin || true; \
	echo "Running firmware: test-rtos-exceptions"; \
	timeout $(FIRMWARE_TIMEOUT)s $(TARGET) $(FIRMWARE_DIR)/test-rtos-exceptions/app.bin || true; \
	echo "Running firmware: test-systick-wfi"; \
	timeout $(FIRMWARE_TIMEOUT)s $(TARGET) $(FIRMWARE_DIR)/test-systick-wfi/app.bin || true; \
	echo "Running firmware: test-tz-bxns-cmse-sau-mpu"; \
	timeout $(FIRMWARE_TIMEOUT)s $(TARGET) $(FIRMWARE_TZ_SEC) $(FIRMWARE_TZ_NS):0x2000 || true

test-m33: $(TARGET) $(FIRMWARE_DIR)/test-cortex-m33/app.bin
	@echo "Running firmware: test-cortex-m33 with SPI flash"; \
	timeout $(FIRMWARE_TIMEOUT)s $(TARGET) $(FIRMWARE_DIR)/test-cortex-m33/app.bin \
	    --uart-stdout \
		--spiflash:SPI1:file=$(FIRMWARE_DIR)/test-cortex-m33/spi_flash.bin:size=2097152:mmap=0x60000000 || true

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	@for d in $(FIRMWARE_SIMPLE); do \
		$(MAKE) -C $(FIRMWARE_DIR)/$$d clean; \
	done
	@$(MAKE) -C $(FIRMWARE_DIR)/test-tz-bxns-cmse-sau-mpu clean

-include $(OBJ:.o=.d) $(TEST_OBJ:.o=.d)
