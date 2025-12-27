/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "m33mu/tpm_tis.h"
#include "m33mu/spi_bus.h"
#include "m33mu/gpio.h"

#if defined(M33MU_HAS_LIBTPMS) || defined(USE_LIBTPMS)
#include <libtpms/tpm_library.h>
#include <libtpms/tpm_error.h>
#endif

#define TPM_TIS_MAX 4
#define TPM_CMD_MAX 4096u
#define TPM_RSP_MAX 4096u

#define TPM_ACCESS         0x0000u
#define TPM_STS            0x0018u
#define TPM_DATA_FIFO      0x0024u
#define TPM_DID_VID        0x0F00u
#define TPM_RID            0x0F04u

#define TPM_ACCESS_VALID   0x80u
#define TPM_ACCESS_ACTIVE  0x20u
#define TPM_ACCESS_REQ     0x02u
#define TPM_ACCESS_RELINQ  0x20u

#define TPM_STS_VALID      0x80u
#define TPM_STS_COMMAND_READY 0x40u
#define TPM_STS_GO         0x20u
#define TPM_STS_DATA_AVAIL 0x10u
#define TPM_STS_EXPECT     0x08u

#if defined(M33MU_HAS_LIBTPMS) || defined(USE_LIBTPMS)
struct tpm_nv_entry {
    char name[64];
    mm_u8 *data;
    mm_u32 len;
};

struct tpm_nv_store {
    mm_bool use_file;
    char base[256];
    struct tpm_nv_entry entries[16];
};
#endif

struct mm_tpm_tis {
    int bus;
    mm_bool cs_valid;
    int cs_bank;
    int cs_pin;
    mm_u32 cs_mask;
    mm_u8 cs_level;
    mm_u8 header[4];
    mm_u8 hdr_have;
    mm_u16 addr;
    mm_u8 len;
    mm_bool is_read;
    mm_bool wait_phase;
    mm_u16 burst_count;
    mm_bool locality_active;
    mm_u8 cmd_buf[TPM_CMD_MAX];
    mm_u32 cmd_len;
    mm_u32 cmd_expected;
    mm_u8 rsp_buf[TPM_RSP_MAX];
    mm_u32 rsp_len;
    mm_u32 rsp_read;
#if defined(M33MU_HAS_LIBTPMS) || defined(USE_LIBTPMS)
    struct tpm_nv_store nv;
#endif
};

static struct mm_tpm_tis g_tpm[TPM_TIS_MAX];
static size_t g_tpm_count = 0;

static mm_bool tpm_spi_trace_enabled(void)
{
    static mm_bool init = MM_FALSE;
    static mm_bool enabled = MM_FALSE;
    if (!init) {
        const char *env = getenv("M33MU_SPI_TRACE");
        enabled = (env != 0 && env[0] != '\0') ? MM_TRUE : MM_FALSE;
        init = MM_TRUE;
    }
    return enabled;
}

static mm_bool tpm_tis_trace_enabled(void)
{
    static mm_bool init = MM_FALSE;
    static mm_bool enabled = MM_FALSE;
    if (!init) {
        const char *env = getenv("M33MU_TPM_TRACE");
        enabled = (env != 0 && env[0] != '\0') ? MM_TRUE : MM_FALSE;
        init = MM_TRUE;
    }
    return enabled;
}

static void tpm_tis_trace_dump(const char *tag, const mm_u8 *buf, mm_u32 len)
{
    mm_u32 i;
    mm_u32 max_len = len;
    if (!tpm_tis_trace_enabled()) {
        return;
    }
    if (buf == 0) {
        fprintf(stderr, "[TPM_TRACE] %s len=%lu (null)\n",
                tag, (unsigned long)len);
        return;
    }
    if (max_len > 256u) {
        max_len = 256u;
    }
    fprintf(stderr, "[TPM_TRACE] %s len=%lu\n", tag, (unsigned long)len);
    for (i = 0; i < max_len; i += 16u) {
        mm_u32 j;
        mm_u32 line_len = max_len - i;
        if (line_len > 16u) line_len = 16u;
        fprintf(stderr, "[TPM_TRACE]   %04lx:", (unsigned long)i);
        for (j = 0; j < line_len; ++j) {
            fprintf(stderr, " %02x", buf[i + j]);
        }
        fprintf(stderr, "\n");
    }
    if (max_len < len) {
        fprintf(stderr, "[TPM_TRACE]   ... (%lu bytes omitted)\n",
                (unsigned long)(len - max_len));
    }
}
static mm_u8 tpm_sample_cs(struct mm_tpm_tis *tpm)
{
    mm_u8 level = 1u;
    mm_u32 moder;
    mm_u32 mode_bits;
    if (tpm == 0 || !tpm->cs_valid) {
        return 0u;
    }
    if (!mm_gpio_bank_reader_present()) {
        return 1u;
    }
    moder = mm_gpio_bank_read_moder(tpm->cs_bank);
    mode_bits = (moder >> (tpm->cs_pin * 2)) & 0x3u;
    if (mode_bits != 1u) {
        level = 1u;
    } else {
        level = (mm_gpio_bank_read(tpm->cs_bank) & tpm->cs_mask) ? 1u : 0u;
    }
    if (level != tpm->cs_level) {
        tpm->cs_level = level ? 1u : 0u;
        if (tpm_spi_trace_enabled()) {
            printf("[SPI] SPI%d CS %s (P%c%d)\n",
                   tpm->bus,
                   tpm->cs_level ? "deasserted" : "asserted",
                   (char)('A' + tpm->cs_bank),
                   tpm->cs_pin);
        }
    }
    return level;
}

static mm_u8 tpm_cs_level(void *opaque)
{
    struct mm_tpm_tis *tpm = (struct mm_tpm_tis *)opaque;
    if (tpm == 0) {
        return 1u;
    }
    if (!tpm->cs_valid) {
        return 0u;
    }
    return tpm_sample_cs(tpm);
}

#if defined(M33MU_HAS_LIBTPMS) || defined(USE_LIBTPMS)
static void tpm_nv_free_entry(struct tpm_nv_entry *e)
{
    if (e->data != 0) {
        free(e->data);
        e->data = 0;
    }
    e->len = 0;
    e->name[0] = '\0';
}

static struct tpm_nv_entry *tpm_nv_find(struct tpm_nv_store *nv, const char *name)
{
    size_t i;
    if (nv == 0 || name == 0) return 0;
    for (i = 0; i < sizeof(nv->entries) / sizeof(nv->entries[0]); ++i) {
        if (nv->entries[i].name[0] == '\0') continue;
        if (strcmp(nv->entries[i].name, name) == 0) {
            return &nv->entries[i];
        }
    }
    return 0;
}

static struct tpm_nv_entry *tpm_nv_alloc(struct tpm_nv_store *nv, const char *name)
{
    size_t i;
    for (i = 0; i < sizeof(nv->entries) / sizeof(nv->entries[0]); ++i) {
        if (nv->entries[i].name[0] == '\0') {
            strncpy(nv->entries[i].name, name, sizeof(nv->entries[i].name) - 1u);
            nv->entries[i].name[sizeof(nv->entries[i].name) - 1u] = '\0';
            return &nv->entries[i];
        }
    }
    return 0;
}

static void tpm_nv_sanitize(char *out, size_t out_len, const char *name)
{
    size_t i = 0;
    if (out_len == 0) return;
    for (; name != 0 && *name != '\0' && i + 1 < out_len; ++name) {
        char c = *name;
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-') {
            out[i++] = c;
        } else {
            out[i++] = '_';
        }
    }
    out[i] = '\0';
}

static void tpm_nv_path(char *out, size_t out_len, const struct tpm_nv_store *nv, const char *name)
{
    char clean[96];
    tpm_nv_sanitize(clean, sizeof(clean), name);
    if (nv->base[0] == '\0') {
        sprintf(out, "%s", clean);
    } else {
        sprintf(out, "%s.%s", nv->base, clean);
    }
    if (out_len > 0) {
        out[out_len - 1u] = '\0';
    }
}

static struct tpm_nv_store *g_nv_store = 0;

static TPM_RESULT tpm_nvram_init(void)
{
    return TPM_SUCCESS;
}

static TPM_RESULT tpm_nvram_loaddata(unsigned char **data,
                                     uint32_t *length,
                                     uint32_t tpm_number,
                                     const char *name)
{
    struct tpm_nv_store *nv = g_nv_store;
    struct tpm_nv_entry *e;
    (void)tpm_number;
    if (nv == 0 || name == 0 || data == 0 || length == 0) return TPM_FAIL;
    e = tpm_nv_find(nv, name);
    if (e == 0 || e->data == 0) {
        if (nv->use_file) {
            FILE *f;
            char path[320];
            size_t n;
            tpm_nv_path(path, sizeof(path), nv, name);
            f = fopen(path, "rb");
            if (f == 0) {
                return TPM_RETRY;
            }
            fseek(f, 0, SEEK_END);
            n = (size_t)ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n == 0) {
                fclose(f);
                return TPM_RETRY;
            }
            *data = (unsigned char *)malloc(n);
            if (*data == 0) {
                fclose(f);
                return TPM_FAIL;
            }
            if (fread(*data, 1u, n, f) != n) {
                fclose(f);
                free(*data);
                *data = 0;
                return TPM_FAIL;
            }
            fclose(f);
            *length = (uint32_t)n;
            return TPM_SUCCESS;
        }
        return TPM_RETRY;
    }
    *data = (unsigned char *)malloc(e->len);
    if (*data == 0) return TPM_FAIL;
    memcpy(*data, e->data, e->len);
    *length = e->len;
    return TPM_SUCCESS;
}

static TPM_RESULT tpm_nvram_storedata(const unsigned char *data,
                                      uint32_t length,
                                      uint32_t tpm_number,
                                      const char *name)
{
    struct tpm_nv_store *nv = g_nv_store;
    struct tpm_nv_entry *e;
    char path[320];
    (void)tpm_number;
    if (nv == 0 || name == 0) return TPM_FAIL;
    e = tpm_nv_find(nv, name);
    if (e == 0) {
        e = tpm_nv_alloc(nv, name);
    }
    if (e == 0) return TPM_FAIL;
    tpm_nv_free_entry(e);
    if (length > 0) {
        e->data = (mm_u8 *)malloc(length);
        if (e->data == 0) return TPM_FAIL;
        memcpy(e->data, data, length);
        e->len = length;
    }
    if (nv->use_file) {
        FILE *f;
        tpm_nv_path(path, sizeof(path), nv, name);
        f = fopen(path, "wb");
        if (f == 0) return TPM_FAIL;
        if (length > 0) {
            size_t n = fwrite(data, 1u, length, f);
            if (n != length) {
                fclose(f);
                return TPM_FAIL;
            }
        }
        fclose(f);
    }
    return TPM_SUCCESS;
}

static TPM_RESULT tpm_nvram_deletename(uint32_t tpm_number,
                                       const char *name,
                                       TPM_BOOL mustExist)
{
    struct tpm_nv_store *nv = g_nv_store;
    struct tpm_nv_entry *e;
    char path[320];
    (void)tpm_number;
    if (nv == 0 || name == 0) return TPM_FAIL;
    e = tpm_nv_find(nv, name);
    if (e == 0 && mustExist) {
        return TPM_FAIL;
    }
    if (e != 0) {
        tpm_nv_free_entry(e);
    }
    if (nv->use_file) {
        tpm_nv_path(path, sizeof(path), nv, name);
        remove(path);
    }
    return TPM_SUCCESS;
}

static TPM_RESULT tpm_io_init(void)
{
    return TPM_SUCCESS;
}

static TPM_RESULT tpm_io_getlocality(TPM_MODIFIER_INDICATOR *localityModifier,
                                     uint32_t tpm_number)
{
    (void)tpm_number;
    if (localityModifier == 0) return TPM_FAIL;
    *localityModifier = 0;
    return TPM_SUCCESS;
}

static TPM_RESULT tpm_io_getphysicalpresence(TPM_BOOL *physicalPresence,
                                             uint32_t tpm_number)
{
    (void)tpm_number;
    if (physicalPresence == 0) return TPM_FAIL;
    *physicalPresence = FALSE;
    return TPM_SUCCESS;
}
#endif

static void tpm_reset(struct mm_tpm_tis *tpm)
{
    if (tpm == 0) return;
    tpm->hdr_have = 0;
    tpm->addr = 0;
    tpm->len = 0;
    tpm->is_read = MM_FALSE;
    tpm->wait_phase = MM_FALSE;
    tpm->burst_count = 64u;
    tpm->locality_active = MM_FALSE;
    tpm->cmd_len = 0;
    tpm->cmd_expected = 0;
    tpm->rsp_len = 0;
    tpm->rsp_read = 0;
}

static mm_u8 tpm_read_reg(struct mm_tpm_tis *tpm, mm_u16 addr)
{
    if (tpm == 0) return 0xFFu;
    switch (addr) {
    case TPM_ACCESS:
        return (mm_u8)(TPM_ACCESS_VALID | (tpm->locality_active ? TPM_ACCESS_ACTIVE : 0));
    case TPM_STS:
        return (mm_u8)(TPM_STS_VALID |
                       TPM_STS_COMMAND_READY |
                       ((tpm->cmd_expected == 0 || tpm->cmd_len < tpm->cmd_expected) ? TPM_STS_EXPECT : 0) |
                       (tpm->rsp_len > tpm->rsp_read ? TPM_STS_DATA_AVAIL : 0));
    case TPM_STS + 1u:
        return (mm_u8)(tpm->burst_count & 0xFFu);
    case TPM_STS + 2u:
        return (mm_u8)((tpm->burst_count >> 8) & 0xFFu);
    case TPM_DID_VID:
        return 0xD1u;
    case TPM_DID_VID + 1u:
        return 0x15u;
    case TPM_DID_VID + 2u:
        return 0x00u;
    case TPM_DID_VID + 3u:
        return 0x01u;
    case TPM_RID:
        return 0x00u;
    default:
        break;
    }
    if (addr >= TPM_DATA_FIFO && addr < TPM_DATA_FIFO + 4u) {
        if (tpm->rsp_read < tpm->rsp_len) {
            mm_u8 v = tpm->rsp_buf[tpm->rsp_read++];
            if (tpm->rsp_read >= tpm->rsp_len) {
                tpm->cmd_len = 0;
                tpm->cmd_expected = 0;
                tpm->rsp_len = 0;
                tpm->rsp_read = 0;
            }
            return v;
        }
        return 0xFFu;
    }
    return 0xFFu;
}

static void tpm_backend_process(struct mm_tpm_tis *tpm)
{
    if (tpm == 0) return;
    if (tpm->cmd_len == 0) return;
    tpm->rsp_len = 0;
    tpm->rsp_read = 0;
#if defined(M33MU_HAS_LIBTPMS) || defined(USE_LIBTPMS)
    {
        unsigned char *resp = 0;
        uint32_t resp_len = 0;
        uint32_t resp_bufsz = 0;
        TPM_RESULT rc;
        rc = TPMLIB_Process(&resp, &resp_len, &resp_bufsz,
                            (unsigned char *)tpm->cmd_buf, tpm->cmd_len);
        if (rc == TPM_SUCCESS && resp != 0 && resp_len > 0) {
            if (resp_len > TPM_RSP_MAX) {
                resp_len = TPM_RSP_MAX;
            }
            memcpy(tpm->rsp_buf, resp, resp_len);
            tpm->rsp_len = resp_len;
            free(resp);
        } else {
            tpm->rsp_buf[0] = 0x80u;
            tpm->rsp_buf[1] = 0x01u;
            tpm->rsp_buf[2] = 0x00u;
            tpm->rsp_buf[3] = 0x00u;
            tpm->rsp_buf[4] = 0x00u;
            tpm->rsp_buf[5] = 0x0Au;
            tpm->rsp_buf[6] = 0x00u;
            tpm->rsp_buf[7] = 0x00u;
            tpm->rsp_buf[8] = 0x01u;
            tpm->rsp_buf[9] = 0x01u;
            tpm->rsp_len = 10u;
        }
    }
#else
    tpm->rsp_buf[0] = 0x80u;
    tpm->rsp_buf[1] = 0x01u;
    tpm->rsp_buf[2] = 0x00u;
    tpm->rsp_buf[3] = 0x00u;
    tpm->rsp_buf[4] = 0x00u;
    tpm->rsp_buf[5] = 0x0Au;
    tpm->rsp_buf[6] = 0x00u;
    tpm->rsp_buf[7] = 0x00u;
    tpm->rsp_buf[8] = 0x01u;
    tpm->rsp_buf[9] = 0x01u;
    tpm->rsp_len = 10u;
#endif
    if (tpm_tis_trace_enabled()) {
        fprintf(stderr, "[TPM_TRACE] cmd=%lu rsp=%lu\n",
                (unsigned long)tpm->cmd_len,
                (unsigned long)tpm->rsp_len);
        tpm_tis_trace_dump("cmd", tpm->cmd_buf, tpm->cmd_len);
        tpm_tis_trace_dump("rsp", tpm->rsp_buf, tpm->rsp_len);
    }
}

static void tpm_write_reg(struct mm_tpm_tis *tpm, mm_u16 addr, mm_u8 value)
{
    if (tpm == 0) return;
    if (addr == TPM_ACCESS) {
        if ((value & TPM_ACCESS_REQ) != 0u) {
            tpm->locality_active = MM_TRUE;
        } else if ((value & TPM_ACCESS_RELINQ) != 0u) {
            tpm->locality_active = MM_FALSE;
        }
        return;
    }
    if (addr == TPM_STS) {
        if ((value & TPM_STS_COMMAND_READY) != 0u) {
            tpm->cmd_len = 0;
            tpm->cmd_expected = 0;
            tpm->rsp_len = 0;
            tpm->rsp_read = 0;
        }
        if ((value & TPM_STS_GO) != 0u) {
            tpm_backend_process(tpm);
        }
        return;
    }
    if (addr >= TPM_DATA_FIFO && addr < TPM_DATA_FIFO + 4u) {
        if (tpm->cmd_len < TPM_CMD_MAX) {
            tpm->cmd_buf[tpm->cmd_len++] = value;
            if (tpm->cmd_expected == 0 && tpm->cmd_len >= 6u) {
                mm_u32 size =
                    ((mm_u32)tpm->cmd_buf[2] << 24) |
                    ((mm_u32)tpm->cmd_buf[3] << 16) |
                    ((mm_u32)tpm->cmd_buf[4] << 8) |
                    (mm_u32)tpm->cmd_buf[5];
                if (size >= 10u && size <= TPM_CMD_MAX) {
                    tpm->cmd_expected = size;
                }
            }
        }
        return;
    }
}

static mm_u8 tpm_spi_xfer(void *opaque, mm_u8 out)
{
    struct mm_tpm_tis *tpm = (struct mm_tpm_tis *)opaque;
    mm_u8 cs_level;
    if (tpm == 0) {
        return 0xFFu;
    }
    cs_level = tpm_sample_cs(tpm);
    if (tpm->cs_valid && cs_level != 0u) {
        return 0xFFu;
    }

    if (tpm->hdr_have < 4u) {
        tpm->header[tpm->hdr_have++] = out;
        if (tpm->hdr_have == 4u) {
            tpm->is_read = ((tpm->header[0] & 0x80u) != 0u) ? MM_TRUE : MM_FALSE;
            tpm->len = (mm_u8)((tpm->header[0] & 0x7Fu) + 1u);
            tpm->addr = ((mm_u16)tpm->header[2] << 8) | (mm_u16)tpm->header[3];
            tpm->wait_phase = MM_TRUE;
        }
        return 0xFFu;
    }

    if (tpm->wait_phase) {
        mm_bool ready = MM_TRUE;
        if (tpm->is_read && tpm->addr == TPM_DATA_FIFO && tpm->rsp_len == 0) {
            ready = MM_FALSE;
        }
        if (ready) {
            tpm->wait_phase = MM_FALSE;
            return 0x01u;
        }
        return 0x00u;
    }

    if (tpm->is_read) {
        mm_u8 v = tpm_read_reg(tpm, tpm->addr);
        if (!(tpm->addr >= TPM_DATA_FIFO && tpm->addr < (TPM_DATA_FIFO + 4u))) {
            tpm->addr++;
        }
        if (tpm->len > 0) {
            tpm->len--;
            if (tpm->len == 0u) {
                tpm->hdr_have = 0u;
                tpm->wait_phase = MM_FALSE;
            }
        }
        return v;
    }

    tpm_write_reg(tpm, tpm->addr, out);
    if (!(tpm->addr >= TPM_DATA_FIFO && tpm->addr < (TPM_DATA_FIFO + 4u))) {
        tpm->addr++;
    }
    if (tpm->len > 0) {
        tpm->len--;
        if (tpm->len == 0u) {
            tpm->hdr_have = 0u;
            tpm->wait_phase = MM_FALSE;
        }
    }
    return 0xFFu;
}

static void tpm_spi_end(void *opaque)
{
    struct mm_tpm_tis *tpm = (struct mm_tpm_tis *)opaque;
    (void)tpm;
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
    if (n < 0) return -1;
    return n;
}

static mm_bool parse_gpio_name(const char *s, int *bank_out, int *pin_out)
{
    int bank;
    int pin = 0;
    const char *p;
    if (s == 0 || bank_out == 0 || pin_out == 0) return MM_FALSE;
    if (s[0] != 'P' && s[0] != 'p') return MM_FALSE;
    if (s[1] < 'A' || (s[1] > 'Z' && s[1] < 'a') || s[1] > 'z') return MM_FALSE;
    bank = (s[1] >= 'a') ? (s[1] - 'a') : (s[1] - 'A');
    p = s + 2;
    if (*p < '0' || *p > '9') return MM_FALSE;
    while (*p >= '0' && *p <= '9') {
        pin = (pin * 10) + (*p - '0');
        p++;
    }
    if (*p != '\0') return MM_FALSE;
    if (pin < 0 || pin > 15) return MM_FALSE;
    *bank_out = bank;
    *pin_out = pin;
    return MM_TRUE;
}

mm_bool mm_tpm_tis_parse_spec(const char *spec, struct mm_tpm_tis_cfg *out)
{
    char tmp[512];
    char *tok;
    if (spec == 0 || out == 0) return MM_FALSE;
    memset(out, 0, sizeof(*out));
    strncpy(tmp, spec, sizeof(tmp) - 1u);
    tmp[sizeof(tmp) - 1u] = '\0';
    tok = strtok(tmp, ":");
    if (tok == 0) return MM_FALSE;
    out->bus = parse_bus_index(tok);
    if (out->bus < 0) return MM_FALSE;
    while ((tok = strtok(0, ":")) != 0) {
        if (strncmp(tok, "cs=", 3) == 0) {
            if (!parse_gpio_name(tok + 3, &out->cs_bank, &out->cs_pin)) return MM_FALSE;
            out->cs_valid = MM_TRUE;
        } else if (strncmp(tok, "file=", 5) == 0) {
            strncpy(out->nv_path, tok + 5, sizeof(out->nv_path) - 1u);
            out->nv_path[sizeof(out->nv_path) - 1u] = '\0';
            out->has_nv_path = MM_TRUE;
        } else {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

mm_bool mm_tpm_tis_register_cfg(const struct mm_tpm_tis_cfg *cfg)
{
    struct mm_tpm_tis *tpm;
    struct mm_spi_device dev;
#if defined(M33MU_HAS_LIBTPMS) || defined(USE_LIBTPMS)
    static mm_bool libtpms_init = MM_FALSE;
    static struct libtpms_callbacks callbacks;
    if (!libtpms_init) {
        TPMLIB_ChooseTPMVersion(TPMLIB_TPM_VERSION_2);
        libtpms_init = MM_TRUE;
    }
#endif
    if (cfg == 0) return MM_FALSE;
    if (g_tpm_count >= TPM_TIS_MAX) return MM_FALSE;
    tpm = &g_tpm[g_tpm_count++];
    memset(tpm, 0, sizeof(*tpm));
    tpm->bus = cfg->bus;
    tpm->cs_valid = cfg->cs_valid;
    tpm->cs_bank = cfg->cs_bank;
    tpm->cs_pin = cfg->cs_pin;
    tpm->cs_mask = (cfg->cs_valid && cfg->cs_pin >= 0) ? (1u << (mm_u32)cfg->cs_pin) : 0u;
    tpm->cs_level = 1u;
    tpm->burst_count = 64u;
#if defined(M33MU_HAS_LIBTPMS) || defined(USE_LIBTPMS)
    tpm->nv.use_file = cfg->has_nv_path ? MM_TRUE : MM_FALSE;
    if (cfg->has_nv_path) {
        {
            size_t n = strlen(cfg->nv_path);
            if (n >= sizeof(tpm->nv.base)) {
                n = sizeof(tpm->nv.base) - 1u;
            }
            memcpy(tpm->nv.base, cfg->nv_path, n);
            tpm->nv.base[n] = '\0';
        }
    }
#else
#endif

#if defined(M33MU_HAS_LIBTPMS) || defined(USE_LIBTPMS)
    g_nv_store = &tpm->nv;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.sizeOfStruct = sizeof(callbacks);
    callbacks.tpm_nvram_init = tpm_nvram_init;
    callbacks.tpm_nvram_loaddata = tpm_nvram_loaddata;
    callbacks.tpm_nvram_storedata = tpm_nvram_storedata;
    callbacks.tpm_nvram_deletename = tpm_nvram_deletename;
    callbacks.tpm_io_init = tpm_io_init;
    callbacks.tpm_io_getlocality = tpm_io_getlocality;
    callbacks.tpm_io_getphysicalpresence = tpm_io_getphysicalpresence;
    TPMLIB_RegisterCallbacks(&callbacks);
    if (TPMLIB_MainInit() != TPM_SUCCESS) {
        fprintf(stderr, "[TPM] TPMLIB_MainInit failed\n");
    }
#endif

    printf("[TPM] SPI%d attached\n", tpm->bus);
    if (tpm->cs_valid) {
        printf("[TPM] SPI%d CS= P%c%d\n",
               tpm->bus,
               (char)('A' + tpm->cs_bank),
               tpm->cs_pin);
    }
#if defined(M33MU_HAS_LIBTPMS) || defined(USE_LIBTPMS)
    if (tpm->nv.use_file) {
        printf("[TPM] NV file base=%s\n", tpm->nv.base);
    }
#endif

    memset(&dev, 0, sizeof(dev));
    dev.bus = tpm->bus;
    dev.xfer = tpm_spi_xfer;
    dev.end = tpm_spi_end;
    dev.cs_level = tpm_cs_level;
    dev.opaque = tpm;
    if (!mm_spi_bus_register_device(&dev)) {
        fprintf(stderr, "[TPM] failed to register SPI%d device\n", tpm->bus);
        return MM_FALSE;
    }
    return MM_TRUE;
}

void mm_tpm_tis_reset_all(void)
{
    size_t i;
    for (i = 0; i < g_tpm_count; ++i) {
        tpm_reset(&g_tpm[i]);
    }
}

void mm_tpm_tis_shutdown_all(void)
{
    size_t i;
    for (i = 0; i < g_tpm_count; ++i) {
        printf("[TPM] SPI%d disconnected\n", g_tpm[i].bus);
    }
#if defined(M33MU_HAS_LIBTPMS) || defined(USE_LIBTPMS)
    TPMLIB_Terminate();
#endif
    g_tpm_count = 0;
}
