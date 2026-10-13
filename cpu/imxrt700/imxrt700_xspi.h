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

#ifndef M33MU_IMXRT700_XSPI_H
#define M33MU_IMXRT700_XSPI_H

#include "m33mu/types.h"
#include "m33mu/flash_persist.h"

/*
 * XSPI0 controller with the EVK's octal NOR (Macronix MX25UM51345G) behind
 * it.  The AHB window is the emulated "flash" buffer; IP commands run the
 * programmed LUT sequence against the same buffer:
 *
 *   - DLL, FSM and bus status read as locked / idle / ready;
 *   - IP commands: SFP_TG_SFAR + SFP_TG_IPCR, TX buffer (TBDR), RX buffer
 *     (RBDR / RBSR / RBCT watermark), ERRSTAT arbitration and error flags;
 *   - NOR opcodes: WREN/WRDI, RDSR, RDSCUR, RDID, page program, 4K / 64K /
 *     chip erase, reads;
 *   - SFP: FRAD descriptors (lock until reset, a zero access policy refuses
 *     program / erase in the region with a FRADnACC error),
 *     TG MDAD descriptors and MGC global lock.
 *
 * Program and erase are written back to --persist files and invalidate the
 * translated code of both XSPI0 aliases.
 */
void mm_imxrt700_xspi_attach(void);
void mm_imxrt700_xspi_reset(void);
void mm_imxrt700_xspi_bind(mm_u8 *flash, mm_u32 flash_size, const struct mm_flash_persist *persist);

#endif /* M33MU_IMXRT700_XSPI_H */
