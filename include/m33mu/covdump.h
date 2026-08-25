/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
/* covdump.h - dump LLVM instrumentation regions at end of execution.
 *
 * Firmware built with clang -fprofile-instr-generate -fcoverage-mapping keeps
 * its execution counters and MC/DC bitmaps in RAM, in regions delimited by
 * linker-provided __start___llvm_prf_* / __stop___llvm_prf_* symbols. Writing
 * them out from inside the firmware costs a serializer, a large staging buffer
 * and a transport; the emulator can simply read them.
 *
 * Only the raw bytes are produced here. The .profraw container is a versioned
 * LLVM format and is assembled host-side, where it stays in step with the
 * llvm-profdata that consumes it.
 */
#ifndef M33MU_COVDUMP_H
#define M33MU_COVDUMP_H

#include "m33mu/types.h"
#include "m33mu/cpu.h"
#include "m33mu/memmap.h"

#define MM_COVDUMP_ERRSZ 256u

struct mm_covdump_region {
    mm_u32 start;
    mm_u32 stop;
    mm_bool present;
};

struct mm_covdump_info {
    struct mm_covdump_region cnts;   /* counters, RAM */
    struct mm_covdump_region bits;   /* MC/DC bitmaps, RAM */
    struct mm_covdump_region data;   /* profile data, flash (manifest only) */
    struct mm_covdump_region names;  /* function names, flash (manifest only) */
};

/* Resolve the instrumentation regions from an ELF's symbol table.
 * Returns MM_FALSE and fills err when the ELF carries no counters region,
 * which is the case for firmware that was not built with coverage. */
mm_bool mm_covdump_resolve(const char *elf_path, struct mm_covdump_info *out,
                           char *err, size_t errsz);

/* Look up a symbol by name in the ELF's symtab and return its address
 * (st_value). Returns MM_FALSE if the symbol doesn't exist, or if
 * libelf wasn't available at build time. */
mm_bool mm_elf_lookup_symbol(const char *elf_path, const char *sym_name,
                              mm_u32 *addr_out, char *err, size_t errsz);

/* Read the counter and bitmap regions out of the emulated memory map and write
 * <prefix>.cnts.bin, <prefix>.bits.bin and <prefix>.json.
 * Returns MM_FALSE and fills err on an unmapped region or a write failure. */
mm_bool mm_covdump_write(const struct mm_memmap *map, enum mm_sec_state sec,
                         const struct mm_covdump_info *info,
                         const char *elf_path, const char *prefix,
                         const char *exit_reason, mm_u64 cycles,
                         char *err, size_t errsz);

#endif /* M33MU_COVDUMP_H */
