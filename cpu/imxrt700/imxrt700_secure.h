/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#ifndef M33MU_IMXRT700_SECURE_H
#define M33MU_IMXRT700_SECURE_H

#include "m33mu/types.h"

/*
 * Security blocks of the RT700 compute domain:
 *
 *  - GLIKEY0..5: glitch-resistant write-enable FSM (SDK fsl_glikey sequence).
 *    GLIKEY4 index 1 gates SYSCON3 CPU1_SVTOR/NSVTOR; GLIKEY0 index 1 gates
 *    the AHBSC0 MISC_CTRL registers.
 *  - AHBSC0/3/4: MISC_CTRL write lock; the AHBSC0 SRAM memory rules feed the
 *    MPCBB hook while secure checking is enabled (CPU0 accesses).
 *  - TRNG: register-level entropy model (MCTL/ENT[]), host RNG backed.
 *  - OCOTP: fuse array behind OTP_SHADOW[] and the READ_CTRL/WRITE_DATA path.
 *  - ELS, PKC, PUF: m33mu synthetic command interface (same contract as the
 *    MCXN947 model, see below), computed with wolfSSL.  Without wolfSSL every
 *    crypto command completes with STATUS.ERROR.
 *
 * Synthetic ELS/PKC/PUF register map (offsets within each block):
 *   0x000 CMD      write starts a command
 *   0x004 STATUS   bit0 BUSY, bit1 DONE, bit2 ERROR, bit3 SECURE (w1c)
 *   0x008 ARG0..3  0x020 RESULT0..3  0x080 KEYIN0..3  0x100 DATA0..3
 *
 * ELS commands (ARG0 = key slot 0..7):
 *   1 GENERATE   random 256-bit key into the slot
 *   2 DERIVE     slot = HMAC(device key, KEYIN0..3)
 *   3 IMPORT     slot = KEYIN0..3 || DATA0..3
 *   4 SIGN       RESULT0..3||DATA0..3 = HMAC-SHA256(slot, mem[ARG1], ARG2 bytes)
 *   5 VERIFY     compare the MAC with the 32 bytes at mem[ARG3]; RESULT0 = 1 if equal
 *   7 RNG        32 random bytes in RESULT0..3||DATA0..3 (and mem[ARG1], ARG2 bytes if ARG1 != 0)
 *   8 SHA256     digest of mem[ARG1], ARG2 bytes in RESULT0..3||DATA0..3 (and mem[ARG3] if != 0)
 *   9 AES_ENC    AES-CBC, key = slot (256-bit), IV = KEYIN0..3, mem[ARG1] -> mem[ARG3], ARG2 bytes
 *  10 AES_DEC    as AES_ENC, decrypting
 * PKC commands (operands big-endian, ARG3 bytes each, result to mem[DATA0]):
 *   1 MODEXP     mem[ARG0] ^ mem[ARG1] mod mem[ARG2]
 *   2 MODMUL     mem[ARG0] * mem[ARG1] mod mem[ARG2]
 * PUF commands:
 *   1 ENROLL     RESULT0 = 1
 *   2 GETKEY     ELS slot ARG0 = HMAC(PUF secret, KEYIN0..3); RESULT0 = 0xCAFE0000 | slot,
 *                RESULT1 = first word of SHA-256(key) (the key itself is never exposed)
 */

void mm_imxrt700_secure_attach(void);
void mm_imxrt700_secure_reset(void);
void mm_imxrt700_secure_flash_bind(mm_u8 *flash, mm_u32 flash_size);

/* Fuse access for the ROM OTP driver. */
mm_bool mm_imxrt700_otp_read(mm_u32 index, mm_u32 *value_out);
mm_bool mm_imxrt700_otp_program(mm_u32 index, mm_u32 value);

/* MM_TRUE while GLIKEYn is in WR_EN state for the given write index. */
mm_bool mm_imxrt700_glikey_write_enabled(mm_u32 glikey, mm_u32 index);

/* MPCBB hook: AHBSC0 SRAM memory rules, 4 KB blocks of the 7.5 MB SRAM. */
mm_bool mm_imxrt700_mpcbb_block_secure(int bank, mm_u32 block_index);
void mm_imxrt700_secure_set_active_core(mm_u32 core);

#endif /* M33MU_IMXRT700_SECURE_H */
