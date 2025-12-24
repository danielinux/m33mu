/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "m33mu/spiflash.h"
#include "m33mu/mmio.h"
#include "m33mu/mem_prot.h"

#define SPIFLASH_MAX 8
#define SPIFLASH_PAGE_SIZE 256u

enum spiflash_state {
    SPIFLASH_IDLE = 0,
    SPIFLASH_ADDR,
    SPIFLASH_READ,
    SPIFLASH_PP,
    SPIFLASH_STATUS_READ,
    SPIFLASH_STATUS_WRITE
};

struct mm_spiflash {
    int bus;
    mm_u32 size;
    mm_u8 *data;
    mm_bool locked;
    mm_bool write_enable;
    mm_u8 status_bp; /* block protection bits (BP2..BP0) */
    mm_u8 cmd;
    mm_u32 addr;
    mm_u32 page_base;
    int dummy_left;
    mm_u8 addr_bytes[4];
    int addr_need;
    int addr_have;
    enum spiflash_state state;
    mm_bool mmap;
    mm_u32 mmap_base;
    char path[256];
    mm_bool dirty;
};

static struct mm_spiflash g_spiflash[SPIFLASH_MAX];
static size_t g_spiflash_count = 0;

static void spiflash_sync(struct mm_spiflash *flash)
{
    FILE *f;
    if (flash == 0 || !flash->dirty) {
        return;
    }
    f = fopen(flash->path, "wb");
    if (f == 0) {
        fprintf(stderr, "spiflash: failed to open %s for write\n", flash->path);
        return;
    }
    if (flash->size > 0u) {
        size_t n = fwrite(flash->data, 1u, (size_t)flash->size, f);
        if (n != (size_t)flash->size) {
            fprintf(stderr, "spiflash: short write for %s\n", flash->path);
        }
    }
    fclose(f);
    flash->dirty = MM_FALSE;
}

static mm_bool spiflash_load(struct mm_spiflash *flash)
{
    FILE *f;
    size_t n = 0;
    if (flash == 0 || flash->size == 0u) {
        return MM_FALSE;
    }
    flash->data = (mm_u8 *)malloc((size_t)flash->size);
    if (flash->data == 0) {
        fprintf(stderr, "spiflash: out of memory\n");
        return MM_FALSE;
    }
    memset(flash->data, 0xFF, (size_t)flash->size);
    f = fopen(flash->path, "rb");
    if (f != 0) {
        n = fread(flash->data, 1u, (size_t)flash->size, f);
        fclose(f);
        if (n < (size_t)flash->size) {
            flash->dirty = MM_TRUE;
            spiflash_sync(flash);
        }
        return MM_TRUE;
    }
    flash->dirty = MM_TRUE;
    spiflash_sync(flash);
    return MM_TRUE;
}

static void spiflash_set_locked(struct mm_spiflash *flash, mm_bool locked)
{
    mm_bool prev;
    if (flash == 0) {
        return;
    }
    prev = flash->locked;
    flash->locked = locked ? MM_TRUE : MM_FALSE;
    if (prev != flash->locked) {
        printf("[SPI_FLASH] SPI%d %s\n",
               flash->bus,
               flash->locked ? "locked" : "unlocked");
    }
}

static void spiflash_set_bp(struct mm_spiflash *flash, mm_u8 bp)
{
    if (flash == 0) {
        return;
    }
    flash->status_bp = (mm_u8)(bp & 0x1Cu);
    spiflash_set_locked(flash, (flash->status_bp != 0u) ? MM_TRUE : MM_FALSE);
}

static mm_u8 spiflash_status(const struct mm_spiflash *flash)
{
    mm_u8 st = 0;
    if (flash == 0) {
        return 0;
    }
    if (flash->write_enable) st |= 0x02u;
    st |= flash->status_bp;
    return st;
}

static void spiflash_erase_range(struct mm_spiflash *flash, mm_u32 addr, mm_u32 size)
{
    mm_u32 end;
    if (flash == 0 || flash->data == 0 || size == 0u) {
        return;
    }
    end = addr + size;
    if (end < addr) {
        return;
    }
    if (addr >= flash->size) {
        return;
    }
    if (end > flash->size) {
        end = flash->size;
    }
    memset(flash->data + addr, 0xFF, (size_t)(end - addr));
    flash->dirty = MM_TRUE;
    spiflash_sync(flash);
}

static mm_u8 spiflash_read_byte(const struct mm_spiflash *flash, mm_u32 addr)
{
    if (flash == 0 || flash->data == 0 || flash->size == 0u) {
        return 0xFFu;
    }
    return flash->data[addr % flash->size];
}

mm_u8 mm_spiflash_xfer(struct mm_spiflash *flash, mm_u8 out)
{
    mm_u8 resp = 0xFFu;
    if (flash == 0) {
        return 0;
    }

    switch (flash->state) {
    case SPIFLASH_IDLE:
        flash->cmd = out;
        flash->addr = 0;
        flash->addr_have = 0;
        flash->addr_need = 0;
        switch (out) {
        case 0x06: /* WREN */
            flash->write_enable = MM_TRUE;
            return 0xFFu;
        case 0x04: /* WRDI */
            flash->write_enable = MM_FALSE;
            return 0xFFu;
        case 0x05: /* RDSR */
            flash->state = SPIFLASH_STATUS_READ;
            return spiflash_status(flash);
        case 0x01: /* WRSR */
            flash->state = SPIFLASH_STATUS_WRITE;
            return 0xFFu;
        case 0x9F: /* RDID */
            flash->state = SPIFLASH_READ;
            flash->addr = 0;
            return 0xC2u; /* generic manufacturer */
        case 0x03: /* READ */
        case 0x0B: /* FAST READ */
            flash->addr_need = 3;
            flash->state = SPIFLASH_ADDR;
            return 0xFFu;
        case 0x02: /* PAGE PROGRAM */
            flash->addr_need = 3;
            flash->state = SPIFLASH_ADDR;
            return 0xFFu;
        case 0x20: /* SECTOR ERASE (4K) */
        case 0xD8: /* BLOCK ERASE (64K) */
        case 0xC7: /* CHIP ERASE */
        case 0x60:
            flash->addr_need = (out == 0xC7 || out == 0x60) ? 0 : 3;
            if (flash->addr_need == 0) {
                if (flash->write_enable) {
                    spiflash_erase_range(flash, 0, flash->size);
                    flash->write_enable = MM_FALSE;
                }
                return 0xFFu;
            }
            flash->state = SPIFLASH_ADDR;
            return 0xFFu;
        default:
            return 0xFFu;
        }
        break;
    case SPIFLASH_ADDR:
        flash->addr_bytes[flash->addr_have++] = out;
        if (flash->addr_have >= flash->addr_need) {
            flash->addr = ((mm_u32)flash->addr_bytes[0] << 16) |
                          ((mm_u32)flash->addr_bytes[1] << 8) |
                          ((mm_u32)flash->addr_bytes[2]);
            flash->page_base = flash->addr & ~(SPIFLASH_PAGE_SIZE - 1u);
            if (flash->cmd == 0x03 || flash->cmd == 0x0B) {
                flash->dummy_left = (flash->cmd == 0x0B) ? 1 : 0;
                flash->state = SPIFLASH_READ;
            } else if (flash->cmd == 0x02) {
                flash->state = SPIFLASH_PP;
            } else if (flash->cmd == 0x20) {
                if (flash->write_enable) {
                    spiflash_erase_range(flash, flash->addr & ~0xFFFu, 0x1000u);
                    flash->write_enable = MM_FALSE;
                }
                flash->state = SPIFLASH_IDLE;
            } else if (flash->cmd == 0xD8) {
                if (flash->write_enable) {
                    spiflash_erase_range(flash, flash->addr & ~0xFFFFu, 0x10000u);
                    flash->write_enable = MM_FALSE;
                }
                flash->state = SPIFLASH_IDLE;
            } else {
                flash->state = SPIFLASH_IDLE;
            }
        }
        return 0xFFu;
    case SPIFLASH_READ:
        if (flash->cmd == 0x9F) {
            /* Return a fixed 3-byte JEDEC ID stream. */
            if (flash->addr == 0) resp = 0xC2u;
            else if (flash->addr == 1) resp = 0x20u;
            else resp = 0x18u;
            flash->addr++;
            return resp;
        }
        if (flash->dummy_left > 0) {
            flash->dummy_left--;
            return 0xFFu;
        }
        resp = spiflash_read_byte(flash, flash->addr);
        flash->addr++;
        return resp;
    case SPIFLASH_PP: {
        if (flash->write_enable && flash->data != 0) {
            mm_u32 idx = flash->addr % flash->size;
            mm_u8 cur = flash->data[idx];
            mm_u8 next = (mm_u8)(cur & out);
            if (next != cur) {
                flash->data[idx] = next;
                flash->dirty = MM_TRUE;
            }
        }
        flash->addr++;
        if ((flash->addr & ~(SPIFLASH_PAGE_SIZE - 1u)) != flash->page_base) {
            flash->addr = flash->page_base;
        }
        return 0xFFu;
    }
    case SPIFLASH_STATUS_READ:
        return spiflash_status(flash);
    case SPIFLASH_STATUS_WRITE:
        if (flash->write_enable) {
            spiflash_set_bp(flash, (mm_u8)(out & 0x1Cu));
            flash->write_enable = MM_FALSE;
        }
        flash->state = SPIFLASH_IDLE;
        return 0xFFu;
    default:
        break;
    }
    return 0xFFu;
}

void mm_spiflash_cs_deassert(struct mm_spiflash *flash)
{
    if (flash == 0) {
        return;
    }
    if (flash->state == SPIFLASH_PP && flash->dirty) {
        spiflash_sync(flash);
    }
    if (flash->cmd == 0x02) {
        flash->write_enable = MM_FALSE;
    }
    flash->state = SPIFLASH_IDLE;
    flash->cmd = 0;
    flash->addr = 0;
    flash->addr_have = 0;
    flash->addr_need = 0;
    flash->dummy_left = 0;
}

mm_bool mm_spiflash_is_locked(const struct mm_spiflash *flash)
{
    if (flash == 0) {
        return MM_FALSE;
    }
    return flash->locked;
}

static mm_bool spiflash_mmio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct mm_spiflash *flash = (struct mm_spiflash *)opaque;
    mm_u32 v = 0;
    if (flash == 0 || value_out == 0) {
        return MM_FALSE;
    }
    if (mm_spiflash_is_locked(flash)) {
        return MM_FALSE;
    }
    if (offset + size_bytes > flash->size) {
        return MM_FALSE;
    }
    if (size_bytes == 1u) {
        v = spiflash_read_byte(flash, offset);
    } else if (size_bytes == 2u) {
        v = (mm_u32)spiflash_read_byte(flash, offset) |
            ((mm_u32)spiflash_read_byte(flash, offset + 1u) << 8);
    } else if (size_bytes == 4u) {
        v = (mm_u32)spiflash_read_byte(flash, offset) |
            ((mm_u32)spiflash_read_byte(flash, offset + 1u) << 8) |
            ((mm_u32)spiflash_read_byte(flash, offset + 2u) << 16) |
            ((mm_u32)spiflash_read_byte(flash, offset + 3u) << 24);
    } else {
        return MM_FALSE;
    }
    *value_out = v;
    return MM_TRUE;
}

static mm_bool spiflash_mmio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    (void)offset;
    (void)size_bytes;
    (void)value;
    return MM_FALSE;
}

static int parse_bus_index(const char *s)
{
    int n = 0;
    if (s == 0) return -1;
    if (strncmp(s, "SPI", 3) != 0) return -1;
    s += 3;
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    if (*s != '\0') return -1;
    if (n <= 0) return -1;
    return n;
}

static mm_bool parse_u32(const char *s, mm_u32 *out)
{
    char *end = 0;
    unsigned long v;
    if (s == 0 || out == 0) return MM_FALSE;
    v = strtoul(s, &end, 0);
    if (end == 0 || *end != '\0') return MM_FALSE;
    *out = (mm_u32)v;
    return MM_TRUE;
}

mm_bool mm_spiflash_parse_spec(const char *spec, struct mm_spiflash_cfg *out)
{
    char tmp[512];
    char *tok;
    mm_bool have_file = MM_FALSE;
    mm_bool have_size = MM_FALSE;
    if (spec == 0 || out == 0) return MM_FALSE;
    memset(out, 0, sizeof(*out));
    strncpy(tmp, spec, sizeof(tmp) - 1u);
    tmp[sizeof(tmp) - 1u] = '\0';
    tok = strtok(tmp, ":");
    if (tok == 0) return MM_FALSE;
    out->bus = parse_bus_index(tok);
    if (out->bus < 0) return MM_FALSE;
    while ((tok = strtok(0, ":")) != 0) {
        if (strncmp(tok, "file=", 5) == 0) {
            strncpy(out->path, tok + 5, sizeof(out->path) - 1u);
            out->path[sizeof(out->path) - 1u] = '\0';
            have_file = MM_TRUE;
        } else if (strncmp(tok, "size=", 5) == 0) {
            if (!parse_u32(tok + 5, &out->size)) return MM_FALSE;
            have_size = MM_TRUE;
        } else if (strncmp(tok, "mmap=", 5) == 0) {
            if (!parse_u32(tok + 5, &out->mmap_base)) return MM_FALSE;
            out->mmap = MM_TRUE;
        } else {
            return MM_FALSE;
        }
    }
    if (!have_file || !have_size) return MM_FALSE;
    return MM_TRUE;
}

mm_bool mm_spiflash_register_cfg(const struct mm_spiflash_cfg *cfg)
{
    struct mm_spiflash *flash;
    if (cfg == 0) return MM_FALSE;
    if (g_spiflash_count >= SPIFLASH_MAX) return MM_FALSE;
    flash = &g_spiflash[g_spiflash_count++];
    memset(flash, 0, sizeof(*flash));
    flash->bus = cfg->bus;
    flash->size = cfg->size;
    flash->mmap = cfg->mmap;
    flash->mmap_base = cfg->mmap_base;
    strncpy(flash->path, cfg->path, sizeof(flash->path) - 1u);
    flash->path[sizeof(flash->path) - 1u] = '\0';
    if (!spiflash_load(flash)) {
        return MM_FALSE;
    }
    if (flash->mmap) {
        printf("[SPI_FLASH] SPI%d attached file=%s size=%lu mmap=0x%08lx\n",
               flash->bus,
               flash->path,
               (unsigned long)flash->size,
               (unsigned long)flash->mmap_base);
    } else {
        printf("[SPI_FLASH] SPI%d attached file=%s size=%lu\n",
               flash->bus,
               flash->path,
               (unsigned long)flash->size);
    }
    return MM_TRUE;
}

struct mm_spiflash *mm_spiflash_get_for_bus(int bus)
{
    size_t i;
    for (i = 0; i < g_spiflash_count; ++i) {
        if (g_spiflash[i].bus == bus) {
            return &g_spiflash[i];
        }
    }
    return 0;
}

void mm_spiflash_reset_all(void)
{
    size_t i;
    for (i = 0; i < g_spiflash_count; ++i) {
        mm_spiflash_cs_deassert(&g_spiflash[i]);
    }
}

void mm_spiflash_shutdown_all(void)
{
    size_t i;
    for (i = 0; i < g_spiflash_count; ++i) {
        printf("[SPI_FLASH] SPI%d disconnected\n", g_spiflash[i].bus);
        spiflash_sync(&g_spiflash[i]);
        free(g_spiflash[i].data);
        g_spiflash[i].data = 0;
    }
    g_spiflash_count = 0;
}

void mm_spiflash_register_mmap_regions(struct mmio_bus *bus)
{
    size_t i;
    if (bus == 0) return;
    for (i = 0; i < g_spiflash_count; ++i) {
        struct mm_spiflash *flash = &g_spiflash[i];
        struct mmio_region reg;
        if (!flash->mmap) continue;
        memset(&reg, 0, sizeof(reg));
        reg.base = flash->mmap_base;
        reg.size = flash->size;
        reg.opaque = flash;
        reg.read = spiflash_mmio_read;
        reg.write = spiflash_mmio_write;
        if (!mmio_bus_register_region(bus, &reg)) {
            fprintf(stderr, "[SPI_FLASH] failed to register mmap region SPI%d @0x%08lx size=%lu\n",
                    flash->bus,
                    (unsigned long)flash->mmap_base,
                    (unsigned long)flash->size);
        }
    }
}

void mm_spiflash_register_prot_regions(struct mm_prot_ctx *prot)
{
    size_t i;
    if (prot == 0) return;
    for (i = 0; i < g_spiflash_count; ++i) {
        struct mm_spiflash *flash = &g_spiflash[i];
        if (!flash->mmap) continue;
        if (!mm_prot_add_region(prot, flash->mmap_base, flash->size,
                                MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_SECURE)) {
            fprintf(stderr, "[SPI_FLASH] failed to add prot region SPI%d (S)\n", flash->bus);
        }
        if (!mm_prot_add_region(prot, flash->mmap_base, flash->size,
                                MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_NONSECURE)) {
            fprintf(stderr, "[SPI_FLASH] failed to add prot region SPI%d (NS)\n", flash->bus);
        }
    }
}
