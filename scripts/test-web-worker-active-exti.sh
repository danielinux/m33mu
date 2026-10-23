#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture_dir="$repo_root/tests/firmware/test-stm32h563-web-active-exti"
arm_cc=${M33MU_ARM_NONE_EABI_GCC:-arm-none-eabi-gcc}
arm_nm=${M33MU_ARM_NONE_EABI_NM:-arm-none-eabi-nm}
"${MAKE:-make}" -C "$fixture_dir" CC="$arm_cc" app.elf
loop_pc=$($arm_nm -n "$fixture_dir/app.elf" |
  awk '$3 == "m33mu_web_active_exti_loop" { print "0x" $1; found = 1; exit }
       END { if (!found) exit 1 }')
M33MU_ACTIVE_EXTI_FIRMWARE="$fixture_dir/app.elf" \
M33MU_ACTIVE_EXTI_LOOP_PC="$loop_pc" \
  "$repo_root/scripts/test-web-worker.sh"
