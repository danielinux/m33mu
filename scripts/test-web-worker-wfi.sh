#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture_dir="$repo_root/tests/firmware/test-stm32h563-web-wfi-exti"
arm_cc=${M33MU_ARM_NONE_EABI_GCC:-arm-none-eabi-gcc}
"${MAKE:-make}" -C "$fixture_dir" CC="$arm_cc" app.elf
M33MU_WFI_FIRMWARE="$fixture_dir/app.elf" \
  "$repo_root/scripts/test-web-worker.sh"
