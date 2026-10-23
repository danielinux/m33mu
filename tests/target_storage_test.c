#include <stdio.h>
#include <string.h>

#include "m33mu/target_storage.h"

static int test_region_sizing_and_lifecycle(void)
{
    struct mm_ram_region regions[3];
    struct mm_target_cfg cfg;
    struct mm_target_storage storage;

    memset(&cfg, 0, sizeof(cfg));
    memset(regions, 0, sizeof(regions));
    memset(&storage, 0, sizeof(storage));
    cfg.flash_size_s = 32u;
    cfg.ram_regions = regions;
    cfg.ram_region_count = 3u;
    regions[0].size = 16u;
    regions[1].size = 8u;
    regions[1].alias_of = 1u;
    regions[2].size = 24u;

    if (mm_target_storage_ram_size(&cfg) != 40u ||
        !mm_target_storage_init(&storage, &cfg) ||
        storage.flash_size != 32u || storage.ram_size != 40u ||
        storage.flash == 0 || storage.ram == 0) {
        mm_target_storage_destroy(&storage);
        return 1;
    }
    memset(storage.flash, 0xA5, storage.flash_size);
    memset(storage.ram, 0x5A, storage.ram_size);
    mm_target_storage_destroy(&storage);
    if (storage.flash != 0 || storage.ram != 0 ||
        storage.flash_size != 0u || storage.ram_size != 0u) {
        return 1;
    }
    mm_target_storage_destroy(&storage);
    return 0;
}

static int test_failure_leaves_empty_storage(void)
{
    struct mm_target_cfg cfg;
    struct mm_target_storage storage;

    memset(&cfg, 0, sizeof(cfg));
    memset(&storage, 0xA5, sizeof(storage));
    if (mm_target_storage_init(&storage, &cfg) || storage.flash != 0 ||
        storage.ram != 0 || storage.flash_size != 0u || storage.ram_size != 0u) {
        mm_target_storage_destroy(&storage);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (test_region_sizing_and_lifecycle() != 0) {
        fprintf(stderr, "target storage region sizing/lifecycle failed\n");
        return 1;
    }
    if (test_failure_leaves_empty_storage() != 0) {
        fprintf(stderr, "target storage failure contract failed\n");
        return 1;
    }
    puts("target_storage_test: ok");
    return 0;
}
