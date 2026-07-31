/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include "stm32h5f4/stm32h5f4_eth.h"
#include "stm32h5f4/stm32h5f4_mmio.h"
#include "m33mu/eth_backend.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"

#define ETH_BASE     0x40028000u
#define ETH_SEC_BASE 0x50028000u
#define ETH_SIZE     0x11F0u

#define ETH_MACCR     0x0000u
#define ETH_MACPFR    0x0004u
#define ETH_MACMDIOAR 0x0200u
#define ETH_MACMDIODR 0x0204u
#define ETH_MACA0HR   0x0300u
#define ETH_MACA0LR   0x0304u

#define ETH_MTLOMR    0x0C00u
#define ETH_MTLTXQOMR 0x0D00u
#define ETH_MTLRXQOMR 0x0D30u

#define ETH_DMAMR     0x1000u
#define ETH_DMASBMR   0x1004u
#define ETH_DMAISR    0x1008u
#define ETH_DMACTXCR  0x1104u
#define ETH_DMACRXCR  0x1108u
#define ETH_DMACTXDLAR 0x1114u
#define ETH_DMACRXDLAR 0x111Cu
#define ETH_DMACTXDTPR 0x1120u
#define ETH_DMACRXDTPR 0x1128u
#define ETH_DMACTXRLR  0x112Cu
#define ETH_DMACRXRLR  0x1130u
#define ETH_DMACIER    0x1134u
#define ETH_DMACSR     0x1160u

#define ETH_MACCR_RE (1u << 0)
#define ETH_MACCR_TE (1u << 1)

#define ETH_DMAMR_SWR (1u << 0)

#define ETH_DMACSR_TI  (1u << 0)
#define ETH_DMACSR_TBU (1u << 2)
#define ETH_DMACSR_RI  (1u << 6)
#define ETH_DMACSR_RBU (1u << 7)

#define ETH_DMACIER_TIE (1u << 0)
#define ETH_DMACIER_TBUE (1u << 2)
#define ETH_DMACIER_RIE (1u << 6)
#define ETH_DMACIER_RBUE (1u << 7)
#define ETH_DMACIER_AIE (1u << 14)
#define ETH_DMACIER_NIE (1u << 15)

#define ETH_TDES3_OWN (1u << 31)
#define ETH_TDES3_FD  (1u << 29)
#define ETH_TDES3_LD  (1u << 28)
#define ETH_TDES2_B1L_MASK 0x3FFFu

#define ETH_RDES3_OWN   (1u << 31)
#define ETH_RDES3_BUF1V (1u << 24)
#define ETH_RDES3_FS    (1u << 29)
#define ETH_RDES3_LS    (1u << 28)
#define ETH_RDES3_PL_MASK 0x3FFFu

#define ETH_MDIOAR_MB        (1u << 0)
#define ETH_MDIOAR_GOC_SHIFT 2u
#define ETH_MDIOAR_GOC_WRITE 0x1u
#define ETH_MDIOAR_GOC_READ  0x3u
#define ETH_MDIOAR_RDA_SHIFT 16u
#define ETH_MDIOAR_PA_SHIFT  21u

#define ETH_IRQ 106

/* LAN8742 PHY register indices and bits used by frosted. */
#define PHY_BCR      0x00u
#define PHY_BSR      0x01u
#define PHY_ID1      0x02u
#define PHY_ID2      0x03u
#define PHY_ANAR     0x04u
#define PHY_ANLPAR   0x05u
#define PHY_MCSR     0x11u
#define PHY_PHYSCSR  0x1Fu

#define PHY_BCR_RESET       (1u << 15)
#define PHY_BCR_AUTONEG_EN  (1u << 12)
#define PHY_BCR_RESTART_AN  (1u << 9)
#define PHY_BCR_SPEED_100   (1u << 13)
#define PHY_BCR_FDUPLEX     (1u << 8)

#define PHY_BSR_100BASE_TX_FD   (1u << 14)
#define PHY_BSR_100BASE_TX_HD   (1u << 13)
#define PHY_BSR_10BASE_T_FD     (1u << 12)
#define PHY_BSR_10BASE_T_HD     (1u << 11)
#define PHY_BSR_AUTONEG_CPLT    (1u << 5)
#define PHY_BSR_AUTONEG_ABILITY (1u << 3)
#define PHY_BSR_LINK_STATUS     (1u << 2)

#define PHY_ANAR_100BASE_TX_FD  (1u << 8)
#define PHY_ANAR_100BASE_TX     (1u << 7)
#define PHY_ANAR_10BASE_T_FD    (1u << 6)
#define PHY_ANAR_10BASE_T       (1u << 5)
#define PHY_ANAR_SELECTOR_8023  (0x1u)

#define PHY_PHYSCSR_AUTONEG_DONE (1u << 12)
#define PHY_PHYSCSR_HCDSPEEDMASK (0x1Cu)
#define PHY_PHYSCSR_100BTX_FD    (0x18u)

struct eth_desc {
    mm_u32 des0;
    mm_u32 des1;
    mm_u32 des2;
    mm_u32 des3;
};

struct stm32h5f4_eth {
    mm_u32 regs[ETH_SIZE / 4u];
    mm_u16 phy_regs[32];
    mm_u8 mac[6];
    mm_u32 tx_idx;
    mm_u32 rx_idx;
    struct mm_nvic *nvic;
    mm_u32 *rcc_regs;
};

static struct stm32h5f4_eth g_eth;

static mm_bool eth_clock_enabled(void)
{
    mm_u32 ahb1enr;
    if (g_eth.rcc_regs == 0) return MM_TRUE;
    ahb1enr = g_eth.rcc_regs[0x88u / 4u];
    return ((ahb1enr >> 19) & 1u) != 0u;
}

static mm_bool eth_tx_clock_enabled(void)
{
    mm_u32 ahb1enr;
    if (g_eth.rcc_regs == 0) return MM_TRUE;
    ahb1enr = g_eth.rcc_regs[0x88u / 4u];
    return ((ahb1enr >> 20) & 1u) != 0u;
}

static mm_bool eth_rx_clock_enabled(void)
{
    mm_u32 ahb1enr;
    if (g_eth.rcc_regs == 0) return MM_TRUE;
    ahb1enr = g_eth.rcc_regs[0x88u / 4u];
    return ((ahb1enr >> 21) & 1u) != 0u;
}

static void eth_raise_irq(void)
{
    if (g_eth.nvic == 0) return;
    mm_nvic_set_pending(g_eth.nvic, ETH_IRQ, MM_TRUE);
}

static void eth_update_irq(void)
{
    mm_u32 csr = g_eth.regs[ETH_DMACSR / 4u];
    mm_u32 ier = g_eth.regs[ETH_DMACIER / 4u];
    mm_bool normal = MM_FALSE;
    mm_bool abnormal = MM_FALSE;

    if ((csr & ETH_DMACSR_TI) && (ier & ETH_DMACIER_TIE)) normal = MM_TRUE;
    if ((csr & ETH_DMACSR_TBU) && (ier & ETH_DMACIER_TBUE)) normal = MM_TRUE;
    if ((csr & ETH_DMACSR_RI) && (ier & ETH_DMACIER_RIE)) normal = MM_TRUE;
    if ((csr & ETH_DMACSR_RBU) && (ier & ETH_DMACIER_RBUE)) abnormal = MM_TRUE;

    if ((normal && (ier & ETH_DMACIER_NIE)) || (abnormal && (ier & ETH_DMACIER_AIE))) {
        g_eth.regs[ETH_DMAISR / 4u] |= 1u;
        eth_raise_irq();
    }
}

static struct mm_memmap *eth_map(void)
{
    return mm_memmap_current();
}

static mm_bool eth_dma_read32(mm_u32 addr, mm_u32 *out)
{
    struct mm_memmap *map = eth_map();
    if (map == 0 || out == 0) return MM_FALSE;
    if (mm_memmap_read(map, MM_NONSECURE, addr, 4u, out)) return MM_TRUE;
    return mm_memmap_read(map, MM_SECURE, addr, 4u, out);
}

static mm_bool eth_dma_write32(mm_u32 addr, mm_u32 value)
{
    struct mm_memmap *map = eth_map();
    if (map == 0) return MM_FALSE;
    if (mm_memmap_write(map, MM_NONSECURE, addr, 4u, value)) return MM_TRUE;
    return mm_memmap_write(map, MM_SECURE, addr, 4u, value);
}

static mm_bool eth_dma_read_desc(mm_u32 addr, struct eth_desc *desc)
{
    if (!eth_dma_read32(addr + 0u, &desc->des0)) return MM_FALSE;
    if (!eth_dma_read32(addr + 4u, &desc->des1)) return MM_FALSE;
    if (!eth_dma_read32(addr + 8u, &desc->des2)) return MM_FALSE;
    if (!eth_dma_read32(addr + 12u, &desc->des3)) return MM_FALSE;
    return MM_TRUE;
}

static mm_bool eth_dma_write_desc(mm_u32 addr, const struct eth_desc *desc)
{
    if (!eth_dma_write32(addr + 0u, desc->des0)) return MM_FALSE;
    if (!eth_dma_write32(addr + 4u, desc->des1)) return MM_FALSE;
    if (!eth_dma_write32(addr + 8u, desc->des2)) return MM_FALSE;
    if (!eth_dma_write32(addr + 12u, desc->des3)) return MM_FALSE;
    return MM_TRUE;
}

static mm_u32 eth_desc_count(mm_u32 rlr)
{
    mm_u32 count = (rlr & 0x3FFu) + 1u;
    return count ? count : 1u;
}

static void eth_generate_mac(mm_u8 mac[6])
{
    mm_u64 v = 0;
    ssize_t n = getrandom(&v, sizeof(v), GRND_NONBLOCK);
    if (n != (ssize_t)sizeof(v)) {
        v = ((mm_u64)rand() << 32) ^ (mm_u64)rand();
    }
    mac[0] = (mm_u8)(v & 0xFFu);
    mac[1] = (mm_u8)((v >> 8) & 0xFFu);
    mac[2] = (mm_u8)((v >> 16) & 0xFFu);
    mac[3] = (mm_u8)((v >> 24) & 0xFFu);
    mac[4] = (mm_u8)((v >> 32) & 0xFFu);
    mac[5] = (mm_u8)((v >> 40) & 0xFFu);
    mac[0] = (mm_u8)((mac[0] & 0xFEu) | 0x02u);
}

static void eth_phy_reset(void)
{
    memset(g_eth.phy_regs, 0, sizeof(g_eth.phy_regs));
    g_eth.phy_regs[PHY_BCR] = PHY_BCR_AUTONEG_EN;
    g_eth.phy_regs[PHY_BSR] = (mm_u16)(PHY_BSR_AUTONEG_ABILITY |
                                       PHY_BSR_100BASE_TX_FD |
                                       PHY_BSR_100BASE_TX_HD |
                                       PHY_BSR_10BASE_T_FD |
                                       PHY_BSR_10BASE_T_HD);
    g_eth.phy_regs[PHY_ID1] = 0x0007u;
    g_eth.phy_regs[PHY_ID2] = 0xC0F0u;
    g_eth.phy_regs[PHY_ANAR] = (mm_u16)(PHY_ANAR_SELECTOR_8023 |
                                        PHY_ANAR_100BASE_TX_FD |
                                        PHY_ANAR_100BASE_TX |
                                        PHY_ANAR_10BASE_T_FD |
                                        PHY_ANAR_10BASE_T);
    g_eth.phy_regs[PHY_ANLPAR] = g_eth.phy_regs[PHY_ANAR];
    g_eth.phy_regs[PHY_PHYSCSR] = 0u;
}

static void eth_apply_mac(void)
{
    g_eth.regs[ETH_MACA0HR / 4u] = ((mm_u32)g_eth.mac[5] << 8) | g_eth.mac[4];
    g_eth.regs[ETH_MACA0LR / 4u] = ((mm_u32)g_eth.mac[3] << 24) |
                                   ((mm_u32)g_eth.mac[2] << 16) |
                                   ((mm_u32)g_eth.mac[1] << 8) |
                                   ((mm_u32)g_eth.mac[0]);
}

void mm_stm32h5f4_eth_reset(void)
{
    memset(g_eth.regs, 0, sizeof(g_eth.regs));
    g_eth.tx_idx = 0;
    g_eth.rx_idx = 0;
    eth_generate_mac(g_eth.mac);
    eth_apply_mac();
    fprintf(stderr, "[ETH_MAC] assigned MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            g_eth.mac[0], g_eth.mac[1], g_eth.mac[2],
            g_eth.mac[3], g_eth.mac[4], g_eth.mac[5]);
    eth_phy_reset();
}

void mm_stm32h5f4_eth_set_nvic(struct mm_nvic *nvic)
{
    g_eth.nvic = nvic;
}

void mm_stm32h5f4_eth_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    (void)bus;
    mm_stm32h5f4_eth_set_nvic(nvic);
}

static void eth_handle_mdio(void)
{
    mm_u32 mdioar = g_eth.regs[ETH_MACMDIOAR / 4u];
    mm_u32 goc;
    mm_u32 reg;
    mm_u32 phy;
    mm_bool link_up = mm_eth_backend_link_up();

    if ((mdioar & ETH_MDIOAR_MB) == 0u) return;

    goc = (mdioar >> ETH_MDIOAR_GOC_SHIFT) & 0x3u;
    reg = (mdioar >> ETH_MDIOAR_RDA_SHIFT) & 0x1Fu;
    phy = (mdioar >> ETH_MDIOAR_PA_SHIFT) & 0x1Fu;

    if (phy != 0u) {
        g_eth.regs[ETH_MACMDIODR / 4u] = 0xFFFFu;
    } else if (goc == ETH_MDIOAR_GOC_READ) {
        mm_u16 val = g_eth.phy_regs[reg];
        if (reg == PHY_BSR) {
            if (link_up) {
                val |= PHY_BSR_LINK_STATUS | PHY_BSR_AUTONEG_CPLT | PHY_BSR_AUTONEG_ABILITY;
            } else {
                val &= ~(PHY_BSR_LINK_STATUS | PHY_BSR_AUTONEG_CPLT);
            }
        } else if (reg == PHY_PHYSCSR) {
            if (link_up) {
                val |= PHY_PHYSCSR_AUTONEG_DONE;
                val &= ~PHY_PHYSCSR_HCDSPEEDMASK;
                val |= PHY_PHYSCSR_100BTX_FD;
            } else {
                val &= ~(PHY_PHYSCSR_AUTONEG_DONE | PHY_PHYSCSR_HCDSPEEDMASK);
            }
        }
        g_eth.regs[ETH_MACMDIODR / 4u] = val;
    } else if (goc == ETH_MDIOAR_GOC_WRITE) {
        mm_u16 val = (mm_u16)(g_eth.regs[ETH_MACMDIODR / 4u] & 0xFFFFu);
        g_eth.phy_regs[reg] = val;
        if (reg == PHY_BCR) {
            if ((val & PHY_BCR_RESET) != 0u) {
                eth_phy_reset();
                g_eth.phy_regs[PHY_BCR] &= ~PHY_BCR_RESET;
            }
            if ((val & PHY_BCR_RESTART_AN) != 0u) {
                g_eth.phy_regs[PHY_BCR] &= ~PHY_BCR_RESTART_AN;
            }
        }
    }

    g_eth.regs[ETH_MACMDIOAR / 4u] &= ~ETH_MDIOAR_MB;
}

static mm_bool eth_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0) return MM_FALSE;
    if (size_bytes != 4u) return MM_FALSE;
    if (!eth_clock_enabled()) {
        *value_out = 0;
        return MM_TRUE;
    }
    if (offset >= ETH_SIZE) return MM_FALSE;
    *value_out = g_eth.regs[offset / 4u];
    if (offset == ETH_MACMDIOAR) {
        eth_handle_mdio();
        *value_out = g_eth.regs[offset / 4u];
    }
    if (offset == ETH_MACA0HR || offset == ETH_MACA0LR) {
        eth_apply_mac();
        *value_out = g_eth.regs[offset / 4u];
    }
    return MM_TRUE;
}

static void eth_dma_reset(void)
{
    g_eth.regs[ETH_DMACSR / 4u] = 0u;
    g_eth.regs[ETH_DMAISR / 4u] = 0u;
    g_eth.tx_idx = 0;
    g_eth.rx_idx = 0;
}

static mm_bool eth_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    if (size_bytes != 4u) return MM_FALSE;
    if (offset >= ETH_SIZE) return MM_FALSE;
    if (!eth_clock_enabled()) return MM_TRUE;

    if (offset == ETH_DMAMR) {
        g_eth.regs[offset / 4u] = value;
        if ((value & ETH_DMAMR_SWR) != 0u) {
            eth_dma_reset();
            mm_stm32h5f4_eth_reset();
            g_eth.regs[ETH_DMAMR / 4u] &= ~ETH_DMAMR_SWR;
        }
        return MM_TRUE;
    }
    if (offset == ETH_DMACSR) {
        g_eth.regs[offset / 4u] &= ~value;
        eth_update_irq();
        return MM_TRUE;
    }
    if (offset == ETH_MACMDIOAR) {
        g_eth.regs[offset / 4u] = value;
        eth_handle_mdio();
        return MM_TRUE;
    }
    g_eth.regs[offset / 4u] = value;
    if (offset == ETH_MACA0HR) {
        g_eth.mac[5] = (mm_u8)((value >> 8) & 0xFFu);
        g_eth.mac[4] = (mm_u8)(value & 0xFFu);
    } else if (offset == ETH_MACA0LR) {
        g_eth.mac[3] = (mm_u8)((value >> 24) & 0xFFu);
        g_eth.mac[2] = (mm_u8)((value >> 16) & 0xFFu);
        g_eth.mac[1] = (mm_u8)((value >> 8) & 0xFFu);
        g_eth.mac[0] = (mm_u8)(value & 0xFFu);
    }
    return MM_TRUE;
}

static void eth_tx_poll(void)
{
    mm_u32 base;
    mm_u32 count;
    struct eth_desc desc;
    mm_bool te;
    mm_u32 processed = 0;

    if (!eth_clock_enabled() || !eth_tx_clock_enabled())
        return;
    te = (g_eth.regs[ETH_MACCR / 4u] & ETH_MACCR_TE) != 0u;
    if (!te)
        return;
    if ((g_eth.regs[ETH_DMACTXCR / 4u] & 1u) == 0u)
        return;

    base = g_eth.regs[ETH_DMACTXDLAR / 4u];
    count = eth_desc_count(g_eth.regs[ETH_DMACTXRLR / 4u]);
    while (processed < count) {
        mm_u32 idx = g_eth.tx_idx;
        mm_u32 addr = base + (idx * 16u);
        mm_u32 len;
        mm_u32 j;
        mm_u8 buf[1600];
        if (!eth_dma_read_desc(addr, &desc))
            break;
        if ((desc.des3 & ETH_TDES3_OWN) == 0u)
            break;
        len = desc.des2 & ETH_TDES2_B1L_MASK;
        if (len > sizeof(buf)) len = sizeof(buf);
        for (j = 0; j < len; ++j) {
            mm_u8 b = 0;
            if (!eth_dma_read32(desc.des0 + (j & ~3u), &desc.des1)) break;
            b = (mm_u8)((desc.des1 >> ((j & 3u) * 8u)) & 0xFFu);
            buf[j] = b;
        }
        mm_eth_backend_send(buf, len);
        desc.des3 &= ~ETH_TDES3_OWN;
        eth_dma_write_desc(addr, &desc);
        g_eth.regs[ETH_DMACSR / 4u] |= ETH_DMACSR_TI;
        eth_update_irq();
        g_eth.tx_idx = (idx + 1u) % count;
        processed++;
    }
}

static mm_u32 eth_rx_buf_len(const struct eth_desc *desc)
{
    mm_u32 len = desc->des3 & ETH_RDES3_PL_MASK;
    if (len == 0u) {
        len = (g_eth.regs[ETH_DMACRXCR / 4u] >> 1) & 0x3FFFu;
    }
    return len;
}

static void eth_rx_poll(void)
{
    mm_u8 buf[1600];
    int n;
    mm_u32 base;
    mm_u32 count;
    struct eth_desc desc;
    mm_u32 addr;
    mm_u32 len;
    mm_u32 i;
    mm_bool re;

    if (!eth_clock_enabled() || !eth_rx_clock_enabled())
        return;
    re = (g_eth.regs[ETH_MACCR / 4u] & ETH_MACCR_RE) != 0u;
    if (!re)
        return;
    if ((g_eth.regs[ETH_DMACRXCR / 4u] & 1u) == 0u)
        return;

    n = mm_eth_backend_recv(buf, sizeof(buf));
    if (n <= 0)
        return;

    base = g_eth.regs[ETH_DMACRXDLAR / 4u];
    count = eth_desc_count(g_eth.regs[ETH_DMACRXRLR / 4u]);
    addr = base + (g_eth.rx_idx * 16u);
    if (!eth_dma_read_desc(addr, &desc)) return;
    if ((desc.des3 & ETH_RDES3_OWN) == 0u) {
        g_eth.regs[ETH_DMACSR / 4u] |= ETH_DMACSR_RBU;
        eth_update_irq();
        return;
    }
    {
        mm_u32 buf1v = desc.des3 & ETH_RDES3_BUF1V;
        len = eth_rx_buf_len(&desc);
        if (len == 0u) return;
        if ((mm_u32)n > len) n = (int)len;
        for (i = 0; i < (mm_u32)n; ++i) {
            mm_u32 word_addr = desc.des0 + (i & ~3u);
            mm_u32 word;
            mm_u32 shift;
            if (!eth_dma_read32(word_addr, &word)) word = 0;
            shift = (i & 3u) * 8u;
            word &= ~(0xFFu << shift);
            word |= (mm_u32)buf[i] << shift;
            eth_dma_write32(word_addr, word);
        }
        desc.des3 = buf1v | ETH_RDES3_FS | ETH_RDES3_LS | ((mm_u32)n & ETH_RDES3_PL_MASK);
        eth_dma_write_desc(addr, &desc);
    }
    g_eth.regs[ETH_DMACSR / 4u] |= ETH_DMACSR_RI;
    eth_update_irq();
    g_eth.rx_idx = (g_eth.rx_idx + 1u) % count;
}

void mm_stm32h5f4_eth_poll(void)
{
    if (!eth_clock_enabled())
        return;
    eth_tx_poll();
    eth_rx_poll();
}

mm_bool mm_stm32h5f4_eth_get_mac(mm_u8 mac[6])
{
    if (mac == 0) {
        return MM_FALSE;
    }
    memcpy(mac, g_eth.mac, sizeof(g_eth.mac));
    return MM_TRUE;
}

mm_bool mm_stm32h5f4_eth_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;
    memset(&reg, 0, sizeof(reg));
    reg.base = ETH_BASE;
    reg.size = ETH_SIZE;
    reg.opaque = 0;
    reg.read = eth_read;
    reg.write = eth_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = ETH_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    g_eth.rcc_regs = mm_stm32h5f4_rcc_regs();
    return MM_TRUE;
}
