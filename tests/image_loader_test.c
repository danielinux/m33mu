/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "m33mu/image_loader.h"

static void write_le16(mm_u8 *p, mm_u16 value)
{
    p[0] = (mm_u8)value;
    p[1] = (mm_u8)(value >> 8);
}

static void write_le32(mm_u8 *p, mm_u32 value)
{
    p[0] = (mm_u8)value;
    p[1] = (mm_u8)(value >> 8);
    p[2] = (mm_u8)(value >> 16);
    p[3] = (mm_u8)(value >> 24);
}

static void make_elf(mm_u8 image[88])
{
    memset(image, 0, 88u);
    image[0] = 0x7fu;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 1u; /* ELFCLASS32 */
    image[5] = 1u; /* ELFDATA2LSB */
    image[6] = 1u; /* EV_CURRENT */
    write_le16(image + 18u, 40u); /* EM_ARM */
    write_le32(image + 28u, 52u); /* e_phoff */
    write_le16(image + 40u, 52u); /* e_ehsize */
    write_le16(image + 42u, 32u); /* e_phentsize */
    write_le16(image + 44u, 1u); /* e_phnum */
    write_le32(image + 52u, 1u); /* PT_LOAD */
    write_le32(image + 56u, 84u); /* p_offset */
    write_le32(image + 60u, 0x08000000u); /* p_vaddr */
    write_le32(image + 64u, 0x08000000u); /* p_paddr */
    write_le32(image + 68u, 4u); /* p_filesz */
    write_le32(image + 72u, 8u); /* p_memsz */
    image[84] = 0x01u;
    image[85] = 0x20u;
    image[86] = 0x70u;
    image[87] = 0x47u;
}

static void make_two_segment_elf(mm_u8 image[124])
{
    make_elf(image);
    write_le16(image + 44u, 2u);
    write_le32(image + 56u, 116u);
    write_le32(image + 68u, 4u);
    write_le32(image + 72u, 4u);
    write_le32(image + 84u, 1u);
    write_le32(image + 88u, 120u);
    write_le32(image + 92u, 0x08000004u);
    write_le32(image + 96u, 0x08000004u);
    write_le32(image + 100u, 4u);
    write_le32(image + 104u, 4u);
    image[116] = 0x11u;
    image[117] = 0x22u;
    image[118] = 0x33u;
    image[119] = 0x44u;
    image[120] = 0x55u;
    image[121] = 0x66u;
    image[122] = 0x77u;
    image[123] = 0x88u;
}

static int test_load_flash_segment(void)
{
    mm_u8 image[88];
    mm_u8 flash[64];
    struct mm_target_cfg cfg;
    struct mm_load_targets targets;
    struct mm_image_load_result result;

    make_elf(image);
    memset(flash, 0, sizeof(flash));
    memset(&cfg, 0, sizeof(cfg));
    memset(&targets, 0, sizeof(targets));
    cfg.flash_base_s = 0x08000000u;
    cfg.flash_size_s = sizeof(flash);
    targets.flash = flash;
    targets.flash_size = sizeof(flash);

    if (mm_elf_load_memory(image, sizeof(image), &cfg, &targets, 0u, &result) != 0) return 1;
    if (result.start != 0x08000000u || result.end != 0x08000004u || result.loaded != 4u) return 1;
    if (memcmp(flash, image + 84u, 4u) != 0) return 1;
    return 0;
}

static int test_reject_truncated_program_segment(void)
{
    mm_u8 image[124];
    mm_u8 flash[64];
    struct mm_target_cfg cfg;
    struct mm_load_targets targets;

    make_two_segment_elf(image);
    memset(flash, 0, sizeof(flash));
    memset(&cfg, 0, sizeof(cfg));
    memset(&targets, 0, sizeof(targets));
    cfg.flash_base_s = 0x08000000u;
    cfg.flash_size_s = sizeof(flash);
    targets.flash = flash;
    targets.flash_size = sizeof(flash);
    write_le32(image + 88u, 122u);

    if (mm_elf_load_memory(image, sizeof(image), &cfg, &targets, 0u, 0) == 0) return 1;
    if (flash[0] != 0u || flash[4] != 0u) return 1;
    return 0;
}

static int test_reject_image_over_configured_limit(void)
{
    mm_u8 image[88];
    mm_u8 flash[64];
    struct mm_target_cfg cfg;
    struct mm_load_targets targets;
    struct mm_image_load_result result;

    make_elf(image);
    memset(flash, 0, sizeof(flash));
    memset(&cfg, 0, sizeof(cfg));
    memset(&targets, 0, sizeof(targets));
    cfg.flash_base_s = 0x08000000u;
    cfg.flash_size_s = sizeof(flash);
    targets.flash = flash;
    targets.flash_size = sizeof(flash);
    memset(&result, 0xA5, sizeof(result));

    if (mm_elf_load_memory_bounded(image, sizeof(image), sizeof(image) - 1u,
                                   &cfg, &targets, 0u, &result) == 0) {
        return 1;
    }
    if (flash[0] != 0u || result.start != 0u || result.end != 0u ||
        result.loaded != 0u) {
        return 1;
    }
    if (mm_elf_load_memory_bounded(image, sizeof(image), sizeof(image),
                                   &cfg, &targets, 0u, &result) != 0) {
        return 1;
    }
    return 0;
}

static int test_flash_offset_for_linked_at_zero(void)
{
    mm_u8 image[88];
    mm_u8 flash[64];
    struct mm_target_cfg cfg;
    struct mm_load_targets targets;

    make_elf(image);
    memset(flash, 0, sizeof(flash));
    memset(&cfg, 0, sizeof(cfg));
    memset(&targets, 0, sizeof(targets));
    cfg.flash_base_s = 0x08000000u;
    cfg.flash_size_s = sizeof(flash);
    targets.flash = flash;
    targets.flash_size = sizeof(flash);
    write_le32(image + 60u, 0u);
    write_le32(image + 64u, 0u);

    if (mm_elf_load_memory(image, sizeof(image), &cfg, &targets, 8u, 0) != 0) return 1;
    if (memcmp(flash + 8u, image + 84u, 4u) != 0) return 1;
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += test_load_flash_segment();
    failures += test_reject_truncated_program_segment();
    failures += test_reject_image_over_configured_limit();
    failures += test_flash_offset_for_linked_at_zero();
    if (failures != 0) {
        printf("image_loader_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("image_loader_test: all cases passed\n");
    return 0;
}
