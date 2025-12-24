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

#ifndef M33MU_TYPES_H
#define M33MU_TYPES_H

/*
 * Core integer and boolean types for the emulator. Keep definitions C90-compatible.
 */

#include <stddef.h>

/* Prefer <stdint.h> when available; fall back to common widths otherwise. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#include <stdint.h>
typedef uint8_t  mm_u8;
typedef uint16_t mm_u16;
typedef uint32_t mm_u32;
typedef uint64_t mm_u64;
typedef int8_t   mm_i8;
typedef int16_t  mm_i16;
typedef int32_t  mm_i32;
typedef int64_t  mm_i64;
#else
typedef unsigned char      mm_u8;
typedef unsigned short     mm_u16;
typedef unsigned int       mm_u32;
typedef unsigned long long mm_u64;
typedef signed char        mm_i8;
typedef signed short       mm_i16;
typedef signed int         mm_i32;
typedef signed long long   mm_i64;
#endif

typedef int mm_bool; /* 0 = false, non-zero = true */

#ifndef MM_FALSE
#define MM_FALSE 0
#endif

#ifndef MM_TRUE
#define MM_TRUE 1
#endif

#endif /* M33MU_TYPES_H */
