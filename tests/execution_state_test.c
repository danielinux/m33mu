#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m33mu/execution_state.h"

static char target_setup_order[16];
static size_t target_setup_count;

static void note_target_setup(char value)
{
    if (target_setup_count < sizeof(target_setup_order)) {
        target_setup_order[target_setup_count++] = value;
    }
}

static void test_soc_reset(void) { note_target_setup('0'); }
static void test_timer_reset(void) { note_target_setup('1'); }
static void test_usart_reset(void) { note_target_setup('2'); }
static void test_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{ (void)bus; (void)nvic; note_target_setup('3'); }
static void test_spi_reset(void) { note_target_setup('4'); }
static void test_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{ (void)bus; (void)nvic; note_target_setup('5'); }
static void test_eth_reset(void) { note_target_setup('6'); }
static void test_eth_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{ (void)bus; (void)nvic; note_target_setup('7'); }
static void test_timer_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{ (void)bus; (void)nvic; note_target_setup('8'); }

static mm_bool test_fpu_allowed(const struct mm_cpu *cpu,
                                const struct mm_scs *scs)
{
    (void)cpu;
    (void)scs;
    return MM_TRUE;
}

static struct mm_decoded test_decode(const struct mm_fetch_result *fetch,
                                     const struct mm_cpu *cpu,
                                     const struct mm_scs *scs)
{
    (void)cpu;
    (void)scs;
    return mm_decode_t32(fetch);
}

static int test_target_setup_order(void)
{
    struct mm_execution_state state;
    mm_execution_state_init(&state);
    state.target.soc_reset = test_soc_reset;
    state.target.timer_reset = test_timer_reset;
    state.target.usart_reset = test_usart_reset;
    state.target.usart_init = test_usart_init;
    state.target.spi_reset = test_spi_reset;
    state.target.spi_init = test_spi_init;
    state.target.eth_reset = test_eth_reset;
    state.target.eth_init = test_eth_init;
    state.target.timer_init = test_timer_init;
    target_setup_count = 0u;
    mm_execution_state_reset_soc(&state);
    mm_execution_state_init_target_devices(&state);
    if (target_setup_count != 9u ||
        memcmp(target_setup_order, "012345678", 9u) != 0) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_destroy(&state);
    return 0;
}

static int test_session_it_state(void)
{
    struct mm_execution_state state;
    struct mm_decoded decoded;
    struct mm_fetch_result fetch;
    mm_bool should_execute;
    mm_execution_state_init(&state);
    memset(&decoded, 0, sizeof(decoded));
    decoded.kind = MM_OP_ADD_IMM;
    state.cores[0].it_pattern = 1u;
    state.cores[0].it_remaining = 1u;
    state.cores[0].it_cond = MM_COND_EQ;
    state.cores[0].cpu.xpsr = itstate_set(1u << 30, 0x08u);
    should_execute = mm_execution_state_should_execute(&state, 0u, &decoded);
    if (!should_execute) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    state.cores[0].cpu.xpsr = itstate_set(0u, 0x08u);
    should_execute = mm_execution_state_should_execute(&state, 0u, &decoded);
    if (should_execute) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    memset(&fetch, 0, sizeof(fetch));
    fetch.pc_fetch = 0x1000u;
    fetch.len = 2u;
    state.cores[0].cpu.r[15] = 0x1003u;
    if (!mm_execution_state_advance_it_if_sequential(&state, 0u,
                                                      &fetch, &decoded) ||
        state.cores[0].it_remaining != 0u) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    state.cores[0].it_remaining = 1u;
    state.cores[0].cpu.r[15] = 0x2001u;
    if (mm_execution_state_advance_it_if_sequential(&state, 0u,
                                                    &fetch, &decoded) ||
        state.cores[0].it_remaining != 1u) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_destroy(&state);
    return 0;
}

static int test_session_execute_decoded(void)
{
    struct mm_execution_state state;
    struct mm_execute_ctx context;
    struct mm_fetch_result fetch;
    struct mm_decoded decoded;
    mm_bool done = MM_FALSE;
    enum mm_exec_status status;

    mm_execution_state_init(&state);
    memset(&context, 0, sizeof(context));
    memset(&fetch, 0, sizeof(fetch));
    memset(&decoded, 0, sizeof(decoded));
    fetch.pc_fetch = 0x1000u;
    fetch.len = 2u;
    decoded.kind = MM_OP_NOP;
    decoded.len = 2u;
    context.done = &done;
    context.opt_gdb = MM_FALSE;
    context.gdb = 0;
    state.cores[0].cpu.r[15] = 0x1003u;
    state.cores[0].it_pattern = 1u;
    state.cores[0].it_remaining = 1u;
    state.cores[0].it_cond = MM_COND_EQ;
    state.cores[0].cpu.xpsr = itstate_set(1u << 30, 0x08u);

    status = mm_execution_state_execute_decoded(&state, 0u, &context,
                                                 &fetch, &decoded, MM_TRUE);
    if (status != MM_EXEC_OK || done || context.cpu != &state.cores[0].cpu ||
        context.map != &state.map || context.scs != &state.cores[0].scs ||
        context.nvic != &state.cores[0].nvic ||
        context.it_remaining != &state.cores[0].it_remaining ||
        state.cores[0].it_remaining != 0u) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_destroy(&state);
    return 0;
}

static int test_dual_core_reset_keeps_staged_peripheral_state(void)
{
    struct mm_execution_state state;
    memset(&state, 0xA5, sizeof(state));
    state.cores[0].scs.cpuid = 0x410fc241u;
    state.cores[1].scs.cpuid = 0x410fc241u;
    state.cores[0].nvic.enable_mask[0] = 0x12345678u;
    state.cores[1].nvic.enable_mask[0] = 0x87654321u;

    mm_execution_state_reset_cores(&state, 2u, 0x1000u, 0x2000u);
    if (state.cores[0].cpu.vtor_s != 0x1000u ||
        state.cores[0].cpu.vtor_ns != 0x2000u ||
        state.cores[0].cpu.sleeping || state.cores[0].cpu.sleep_wfe ||
        state.cores[1].cpu.vtor_s != 0x1000u ||
        state.cores[1].cpu.vtor_ns != 0x2000u ||
        !state.cores[1].cpu.sleeping || !state.cores[1].cpu.sleep_wfe ||
        state.active_core != 0u ||
        state.cores[0].it_pattern != 0u || state.cores[0].it_remaining != 0u ||
        state.cores[0].it_cond != 0u || state.cores[1].it_pattern != 0u ||
        state.cores[1].it_remaining != 0u || state.cores[1].it_cond != 0u) {
        return 1;
    }
    if (state.cores[0].scs.cpuid != 0x410fc241u ||
        state.cores[1].scs.cpuid != 0x410fc241u ||
        state.cores[0].nvic.enable_mask[0] != 0x12345678u ||
        state.cores[1].nvic.enable_mask[0] != 0x87654321u) {
        return 1;
    }
    return 0;
}

static int test_session_resource_lifecycle(void)
{
    struct mm_execution_state state;
    mm_execution_state_init(&state);
    if (state.active_core != 0u || state.clock.cycle_total != 0u ||
        state.storage.flash != 0 || state.code_cache.page_gen != 0) {
        return 1;
    }
    state.storage.flash = (mm_u8 *)malloc(1u);
    state.storage.flash_size = 1u;
    state.code_cache.page_gen = (mm_u32 *)malloc(sizeof(mm_u32));
    if (state.storage.flash == 0 || state.code_cache.page_gen == 0) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_end_run(&state);
    if (state.code_cache.page_gen != 0 || state.storage.flash == 0) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_destroy(&state);
    if (state.storage.flash != 0 || state.storage.ram != 0 ||
        state.code_cache.page_gen != 0) {
        return 1;
    }
    return 0;
}

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

static int test_session_bounded_elf_load(void)
{
    struct mm_execution_state state;
    struct mm_image_load_result result;
    mm_u8 image[88] = { 0 };
    mm_u32 loaded_word = 0u;
    mm_execution_state_init(&state);
    if (!mm_execution_state_select_target(&state, "stm32h563") ||
        !mm_execution_state_allocate_storage(&state)) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    if (state.storage.flash[0] != 0xffu || state.storage.ram[0] != 0u) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    image[0] = 0x7fu; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1u; image[5] = 1u; image[6] = 1u;
    write_le16(image + 18u, 40u);
    write_le32(image + 28u, 52u);
    write_le16(image + 40u, 52u);
    write_le16(image + 42u, 32u);
    write_le16(image + 44u, 1u);
    write_le32(image + 52u, 1u);
    write_le32(image + 56u, 84u);
    write_le32(image + 60u, state.target.flash_base_s);
    write_le32(image + 64u, state.target.flash_base_s);
    write_le32(image + 68u, 4u);
    write_le32(image + 72u, 4u);
    image[84] = 0x01u; image[85] = 0x20u;
    image[86] = 0x70u; image[87] = 0x47u;
    if (mm_execution_state_load_elf(&state, image, sizeof(image),
                                    sizeof(image), 0u, &result) != 0 ||
        result.start != state.target.flash_base_s || result.loaded != 4u ||
        memcmp(state.storage.flash, image + 84u, 4u) != 0) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_init_memory_map(&state);
    if (!mm_execution_state_configure_memory(&state) ||
        !mm_memmap_read(&state.map, MM_SECURE, state.target.flash_base_s,
                        4u, &loaded_word) || loaded_word != 0x47702001u) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_init_code_cache(&state);
    if (state.map.code_cache != &state.code_cache) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_reset_cores(&state, 1u,
                                   state.target.flash_base_s,
                                   state.target.flash_base_ns);
    state.cores[0].cpu.r[15] = state.target.flash_base_s | 1u;
    mm_scs_init(&state.cores[0].scs, 0x410fc241u);
    {
        struct mm_fetch_result fetch;
        struct mm_decoded decoded;
        mm_bool fpu_ok = MM_FALSE;
        if (!mm_execution_state_fetch_decode(&state, test_fpu_allowed,
                                             test_decode, &fetch, &decoded,
                                             &fpu_ok) ||
            fetch.fault || fetch.pc_fetch != state.target.flash_base_s ||
            fetch.len != 2u || !fpu_ok || decoded.kind != MM_OP_MOV_IMM ||
            decoded.rd != 0u || decoded.imm != 1u ||
            state.cores[0].cpu.r[15] != state.target.flash_base_s + 3u) {
            fprintf(stderr, "fetch/decode: fault=%u pc=%08x len=%u fpu=%u kind=%u rd=%u imm=%u next=%08x base=%08x\n",
                    (unsigned)fetch.fault, fetch.pc_fetch, (unsigned)fetch.len,
                    (unsigned)fpu_ok, (unsigned)decoded.kind,
                    (unsigned)decoded.rd, decoded.imm,
                    state.cores[0].cpu.r[15], state.target.flash_base_s);
            mm_execution_state_destroy(&state);
            return 1;
        }
    }
    memset(&result, 0xA5, sizeof(result));
    if (mm_execution_state_load_elf(&state, image, sizeof(image),
                                    sizeof(image) - 1u, 0u, &result) == 0 ||
        result.start != 0u || result.end != 0u || result.loaded != 0u) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_destroy(&state);
    return 0;
}

int main(void)
{
    struct mm_execution_state state;
    struct mmio_region external_regions[2];
    if (test_session_resource_lifecycle() != 0) {
        fprintf(stderr, "execution_state_test: session resource lifecycle failed\n");
        return 1;
    }
    if (test_target_setup_order() != 0) {
        fprintf(stderr, "execution_state_test: target setup ordering failed\n");
        return 1;
    }
    if (test_session_it_state() != 0) {
        fprintf(stderr, "execution_state_test: IT state handling failed\n");
        return 1;
    }
    if (test_session_execute_decoded() != 0) {
        fprintf(stderr, "execution_state_test: decoded execution failed\n");
        return 1;
    }
    if (test_session_bounded_elf_load() != 0) {
        fprintf(stderr, "execution_state_test: bounded session ELF load failed\n");
        return 1;
    }
    if (test_dual_core_reset_keeps_staged_peripheral_state() != 0) {
        fprintf(stderr, "execution_state_test: dual-core reset failed\n");
        return 1;
    }
    mm_execution_state_reset_cores(0, 1u, 0u, 0u);
    memset(&state, 0, sizeof(state));
    mm_execution_state_reset_cores(&state, 1u, 0x3000u, 0x4000u);
    if (state.cores[0].cpu.vtor_s != 0x3000u ||
        state.cores[0].cpu.vtor_ns != 0x4000u ||
        state.cores[1].cpu.sleeping || state.active_core != 0u) {
        fprintf(stderr, "execution_state_test: single-core reset failed\n");
        return 1;
    }
    mm_memmap_init(&state.map, state.regions,
                   sizeof(state.regions) / sizeof(state.regions[0]));
    if (state.map.mmio.regions != state.regions ||
        state.map.mmio.region_capacity !=
            sizeof(state.regions) / sizeof(state.regions[0])) {
        fprintf(stderr, "execution_state_test: owned MMIO storage binding failed\n");
        return 1;
    }
    state.core_sys.dwt.vcycles = &state.clock.virtual_cycles;
    if (!mm_core_sys_register(&state.map.mmio, &state.core_sys) ||
        state.map.mmio.region_count == 0u ||
        state.core_sys.dwt.vcycles != &state.clock.virtual_cycles) {
        fprintf(stderr, "execution_state_test: owned core system registration failed\n");
        return 1;
    }
    mm_memmap_init(&state.map, external_regions,
                   sizeof(external_regions) / sizeof(external_regions[0]));
    if (state.map.mmio.regions != external_regions ||
        state.map.mmio.region_capacity != 2u) {
        fprintf(stderr, "execution_state_test: external MMIO storage binding failed\n");
        return 1;
    }
    puts("execution_state_test: ok");
    return 0;
}
