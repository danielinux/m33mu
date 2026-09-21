
  ────────────────────────────────────────
  ▸ todo_write todo
    content: - [ ] Phase 1: Enumeration & Mapping
  - [ ] Enumerate all source and test files in scope.
  - [ ] Map decode tables, opcode dispatch, and instruction logic.
  - [ ] Identify key flag update (N, Z, C, V, Q, GE) and PC-related logic.
- [ ] Phase 2: Deep Analysis (Instruction Path Fidelity)
  - [ ] Check 16-bit vs 32-bit Thumb decode.
  - [ ] Verify flag updates and APSR side effects.
  - [ ] Verify PC semantics (PC-relative, branches, pipeline offset).
  - [ ] Verify arithmetic/logic edge cases (sign/zero extend, saturation, shifts).
  - [ ] Check for incorrect UNPREDICTABLE/UNDEFINED handling.
  - [ ] Check for missing precondition/privilege/alignment enforcement.
- [ ] Phase 3: Verification & Synthesis
  - [ ] Cross-check suspicious code with existing tests.
  - [ ] Document findings in the required Markdown format.
  - [ ] Final Review.


  ────────────────────────────────────────
  ▸ shell
    command: ls -R /home/gus/src/m33mu/src/ /home/gus/src/m33mu/include/ /home/gus/src/m33mu/tests/

/home/gus/src/m33mu/include/:
m33mu

/home/gus/src/m33mu/include/m33mu:
capstone.h
chario.h
code_cache.h
core_sys.h
cpu_db.h
cpu.h
decode.h
dma.h
dsp_helpers.h
eth_backend.h
exception.h
exc_return.h
exec_helpers.h
execute.h
fetch.h
flash_persist.h
gdbstub.h
gpio.h
irq.h
mem.h
memmap.h
mem_prot.h
mmio.h
mpu.h
nvic.h
otp.h
pka.h
runloop.h
sau.h
scheduler.h
scs.h
snapshot.h
spi_bus.h
spiflash.h
ta100.h
table_branch.h
target.h
target_hal.h
timer.h
tpm_tis.h
trace.h
tt.h
types.h
tz.h
undo.h
usbdev.h
vector.h

/home/gus/src/m33mu/src/:
core_interfaces.c
core_sys.c
cpu_db.c
exc_return.c
execute.c
flash_persist.c
gdbstub.c
m33mu
main.c
memmap.c
nvic.c
otp.c
runloop.c
scs.c
snapshot.c
system_reset.c
target_hal.c
timer.c
trace.c
vector.c

/home/gus/src/m33mu/src/m33mu:
capstone.c
capstone_stub.c
code_cache.c
cpu.c
decode.c
eth_backend.c
exception.c
exec_helpers.c
fetch.c
mem.c
mem_prot.c
mpu.c
pka.c
sau.c
spi_bus.c
spiflash.c
ta100.c
ta100.c.orig
table_branch.c
tpm_tis.c
tt.c
tz.c
usbdev.c

/home/gus/src/m33mu/tests/:
cpu_banks_test.c
decode_capstone_alu_test.c
decode_capstone_branch_test.c
decode_capstone_fuzz_full32_test.c
decode_phase1_test.c
decode_phase2_test.c
decode_phase3_test.c
exception_vector_test.c
exclusive_monitor_test.c
exc_return_test.c
exec_capstone_alu_flags_test.c
exec_compliance_regressions_test.c
exec_curve_load_alu_test.c
exec_div_test.c
exec_dsp_qflag_test.c
exec_ecc_alu_chain_test.c
exec_ecc_cmp_repro_test.c
exec_ed25519_ctx_sanity_test.c
exec_fpu_convert_test.c
exec_helper_test.c
exec_it_lsl_imm_flags_test.c
exec_it_thumb1_alias_flags_matrix_test.c
exec_it_thumb1_raw_alu_flags_test.c
exec_ldr_pc_jump_table_test.c
exec_longmul_test.c
exec_mem_boundary_test.c
exec_sp_clamp_ct_test.c
exec_sp_limit_test.c
exec_verify_authenticity_test.c
fetch_test.c
firmware
gdbstub_packet_test.c
memmap_test.c
mpu_test.c
nvic_test.c
otp_test.c
prot_sau_nsc_test.c
runloop_test.c
sau_test.c
scs_test.c
snapshot_test.c
stm32_spi_test.c
stm32_timers_test.c
ta100_nv_null_test.c
table_branch_test.c
tpm_nv_path_test.c
tt_test.c
tz_branch_test.c
uart_pty_test.c
vectors
vector_test.c

/home/gus/src/m33mu/tests/firmware:
bench-speed
test-fpu-instructions
test-lpc55s69
test-lpc55s69-sdk
test-mcxn947
test-mcxw
test-nrf5340
test-nrf54lm20
test-pic32ck
test-pic32ck-sdk
test-rtos-exceptions
test-stm32h563
test-stm32h563-dualbank
test-stm32h563-uart-echo
test-stm32l552
test-stm32u585
test-ta100
test-tz-bxns-cmse-sau-mpu

/home/gus/src/m33mu/tests/firmware/bench-speed:
bench.c
ivt.c
Makefile
target.ld

/home/gus/src/m33mu/tests/firmware/test-fpu-instructions:
ivt.c
main.c
Makefile
target.ld

/home/gus/src/m33mu/tests/firmware/test-lpc55s69:
ivt.c
main.c
Makefile
syscalls.c
target.ld

/home/gus/src/m33mu/tests/firmware/test-lpc55s69-sdk:
emu_board.c
ivt.c
Makefile
syscalls.c
target.ld
test_crc.c
test_iap.c
test_mrt.c

/home/gus/src/m33mu/tests/firmware/test-mcxn947:
ivt.c
main.c
Makefile
syscalls.c
target.ld

/home/gus/src/m33mu/tests/firmware/test-mcxw:
ivt.c
main.c
Makefile
syscalls.c
target.ld

/home/gus/src/m33mu/tests/firmware/test-nrf5340:
ivt.c
main.c
Makefile
syscalls.c
target.ld

/home/gus/src/m33mu/tests/firmware/test-nrf54lm20:
ivt.c
main.c
Makefile
syscalls.c
target.ld

/home/gus/src/m33mu/tests/firmware/test-pic32ck:
emu_board.c
ivt.c
Makefile
syscalls.c
target.ld
test_uart.c

/home/gus/src/m33mu/tests/firmware/test-pic32ck-sdk:
emu_board.c
ivt.c
Makefile
syscalls.c
target.ld
test_fcw.c
test_gpio.c
test_idau.c
test_tcc.c
test_trng.c
test_uart.c

/home/gus/src/m33mu/tests/firmware/test-rtos-exceptions:
ivt.c
Makefile
rtos_exception_test.c
target.ld

/home/gus/src/m33mu/tests/firmware/test-stm32h563:
isa_sweeper.c
ivt.c
main.c
Makefile
syscalls.c
target.ld

/home/gus/src/m33mu/tests/firmware/test-stm32h563-dualbank:
ivt.c
main.c
Makefile
syscalls.c
target.ld

/home/gus/src/m33mu/tests/firmware/test-stm32h563-uart-echo:
ivt.c
main.c
Makefile

/home/gus/src/m33mu/tests/firmware/test-stm32l552:
isa_sweeper.c
ivt.c
main.c
Makefile
syscalls.c
target.ld

/home/gus/src/m33mu/tests/firmware/test-stm32u585:
isa_sweeper.c
ivt.c
main.c
Makefile
syscalls.c
target.ld

/home/gus/src/m33mu/tests/firmware/test-ta100:
ivt.c
main.c
Makefile
run_test.sh
syscalls.c
target.ld
test_simple.c

/home/gus/src/m33mu/tests/firmware/test-tz-bxns-cmse-sau-mpu:
common
Makefile
memory_map.h
nonsecure
secure

/home/gus/src/m33mu/tests/firmware/test-tz-bxns-cmse-sau-mpu/common:
memory_map.h

/home/gus/src/m33mu/tests/firmware/test-tz-bxns-cmse-sau-mpu/nonsecure:
nonsecure.ld
nonsecure_main.c

/home/gus/src/m33mu/tests/firmware/test-tz-bxns-cmse-sau-mpu/secure:
secure.ld
secure_main.c

/home/gus/src/m33mu/tests/vectors:
alu_capstone_flags_vectors.txt
alu_capstone_vectors.txt
branch_capstone_vectors.txt
  ────────────────────────────────────────
  ▸ shell
    command: grep -rnE "TODO|FIXME|HACK|XXX|unpredict|undef|thumb|it|carry|overflow|saturat" /home/gus/src/m33mu/src/m33mu/ /home/gus/src/m33mu/src/ /home/gus/src/m33mu/include/m33mu/ | grep -v "tests/" | head -n 50

/home/gus/src/m33mu/src/m33mu/tpm_tis.c:64:    mm_bool wait_phase;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:66:    mm_bool locality_active;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:83:    static mm_bool init = MM_FALSE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:85:    if (!init) {
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:88:        init = MM_TRUE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:95:    static mm_bool init = MM_FALSE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:97:    if (!init) {
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:100:        init = MM_TRUE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:132:        fprintf(stderr, "[TPM_TRACE]   ... (%lu bytes omitted)\n",
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:140:    mm_u32 mode_bits;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:148:    mode_bits = (moder >> (tpm->cs_pin * 2)) & 0x3u;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:149:    if (mode_bits != 1u) {
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:216:static void tpm_nv_sanitize(char *out, size_t out_len, const char *name)
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:239:    tpm_nv_sanitize(clean, sizeof(clean), name);
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:270:static TPM_RESULT tpm_nvram_init(void)
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:354:            size_t n = fwrite(data, 1u, length, f);
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:388:static TPM_RESULT tpm_io_init(void)
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:393:static TPM_RESULT tpm_io_getlocality(TPM_MODIFIER_INDICATOR *localityModifier,
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:397:    if (localityModifier == 0) return TPM_FAIL;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:398:    *localityModifier = 0;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:419:    tpm->wait_phase = MM_FALSE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:421:    tpm->locality_active = MM_FALSE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:431:    switch (addr) {
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:433:        return (mm_u8)(TPM_ACCESS_VALID | (tpm->locality_active ? TPM_ACCESS_ACTIVE : 0));
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:529:static void tpm_write_reg(struct mm_tpm_tis *tpm, mm_u16 addr, mm_u8 value)
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:534:            tpm->locality_active = MM_TRUE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:536:            tpm->locality_active = MM_FALSE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:588:            tpm->wait_phase = MM_TRUE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:593:    if (tpm->wait_phase) {
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:599:            tpm->wait_phase = MM_FALSE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:614:                tpm->wait_phase = MM_FALSE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:620:    tpm_write_reg(tpm, tpm->addr, out);
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:628:            tpm->wait_phase = MM_FALSE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:710:    static mm_bool libtpms_init = MM_FALSE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:712:    if (!libtpms_init) {
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:714:        libtpms_init = MM_TRUE;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:747:    callbacks.tpm_nvram_init = tpm_nvram_init;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:751:    callbacks.tpm_io_init = tpm_io_init;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:752:    callbacks.tpm_io_getlocality = tpm_io_getlocality;
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:755:    if (TPMLIB_MainInit() != TPM_SUCCESS) {
/home/gus/src/m33mu/src/m33mu/tpm_tis.c:756:        fprintf(stderr, "[TPM] TPMLIB_MainInit failed\n");
/home/gus/src/m33mu/src/m33mu/usbdev.c:101:    mm_bool descriptors_written;
/home/gus/src/m33mu/src/m33mu/usbdev.c:103:    mm_bool logged_wait_irq;
/home/gus/src/m33mu/src/m33mu/usbdev.c:195:    switch (index) {
/home/gus/src/m33mu/src/m33mu/usbdev.c:482:        switch (type) {
/home/gus/src/m33mu/src/m33mu/usbdev.c:517:                    if (!isxdigit((unsigned char)*p)) continue;
/home/gus/src/m33mu/src/m33mu/usbdev.c:569:            if (write(fd, cfg, cfg_len) < 0) {
/home/gus/src/m33mu/src/m33mu/usbdev.c:570:                usb_trace("endpoint desc write failed path=%s errno=%d", path, errno);
/home/gus/src/m33mu/src/m33mu/usbdev.c:605:    switch (g_usb.desc_state) {
/home/gus/src/m33mu/src/m33mu/usbdev.c:729:            ssize_t n = write(g_usb.ep0_fd,
  ────────────────────────────────────────
  ▸ shell
    command: grep -rnE "unpredict|undef|thumb|it|carry|overflow|saturat" /home/gus/src/m33mu/src/m33mu/ /home/gus/src/m33mu/src/ /home/gus/src/m33mu/include/m33mu/ | grep -v "tests/" | grep -v "tpm_tis.c" | grep -v "usbdev.c"

/home/gus/src/m33mu/include/m33mu/gdbstub.h:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/include/m33mu/gdbstub.h:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/include/m33mu/gdbstub.h:38:    mm_bool request_quit;
/home/gus/src/m33mu/include/m33mu/gdbstub.h:56:void mm_gdb_stub_init(struct mm_gdb_stub *stub);
/home/gus/src/m33mu/include/m33mu/gdbstub.h:58:mm_bool mm_gdb_stub_wait_client(struct mm_gdb_stub *stub);
/home/gus/src/m33mu/include/m33mu/gdbstub.h:59:mm_bool mm_gdb_stub_wait_client_blocking(struct mm_gdb_stub *stub);
/home/gus/src/m33mu/include/m33mu/gdbstub.h:66:mm_bool mm_gdb_stub_breakpoint_hit(const struct mm_gdb_stub *stub, mm_u32 pc);
/home/gus/src/m33mu/include/m33mu/gdbstub.h:72:mm_bool mm_gdb_stub_take_quit(struct mm_gdb_stub *stub);
/home/gus/src/m33mu/include/m33mu/mpu.h:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/include/m33mu/mpu.h:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/include/m33mu/mpu.h:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/include/m33mu/mpu.h:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/include/m33mu/mpu.h:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/include/m33mu/mpu.h:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/include/m33mu/flash_persist.h:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/include/m33mu/flash_persist.h:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/include/m33mu/flash_persist.h:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/include/m33mu/flash_persist.h:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/include/m33mu/flash_persist.h:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/include/m33mu/flash_persist.h:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/include/m33mu/fetch.h:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/include/m33mu/fetch.h:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/include/m33mu/fetch.h:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/include/m33mu/fetch.h:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/include/m33mu/fetch.h:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/include/m33mu/fetch.h:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/include/m33mu/fetch.h:32:    mm_u32 insn;       /* instruction word; if 32-bit, hw1 is high half */
/home/gus/src/m33mu/include/m33mu/fetch.h:36:    mm_u32 pc_fetch;   /* address used for this fetch (bit0 cleared) */
/home/gus/src/m33mu/include/m33mu/fetch.h:40:mm_bool t32_is_32bit_prefix(mm_u16 prefix);
/home/gus/src/m33mu/include/m33mu/target_hal.h:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/include/m33mu/target_hal.h:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/include/m33mu/target_hal.h:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/include/m33mu/target_hal.h:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/include/m33mu/target_hal.h:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/include/m33mu/target_hal.h:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/include/m33mu/target_hal.h:39:void mm_uart_io_init(struct mm_uart_io *io);
/home/gus/src/m33mu/include/m33mu/target_hal.h:61:void mm_target_usart_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic);
/home/gus/src/m33mu/include/m33mu/target_hal.h:64:void mm_target_spi_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic);
/home/gus/src/m33mu/include/m33mu/target_hal.h:68:void mm_target_eth_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic);
/home/gus/src/m33mu/include/m33mu/otp.h:17:    mm_bool write_enabled;
/home/gus/src/m33mu/include/m33mu/otp.h:21:void mm_otp_init(struct mm_otp *otp, const char *target_name, mm_u32 size);
/home/gus/src/m33mu/include/m33mu/otp.h:23:mm_bool mm_otp_write(struct mm_otp *otp, mm_u32 offset, const mm_u8 *src, mm_u32 len);
/home/gus/src/m33mu/include/m33mu/otp.h:27:void mm_otp_set_write_enabled(struct mm_otp *otp, mm_bool enabled);
/home/gus/src/m33mu/include/m33mu/gpio.h:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/include/m33mu/gpio.h:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/include/m33mu/gpio.h:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/include/m33mu/gpio.h:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/include/m33mu/gpio.h:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/include/m33mu/gpio.h:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/include/m33mu/gpio.h:45:void mm_gpio_line_init(struct mm_gpio_line *line, mm_gpio_listener_fn listener, void *opaque);

[Output exceeded 2000 line limit (3626 lines total). Full output saved to /tmp/.tmp1iV4DK/output-2. Read it with shell commands like `head`, `tail`, or `sed -n '100,200p'` up to 2000 lines at a time.]
  ────────────────────────────────────────
  ▸ shell
    command: grep -rnE "(unpredict|undef|thumb|it|carry|overflow|saturat|it_prefix)" /home/gus/src/m33mu/src/m33mu/ /home/gus/src/m33mu/src/ /home/gus/src/m33mu/include/m33mu/ | grep -v "tests/" | grep -vE "(tpm_tis.c|usbdev.c)" | head -n 50

/home/gus/src/m33mu/src/m33mu/cpu.c:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/src/m33mu/cpu.c:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/src/m33mu/cpu.c:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/src/m33mu/cpu.c:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/src/m33mu/cpu.c:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/src/m33mu/cpu.c:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/src/m33mu/cpu.c:32:static void cpu_init_msp_top(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 value)
/home/gus/src/m33mu/src/m33mu/cpu.c:69:    cpu_init_msp_top(cpu, sec, value);
/home/gus/src/m33mu/src/m33mu/cpu.c:109:        cpu_init_msp_top(cpu, cpu->sec_state, value);
/home/gus/src/m33mu/src/m33mu/cpu.c:156:        cpu_init_msp_top(cpu, sec, value);
/home/gus/src/m33mu/src/m33mu/cpu.c:255:void mm_cpu_set_security(struct mm_cpu *cpu, enum mm_sec_state sec)
/home/gus/src/m33mu/src/m33mu/cpu.c:262:enum mm_sec_state mm_cpu_get_security(const struct mm_cpu *cpu)
/home/gus/src/m33mu/src/m33mu/cpu.c:330:    /* Both success and failure clear the local exclusive monitor. */
/home/gus/src/m33mu/src/m33mu/pka.c:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/src/m33mu/pka.c:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/src/m33mu/pka.c:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/src/m33mu/pka.c:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/src/m33mu/pka.c:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/src/m33mu/pka.c:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/src/m33mu/pka.c:207:static void pka_write_u64(struct pka_state *pka, mm_u32 offset, mm_u64 value)
/home/gus/src/m33mu/src/m33mu/pka.c:243:static void pka_write_bytes_be(struct pka_state *pka, mm_u32 offset, const mm_u8 *src, mm_u32 size)
/home/gus/src/m33mu/src/m33mu/pka.c:301:        ret = mp_init(&tp);
/home/gus/src/m33mu/src/m33mu/pka.c:303:        ret = mp_init(&ta);
/home/gus/src/m33mu/src/m33mu/pka.c:308:        ret = mp_init(&tb);
/home/gus/src/m33mu/src/m33mu/pka.c:314:        ret = mp_init(&tn);
/home/gus/src/m33mu/src/m33mu/pka.c:342:static mm_bool pka_write_mp(struct pka_state *pka, mm_u32 offset, mm_u32 size_bytes, const mp_int *in)
/home/gus/src/m33mu/src/m33mu/pka.c:356:    pka_write_bytes_be(pka, offset, buf, size_bytes);
/home/gus/src/m33mu/src/m33mu/pka.c:366:    if (mp_init(&tmp) != MP_OKAY) return;
/home/gus/src/m33mu/src/m33mu/pka.c:389:    pka_write_mp(pka, x_off, size_bytes, pt->x);
/home/gus/src/m33mu/src/m33mu/pka.c:390:    pka_write_mp(pka, y_off, size_bytes, pt->y);
/home/gus/src/m33mu/src/m33mu/pka.c:392:        pka_write_mp(pka, z_off, size_bytes, pt->z);
/home/gus/src/m33mu/src/m33mu/pka.c:399:    switch (mode) {
/home/gus/src/m33mu/src/m33mu/pka.c:437:static void pka_write_status_code(struct pka_state *pka, mm_u32 offset, mm_u32 code)
/home/gus/src/m33mu/src/m33mu/pka.c:439:    pka_write_u64(pka, offset, (mm_u64)code);
/home/gus/src/m33mu/src/m33mu/pka.c:453:    mm_u64 bits64;
/home/gus/src/m33mu/src/m33mu/pka.c:454:    mm_u32 bits;
/home/gus/src/m33mu/src/m33mu/pka.c:458:    mp_init(&a);
/home/gus/src/m33mu/src/m33mu/pka.c:459:    mp_init(&b);
/home/gus/src/m33mu/src/m33mu/pka.c:460:    mp_init(&n);
/home/gus/src/m33mu/src/m33mu/pka.c:461:    mp_init(&tmp1);
/home/gus/src/m33mu/src/m33mu/pka.c:462:    mp_init(&tmp2);
/home/gus/src/m33mu/src/m33mu/pka.c:463:    mp_init(&tmp3);
/home/gus/src/m33mu/src/m33mu/pka.c:464:    mp_init(&tmp4);
/home/gus/src/m33mu/src/m33mu/pka.c:467:    switch (mode) {
/home/gus/src/m33mu/src/m33mu/pka.c:469:        bits64 = pka_read_u32(pka, PKA_RAM_MOD_LEN);
/home/gus/src/m33mu/src/m33mu/pka.c:470:        bits = (mm_u32)bits64;
/home/gus/src/m33mu/src/m33mu/pka.c:471:        bytes = (bits + 7u) / 8u;
/home/gus/src/m33mu/src/m33mu/pka.c:473:        mp_2expt(&tmp1, (int)(bits * 2u));
/home/gus/src/m33mu/src/m33mu/pka.c:475:        pka_write_mp(pka, PKA_RAM_MONT_R2, (bits + 31u) / 32u * 4u, &tmp2);
/home/gus/src/m33mu/src/m33mu/pka.c:480:        bits64 = pka_read_u32(pka, PKA_RAM_OP_LEN);
  ────────────────────────────────────────
  ▸ shell
    command: grep -rnE "(unpredict|undef|thumb|it|carry|overflow|saturat|it_prefix)" /home/gus/src/m33mu/src/m33mu/ /home/gus/src/m33mu/src/ /home/gus/src/m33mu/include/m33mu/ | grep -v "tests/" | grep -vE "(tpm_tis.c|usbdev.c|pka.c|cpu.c)"

/home/gus/src/m33mu/include/m33mu/gdbstub.h:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/include/m33mu/gdbstub.h:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/include/m33mu/gdbstub.h:38:    mm_bool request_quit;
/home/gus/src/m33mu/include/m33mu/gdbstub.h:56:void mm_gdb_stub_init(struct mm_gdb_stub *stub);
/home/gus/src/m33mu/include/m33mu/gdbstub.h:58:mm_bool mm_gdb_stub_wait_client(struct mm_gdb_stub *stub);
/home/gus/src/m33mu/include/m33mu/gdbstub.h:59:mm_bool mm_gdb_stub_wait_client_blocking(struct mm_gdb_stub *stub);
/home/gus/src/m33mu/include/m33mu/gdbstub.h:66:mm_bool mm_gdb_stub_breakpoint_hit(const struct mm_gdb_stub *stub, mm_u32 pc);
/home/gus/src/m33mu/include/m33mu/gdbstub.h:72:mm_bool mm_gdb_stub_take_quit(struct mm_gdb_stub *stub);
/home/gus/src/m33mu/include/m33mu/mpu.h:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/include/m33mu/mpu.h:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/include/m33mu/mpu.h:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/include/m33mu/mpu.h:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/include/m33mu/mpu.h:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/include/m33mu/mpu.h:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/include/m33mu/flash_persist.h:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/include/m33mu/flash_persist.h:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/include/m33mu/flash_persist.h:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/include/m33mu/flash_persist.h:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/include/m33mu/flash_persist.h:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/include/m33mu/flash_persist.h:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/include/m33mu/fetch.h:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/include/m33mu/fetch.h:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/include/m33mu/fetch.h:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/include/m33mu/fetch.h:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/include/m33mu/fetch.h:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/include/m33mu/fetch.h:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/include/m33mu/fetch.h:32:    mm_u32 insn;       /* instruction word; if 32-bit, hw1 is high half */
/home/gus/src/m33mu/include/m33mu/fetch.h:36:    mm_u32 pc_fetch;   /* address used for this fetch (bit0 cleared) */
/home/gus/src/m33mu/include/m33mu/fetch.h:40:mm_bool t32_is_32bit_prefix(mm_u16 prefix);
/home/gus/src/m33mu/include/m33mu/target_hal.h:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/include/m33mu/target_hal.h:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/include/m33mu/target_hal.h:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/include/m33mu/target_hal.h:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/include/m33mu/target_hal.h:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/include/m33mu/target_hal.h:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/include/m33mu/target_hal.h:39:void mm_uart_io_init(struct mm_uart_io *io);
/home/gus/src/m33mu/include/m33mu/target_hal.h:61:void mm_target_usart_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic);
/home/gus/src/m33mu/include/m33mu/target_hal.h:64:void mm_target_spi_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic);
/home/gus/src/m33mu/include/m33mu/target_hal.h:68:void mm_target_eth_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic);
/home/gus/src/m33mu/include/m33mu/otp.h:17:    mm_bool write_enabled;
/home/gus/src/m33mu/include/m33mu/otp.h:21:void mm_otp_init(struct mm_otp *otp, const char *target_name, mm_u32 size);
/home/gus/src/m33mu/include/m33mu/otp.h:23:mm_bool mm_otp_write(struct mm_otp *otp, mm_u32 offset, const mm_u8 *src, mm_u32 len);
/home/gus/src/m33mu/include/m33mu/otp.h:27:void mm_otp_set_write_enabled(struct mm_otp *otp, mm_bool enabled);
/home/gus/src/m33mu/include/m33mu/gpio.h:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/include/m33mu/gpio.h:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/include/m33mu/gpio.h:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/include/m33mu/gpio.h:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/include/m33mu/gpio.h:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/include/m33mu/gpio.h:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/include/m33mu/gpio.h:45:void mm_gpio_line_init(struct mm_gpio_line *line, mm_gpio_listener_fn listener, void *opaque);

[Output exceeded 2000 line limit (3214 lines total). Full output saved to /tmp/.tmp1iV4DK/output-4. Read it with shell commands like `head`, `tail`, or `sed -n '100,200p'` up to 2000 lines at a time.]
  ────────────────────────────────────────
  ▸ shell
    command: grep -rnE "(unpredict|undef|thumb|it|carry|overflow|saturat|it_prefix)" /home/gus/src/m33mu/src/m33mu/ /home/gus/src/m33mu/src/ /home/gus/src/m33mu/include/m33mu/ | grep -v "tests/" | grep -vE "(tpm_tis.c|usbdev.c|pka.c|cpu.c|gdbstub.h|mpu.h|flash_persist.h|fetch.h|target_hal.h|otp.h|gpio.h)" | head -n 50

/home/gus/src/m33mu/src/m33mu/tz.c:5: * This program is free software: you can redistribute it and/or modify
/home/gus/src/m33mu/src/m33mu/tz.c:6: * it under the terms of the GNU Affero General Public License as published
/home/gus/src/m33mu/src/m33mu/tz.c:7: * by the Free Software Foundation, either version 3 of the License, or
/home/gus/src/m33mu/src/m33mu/tz.c:10: * This program is distributed in the hope that it will be useful,
/home/gus/src/m33mu/src/m33mu/tz.c:11: * but WITHOUT ANY WARRANTY; without even the implied warranty of
/home/gus/src/m33mu/src/m33mu/tz.c:16: * along with this program.  If not, see <https://www.gnu.org/licenses/>.
/home/gus/src/m33mu/src/m33mu/tz.c:109:    if (!mm_memmap_write(map, MM_SECURE, sp, 4u, stacked_ra)) {
/home/gus/src/m33mu/src/m33mu/tz.c:112:    if (!mm_memmap_write(map, MM_SECURE, sp + 4u, 4u, retpsr)) {
/home/gus/src/m33mu/src/m33mu/tz.c:124:    /* Minimal model: SG is used for NS->S transition via an NSC veneer. */
/home/gus/src/m33mu/src/m33mu/tz.c:162:    /* BXNS is defined for Secure->Non-secure transition. */
/home/gus/src/m33mu/src/m33mu/tz.c:209:            printf("[TZ_BLXNS] overflow depth=%u max=%u, call aborted\n",
/home/gus/src/m33mu/src/m33mu/eth_backend.c:212:        n = write(g_backend.fd, data, len);
/home/gus/src/m33mu/src/m33mu/ta100.c:212:/* TA100 write modes (subset) */
/home/gus/src/m33mu/src/m33mu/ta100.c:336:    mm_bool rng_initialized;
/home/gus/src/m33mu/src/m33mu/ta100.c:373:    static mm_bool init = MM_FALSE;
/home/gus/src/m33mu/src/m33mu/ta100.c:375:    if (!init) {
/home/gus/src/m33mu/src/m33mu/ta100.c:378:        init = MM_TRUE;
/home/gus/src/m33mu/src/m33mu/ta100.c:385:    static mm_bool init = MM_FALSE;
/home/gus/src/m33mu/src/m33mu/ta100.c:387:    if (!init) {
/home/gus/src/m33mu/src/m33mu/ta100.c:390:        init = MM_TRUE;
/home/gus/src/m33mu/src/m33mu/ta100.c:398:    static mm_bool init = MM_FALSE;
/home/gus/src/m33mu/src/m33mu/ta100.c:400:    if (!init) {
/home/gus/src/m33mu/src/m33mu/ta100.c:403:        init = MM_TRUE;
/home/gus/src/m33mu/src/m33mu/ta100.c:436:        fprintf(stderr, "[TA100_TRACE]   ... (%lu bytes omitted)\n",
/home/gus/src/m33mu/src/m33mu/ta100.c:448:    switch (key_type) {
/home/gus/src/m33mu/src/m33mu/ta100.c:478:/* TA100 uses CRC-16 CCITT (polynomial 0x1021, init 0xFFFF) */
/home/gus/src/m33mu/src/m33mu/ta100.c:484:    const mm_u16 crchighbit = 0x8000;
/home/gus/src/m33mu/src/m33mu/ta100.c:490:            mm_u16 bit = crc & crchighbit;
/home/gus/src/m33mu/src/m33mu/ta100.c:493:                bit ^= crchighbit;
/home/gus/src/m33mu/src/m33mu/ta100.c:495:            if (bit != 0u) {
/home/gus/src/m33mu/src/m33mu/ta100.c:515:static void ta100_init_nv_layout(struct mm_ta100 *ta)
/home/gus/src/m33mu/src/m33mu/ta100.c:607:static void ta100_cmd_write(struct mm_ta100 *ta, const mm_u8 *cmd, mm_u32 cmd_len)
/home/gus/src/m33mu/src/m33mu/ta100.c:703:    if (!ta->rng_initialized) {
/home/gus/src/m33mu/src/m33mu/ta100.c:704:        ret = wc_InitRng(&ta->rng);
/home/gus/src/m33mu/src/m33mu/ta100.c:713:        ta->rng_initialized = MM_TRUE;
/home/gus/src/m33mu/src/m33mu/ta100.c:755:        if (!ta->rng_initialized) {
/home/gus/src/m33mu/src/m33mu/ta100.c:756:            int ret = wc_InitRng(&ta->rng);
/home/gus/src/m33mu/src/m33mu/ta100.c:765:            ta->rng_initialized = MM_TRUE;
/home/gus/src/m33mu/src/m33mu/ta100.c:841:        if (!ta->rng_initialized) {
/home/gus/src/m33mu/src/m33mu/ta100.c:842:            ret = wc_InitRng(&ta->rng);
/home/gus/src/m33mu/src/m33mu/ta100.c:851:            ta->rng_initialized = MM_TRUE;
/home/gus/src/m33mu/src/m33mu/ta100.c:881:    if (!ta->rng_initialized) {
/home/gus/src/m33mu/src/m33mu/ta100.c:882:        ret = wc_InitRng(&ta->rng);
/home/gus/src/m33mu/src/m33mu/ta100.c:891:        ta->rng_initialized = MM_TRUE;
/home/gus/src/m33mu/src/m33mu/ta100.c:894:    wc_ecc_init(&key);
/home/gus/src/m33mu/src/m33mu/ta100.c:975:        wc_InitSha256(&sha);
/home/gus/src/m33mu/src/m33mu/ta100.c:1004:    wc_InitSha256(&sha);
/home/gus/src/m33mu/src/m33mu/ta100.c:1011:    wc_ecc_init(&key);
/home/gus/src/m33mu/src/m33mu/ta100.c:1032:    if (!ta->rng_initialized) {
/home/gus/src/m33mu/src/m33mu/ta100.c:1033:        ret = wc_InitRng(&ta->rng);
  ────────────────────────────────────────
  ▸ shell
    command: ls -l /home/gus/src/m33mu/src/m33mu/decode.c /home/gus/src/m33mu/src/m33mu/exec_helpers.c /home/gus/src/m33mu/src/execute.c /home/gus/src/m33mu/src/m33mu/fetch.c

-rw-rw-r-- 1 gus gus 263055 Apr  6 17:21 /home/gus/src/m33mu/src/execute.c
-rw-rw-r-- 1 gus gus 104620 Apr  6 17:21 /home/gus/src/m33mu/src/m33mu/decode.c
-rw-rw-r-- 1 gus gus  14798 Apr  6 17:21 /home/gus/src/m33mu/src/m33mu/exec_helpers.c
-rw-rw-r-- 1 gus gus   3850 Mar 23 01:35 /home/gus/src/m33mu/src/m33mu/fetch.c
  ────────────────────────────────────────
  ▸ shell
    command: grep -nE "switch|case|if|else|return" /home/gus/src/m33mu/src/execute.c | head -n 50

5: * This program is free software: you can redistribute it and/or modify
18: * SPDX-License-Identifier: AGPL-3.0-or-later
48:    if (g_stack_trace < 0) {
52:    return g_stack_trace ? MM_TRUE : MM_FALSE;
57:    if (g_splim_trace < 0) {
61:    return g_splim_trace ? MM_TRUE : MM_FALSE;
66:    if (g_pop_trace < 0) {
70:    return g_pop_trace ? MM_TRUE : MM_FALSE;
75:    if (g_psp_trace < 0) {
79:    return g_psp_trace ? MM_TRUE : MM_FALSE;
84:    if (g_msp_trace < 0) {
88:    return g_msp_trace ? MM_TRUE : MM_FALSE;
93:    if (g_ctrl_trace < 0) {
97:    return g_ctrl_trace ? MM_TRUE : MM_FALSE;
102:    if (g_svc_stack_trace < 0) {
106:    return g_svc_stack_trace ? MM_TRUE : MM_FALSE;
120:    if (scs == 0) {
121:        return 0u;
123:    return (sec == MM_NONSECURE) ? scs->cpacr_ns : scs->cpacr_s;
130:    if (cpu == 0 || scs == 0 || !scs->fpu_present) {
131:        return MM_FALSE;
133:    if (cpu->sec_state == MM_NONSECURE) {
134:        if (((scs->nsacr >> 10) & 0x1u) == 0u || ((scs->nsacr >> 11) & 0x1u) == 0u) {
135:            return MM_FALSE;
140:    if (cp10 == 0u || cp11 == 0u) {
141:        return MM_FALSE;
143:    if (!mm_cpu_get_privileged(cpu) && (cp10 != 3u || cp11 != 3u)) {
144:        return MM_FALSE;
146:    return MM_TRUE;
151:    if (cpu == 0) {
152:        return MM_FALSE;
155:    if (!mm_rp2350_active()) {
156:        return MM_FALSE;
158:    if (coproc == 5u && cpu->sec_state != MM_SECURE) {
159:        return MM_FALSE;
161:    return MM_TRUE;
170:    if (ctx == 0 || ctx->cpu == 0 || ctx->scs == 0) {
171:        return MM_FALSE;
173:    if (fpu_access_allowed(ctx->cpu, ctx->scs)) {
174:        if (ctx->cpu->mode == MM_HANDLER &&
177:            if (idx < MM_EXC_STACK_MAX &&
183:                    if (!mm_memmap_write(ctx->map, sec, sp + (mm_u32)(i * 4u), 4u, ctx->cpu->s[i])) {
184:                        if (ctx->raise_mem_fault != 0 && ctx->fetch != 0) {
185:                            if (!ctx->raise_mem_fault(ctx->cpu, ctx->map, ctx->scs,
192:                        return MM_FALSE;
195:                if (!mm_memmap_write(ctx->map, sec, sp + (16u * 4u), 4u, ctx->cpu->fpscr) ||
197:                    if (ctx->raise_mem_fault != 0 && ctx->fetch != 0) {
198:                        if (!ctx->raise_mem_fault(ctx->cpu, ctx->map, ctx->scs,
205:                    return MM_FALSE;
211:        return MM_TRUE;
  ────────────────────────────────────────
  ▸ shell
    command: grep -nE "mm_cpu_step|mm_cpu_exec" /home/gus/src/m33mu/src/execute.c

(no output)

Command exited with code 1
  ────────────────────────────────────────
  ▸ shell
    command: grep -nE "mm_cpu_step|mm_cpu_exec|execute" /home/gus/src/m33mu/src/execute.c /home/gus/src/m33mu/src/m33mu/decode.c /home/gus/src/m33mu/src/m33mu/exec_helpers.c | head -n 50

/home/gus/src/m33mu/src/execute.c:22:#include "m33mu/execute.h"
/home/gus/src/m33mu/src/execute.c:164:static mm_bool fpu_check_or_fault(struct mm_execute_ctx *ctx)
/home/gus/src/m33mu/src/execute.c:506:mm_bool mm_it_should_execute(struct mm_cpu *cpu,
/home/gus/src/m33mu/src/execute.c:512:    mm_bool execute_it = MM_TRUE;
/home/gus/src/m33mu/src/execute.c:552:        execute_it = take;
/home/gus/src/m33mu/src/execute.c:554:    return execute_it;
/home/gus/src/m33mu/src/execute.c:579:enum mm_exec_status mm_execute_decoded(struct mm_execute_ctx *ctx)
/home/gus/src/m33mu/src/execute.c:630:                    /* Snapshot last executed instruction for fetch diagnostics. */
/home/gus/src/m33mu/src/m33mu/decode.c:133:     * the remaining architecturally undefined hint slots execute as NOPs. */
  ────────────────────────────────────────
  ▸ shell
    command: cat /home/gus/src/m33mu/src/execute.c | sed -n '579,650p'

enum mm_exec_status mm_execute_decoded(struct mm_execute_ctx *ctx)
{
    mm_u8 itstate_val = 0;
    mm_u32 pc_before_exec = 0;
    mm_bool (*handle_pc_write)(struct mm_cpu *, struct mm_memmap *, struct mm_scs *, mm_u32, mm_u8 *, mm_u8 *, mm_u8 *);
    mm_bool (*raise_mem_fault)(struct mm_cpu *, struct mm_memmap *, struct mm_scs *, mm_u32, mm_u32, mm_u32, mm_bool);
    mm_bool (*raise_usage_fault)(struct mm_cpu *, struct mm_memmap *, struct mm_scs *, mm_u32, mm_u32, mm_u32);
    mm_bool (*enter_exception)(struct mm_cpu *, struct mm_memmap *, struct mm_scs *, mm_u32, mm_u32, mm_u32);
    mm_bool opt_gdb;

    if (ctx == 0 || ctx->cpu == 0 || ctx->map == 0 || ctx->scs == 0 || ctx->fetch == 0 || ctx->dec == 0 ||
        ctx->it_pattern == 0 || ctx->it_remaining == 0 || ctx->it_cond == 0 || ctx->done == 0) {
        return MM_EXEC_CONTINUE;
    }

    handle_pc_write = ctx->handle_pc_write;
    raise_mem_fault = ctx->raise_mem_fault;
    raise_usage_fault = ctx->raise_usage_fault;
    enter_exception = ctx->enter_exception;
    opt_gdb = ctx->opt_gdb;

#define cpu (*ctx->cpu)
#define map (*ctx->map)
#define scs (*ctx->scs)
#define gdb (*ctx->gdb)
#define f (*ctx->fetch)
#define d (*ctx->dec)
#define it_pattern (*ctx->it_pattern)
#define it_remaining (*ctx->it_remaining)
#define it_cond (*ctx->it_cond)
#define done (*ctx->done)
#define EXEC_SET_SP(value_expr) do { \
    if (!exec_set_active_sp(&cpu, &map, &scs, &f, (value_expr), raise_usage_fault, &done)) { \
        return MM_EXEC_CONTINUE; \
    } \
} while (0)
#define EXEC_RAISE_UNDEF() do { \
    if (getenv("M33MU_UNDEF_TRACE")) { \
        printf("[UNDEF_EXEC] pc=0x%08lx len=%u raw=0x%08lx kind=%u\n", \
               (unsigned long)f.pc_fetch, \
               (unsigned int)d.len, \
               (unsigned long)d.raw, \
               (unsigned int)d.kind); \
    } \
    if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, (1u << 16))) { \
        done = MM_TRUE; \
    } \
    return MM_EXEC_CONTINUE; \
} while (0)

                    pc_before_exec = cpu.r[15];
                    /* Snapshot last executed instruction for fetch diagnostics. */
                    switch (d.kind) {
                        case MM_OP_IT:
                            it_cond = (mm_u8)((d.imm >> 4) & 0x0fu);
                            it_mask_to_pattern(it_cond, (mm_u8)(d.imm & 0x0fu), &it_pattern, &it_remaining);
                            itstate_val = (mm_u8)((it_cond << 4) | (d.imm & 0x0fu));
                            cpu.xpsr = itstate_set(cpu.xpsr, itstate_val);
                            break;
                        case MM_OP_NOP:
                        case MM_OP_NOP_W:
                            /* NOP instructions - no operation */
                            break;
                        case MM_OP_DSB:
                        case MM_OP_DMB:
                        case MM_OP_ISB:
                            /* Barriers are modeled as no-ops for now. */
                            break;
                        case MM_OP_MCR_MRC: {
                                               mm_u8 op1 = (mm_u8)(d.imm & 0x7u);
                                               mm_u8 op2 = (mm_u8)((d.imm >> 3) & 0x7u);
                                               mm_u8 opcode = (mm_u8)((d.imm >> 6) & 0x1u); /* 0=MCR, 1=MRC */-32602: Could not interpret tool use parameters for id call_2crlp99l: trailing characters at line 1 column 71A tool call could not be parsed — the response may have been truncated. Try breaking the task into smaller steps or resending your message.
