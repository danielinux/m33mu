/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
/* covdump.c - see include/m33mu/covdump.h */

#include "m33mu/covdump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef M33MU_HAS_LIBELF
#include <fcntl.h>
#include <unistd.h>
#include <libelf.h>
#include <gelf.h>
#endif

struct mm_covdump_sym {
    const char *name;
    struct mm_covdump_region *region;
    mm_bool is_start;
};

#ifdef M33MU_HAS_LIBELF

static void covdump_note(struct mm_covdump_region *r, mm_bool is_start,
                         mm_u32 value)
{
    if (is_start) {
        r->start = value;
    } else {
        r->stop = value;
    }
}

static mm_bool covdump_lookup_in_elf(Elf *elf, const char *sym_name,
                                     mm_u32 *addr_out)
{
    Elf_Scn *scn = 0;
    GElf_Shdr shdr;

    while ((scn = elf_nextscn(elf, scn)) != 0) {
        Elf_Data *data;
        size_t nsym;
        size_t s;

        if (gelf_getshdr(scn, &shdr) == 0) {
            continue;
        }
        if (shdr.sh_type != SHT_SYMTAB) {
            continue;
        }

        data = elf_getdata(scn, NULL);
        if (data == 0 || shdr.sh_entsize == 0) {
            continue;
        }

        nsym = (size_t)(shdr.sh_size / shdr.sh_entsize);
        for (s = 0; s < nsym; ++s) {
            GElf_Sym sym;
            const char *name;

            if (gelf_getsym(data, (int)s, &sym) == 0) {
                continue;
            }
            name = elf_strptr(elf, shdr.sh_link, sym.st_name);
            if (name == 0) {
                continue;
            }
            if (strcmp(name, sym_name) == 0) {
                *addr_out = (mm_u32)sym.st_value;
                return MM_TRUE;
            }
        }
    }
    return MM_FALSE;
}

mm_bool mm_elf_lookup_symbol(const char *elf_path, const char *sym_name,
                             mm_u32 *addr_out, char *err, size_t errsz)
{
    Elf *elf = 0;
    int fd = -1;
    mm_bool found;

    if (elf_path == 0 || sym_name == 0 || addr_out == 0) {
        snprintf(err, errsz, "invalid arguments to mm_elf_lookup_symbol");
        return MM_FALSE;
    }
    *addr_out = 0;

    if (elf_version(EV_CURRENT) == EV_NONE) {
        snprintf(err, errsz, "libelf version mismatch");
        return MM_FALSE;
    }

    fd = open(elf_path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errsz, "cannot open %s", elf_path);
        return MM_FALSE;
    }

    elf = elf_begin(fd, ELF_C_READ, NULL);
    if (elf == 0) {
        snprintf(err, errsz, "%s is not readable as ELF", elf_path);
        close(fd);
        return MM_FALSE;
    }

    found = covdump_lookup_in_elf(elf, sym_name, addr_out);

    elf_end(elf);
    close(fd);

    if (!found) {
        snprintf(err, errsz, "symbol '%s' not found in %s", sym_name, elf_path);
    }
    return found;
}

mm_bool mm_covdump_resolve(const char *elf_path, struct mm_covdump_info *out,
                           char *err, size_t errsz)
{
    struct mm_covdump_sym wanted[8];
    Elf *elf = 0;
    int fd = -1;
    size_t i;
    mm_bool ok = MM_FALSE;

    if (elf_path == 0 || out == 0) {
        return MM_FALSE;
    }
    memset(out, 0, sizeof(*out));
    wanted[0].name = "__start___llvm_prf_cnts";
    wanted[0].region = &out->cnts; wanted[0].is_start = MM_TRUE;
    wanted[1].name = "__stop___llvm_prf_cnts";
    wanted[1].region = &out->cnts; wanted[1].is_start = MM_FALSE;
    wanted[2].name = "__start___llvm_prf_bits";
    wanted[2].region = &out->bits; wanted[2].is_start = MM_TRUE;
    wanted[3].name = "__stop___llvm_prf_bits";
    wanted[3].region = &out->bits; wanted[3].is_start = MM_FALSE;
    wanted[4].name = "__start___llvm_prf_data";
    wanted[4].region = &out->data; wanted[4].is_start = MM_TRUE;
    wanted[5].name = "__stop___llvm_prf_data";
    wanted[5].region = &out->data; wanted[5].is_start = MM_FALSE;
    wanted[6].name = "__start___llvm_prf_names";
    wanted[6].region = &out->names; wanted[6].is_start = MM_TRUE;
    wanted[7].name = "__stop___llvm_prf_names";
    wanted[7].region = &out->names; wanted[7].is_start = MM_FALSE;

    if (elf_version(EV_CURRENT) == EV_NONE) {
        snprintf(err, errsz, "libelf version mismatch");
        return MM_FALSE;
    }
    fd = open(elf_path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errsz, "cannot open %s", elf_path);
        return MM_FALSE;
    }
    elf = elf_begin(fd, ELF_C_READ, NULL);
    if (elf == 0) {
        snprintf(err, errsz, "%s is not readable as ELF", elf_path);
        close(fd);
        return MM_FALSE;
    }

    for (i = 0; i < sizeof(wanted) / sizeof(wanted[0]); ++i) {
        mm_u32 addr;

        if (covdump_lookup_in_elf(elf, wanted[i].name, &addr)) {
            covdump_note(wanted[i].region, wanted[i].is_start, addr);
            wanted[i].region->present = MM_TRUE;
        }
    }

    elf_end(elf);
    close(fd);

    /* The counters region is the one that must exist: bitmaps are absent when
     * the firmware was built without -fcoverage-mcdc, which is legitimate. */
    if (!out->cnts.present || out->cnts.stop < out->cnts.start) {
        snprintf(err, errsz,
                 "no __llvm_prf_cnts region in %s; was it built with "
                 "clang -fprofile-instr-generate?", elf_path);
    } else {
        ok = MM_TRUE;
    }
    return ok;
}

#else /* !M33MU_HAS_LIBELF */

mm_bool mm_elf_lookup_symbol(const char *elf_path, const char *sym_name,
                             mm_u32 *addr_out, char *err, size_t errsz)
{
    (void)elf_path;
    (void)sym_name;
    (void)addr_out;
    snprintf(err, errsz, "symbol lookup needs libelf, which was not "
             "available at build time");
    return MM_FALSE;
}

mm_bool mm_covdump_resolve(const char *elf_path, struct mm_covdump_info *out,
                           char *err, size_t errsz)
{
    (void)elf_path;
    (void)out;
    snprintf(err, errsz, "coverage dumping needs libelf, which was not "
                         "available at build time");
    return MM_FALSE;
}

#endif /* M33MU_HAS_LIBELF */

static mm_bool covdump_read_region(const struct mm_memmap *map,
                                   enum mm_sec_state sec,
                                   const struct mm_covdump_region *r,
                                   mm_u8 **buf_out, size_t *len_out,
                                   const char *what, char *err, size_t errsz)
{
    mm_u32 addr;
    size_t len;
    size_t i;
    mm_u8 *buf;

    *buf_out = 0;
    *len_out = 0;
    if (!r->present || r->stop <= r->start) {
        return MM_TRUE; /* absent region: nothing to write, not an error */
    }
    len = (size_t)(r->stop - r->start);
    buf = (mm_u8 *)malloc(len);
    if (buf == 0) {
        snprintf(err, errsz, "out of memory for %s (%zu bytes)", what, len);
        return MM_FALSE;
    }
    addr = r->start;
    for (i = 0; i < len; ++i) {
        mm_u8 v = 0;
        if (!mm_memmap_read8(map, sec, addr + (mm_u32)i, &v)) {
            snprintf(err, errsz,
                     "%s: address 0x%08lx is not mapped in the %s state",
                     what, (unsigned long)(addr + (mm_u32)i),
                     (sec == MM_NONSECURE) ? "non-secure" : "secure");
            free(buf);
            return MM_FALSE;
        }
        buf[i] = v;
    }
    *buf_out = buf;
    *len_out = len;
    return MM_TRUE;
}

static mm_bool covdump_write_file(const char *path, const mm_u8 *buf,
                                  size_t len, char *err, size_t errsz)
{
    FILE *f = fopen(path, "wb");
    if (f == 0) {
        snprintf(err, errsz, "cannot write %s", path);
        return MM_FALSE;
    }
    if (len > 0u && fwrite(buf, 1u, len, f) != len) {
        snprintf(err, errsz, "short write to %s", path);
        fclose(f);
        return MM_FALSE;
    }
    if (fclose(f) != 0) {
        snprintf(err, errsz, "cannot close %s", path);
        return MM_FALSE;
    }
    return MM_TRUE;
}

static void covdump_json_region(FILE *f, const char *name,
                                const struct mm_covdump_region *r,
                                mm_bool last)
{
    if (r->present) {
        fprintf(f, "    \"%s\": { \"start\": \"0x%08lx\", \"stop\": "
                   "\"0x%08lx\", \"size\": %lu }%s\n",
                name, (unsigned long)r->start, (unsigned long)r->stop,
                (unsigned long)(r->stop - r->start), last ? "" : ",");
    } else {
        fprintf(f, "    \"%s\": null%s\n", name, last ? "" : ",");
    }
}

mm_bool mm_covdump_write(const struct mm_memmap *map, enum mm_sec_state sec,
                         const struct mm_covdump_info *info,
                         const char *elf_path, const char *prefix,
                         const char *exit_reason, mm_u64 cycles,
                         char *err, size_t errsz)
{
    char path[1024];
    mm_u8 *cnts = 0;
    mm_u8 *bits = 0;
    size_t cnts_len = 0;
    size_t bits_len = 0;
    FILE *mf;

    if (map == 0 || info == 0 || prefix == 0) {
        snprintf(err, errsz, "invalid coverage dump request");
        return MM_FALSE;
    }
    if (!covdump_read_region(map, sec, &info->cnts, &cnts, &cnts_len,
                             "__llvm_prf_cnts", err, errsz)) {
        return MM_FALSE;
    }
    if (!covdump_read_region(map, sec, &info->bits, &bits, &bits_len,
                             "__llvm_prf_bits", err, errsz)) {
        free(cnts);
        return MM_FALSE;
    }

    snprintf(path, sizeof(path), "%s.cnts.bin", prefix);
    if (!covdump_write_file(path, cnts, cnts_len, err, errsz)) {
        free(cnts); free(bits);
        return MM_FALSE;
    }
    snprintf(path, sizeof(path), "%s.bits.bin", prefix);
    if (!covdump_write_file(path, bits, bits_len, err, errsz)) {
        free(cnts); free(bits);
        return MM_FALSE;
    }
    free(cnts);
    free(bits);

    snprintf(path, sizeof(path), "%s.json", prefix);
    mf = fopen(path, "w");
    if (mf == 0) {
        snprintf(err, errsz, "cannot write %s", path);
        return MM_FALSE;
    }
    fprintf(mf, "{\n");
    fprintf(mf, "  \"elf\": \"%s\",\n", (elf_path != 0) ? elf_path : "");
    fprintf(mf, "  \"security_state\": \"%s\",\n",
            (sec == MM_NONSECURE) ? "non-secure" : "secure");
    fprintf(mf, "  \"exit_reason\": \"%s\",\n",
            (exit_reason != 0) ? exit_reason : "unknown");
    fprintf(mf, "  \"cycles\": %llu,\n", (unsigned long long)cycles);
    fprintf(mf, "  \"regions\": {\n");
    covdump_json_region(mf, "cnts", &info->cnts, MM_FALSE);
    covdump_json_region(mf, "bits", &info->bits, MM_FALSE);
    covdump_json_region(mf, "data", &info->data, MM_FALSE);
    covdump_json_region(mf, "names", &info->names, MM_TRUE);
    fprintf(mf, "  }\n}\n");
    if (fclose(mf) != 0) {
        snprintf(err, errsz, "cannot close %s", path);
        return MM_FALSE;
    }

    printf("[COVDUMP] %s.cnts.bin %zu bytes, %s.bits.bin %zu bytes (%s)\n",
           prefix, cnts_len, prefix, bits_len,
           (exit_reason != 0) ? exit_reason : "unknown");
    return MM_TRUE;
}
