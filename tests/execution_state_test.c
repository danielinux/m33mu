#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m33mu/execution_state.h"
#include "m33mu/execution_exception.h"
#include "m33mu/gpio.h"
#include "m33mu/signal.h"
#include "m33mu/vector.h"

static char target_setup_order[16];
static size_t target_setup_count;
static mm_u32 fetch_fault_handler_calls;
static mm_bool fetch_fault_handler_result;
static mm_u32 fetch_fault_handler_addr;
static mm_bool usage_fault_handler_result;
static mm_u32 usage_fault_handler_calls;
static mm_u32 usage_fault_handler_addr;
static mm_bool usage_fault_state_owned;
static mm_bool irq_callback_state_owned;

static mm_bool test_session_irq_enter(void *opaque,
                                      struct mm_cpu *cpu,
                                      struct mm_memmap *map,
                                      struct mm_scs *scs,
                                      mm_u32 exception_number,
                                      mm_u32 return_pc,
                                      mm_u32 xpsr,
                                      enum mm_sec_state handler_sec)
{
    struct mm_execution_state *state = (struct mm_execution_state *)opaque;
    irq_callback_state_owned = state != 0 && cpu == &state->cores[0].cpu &&
        map == &state->map && scs == &state->cores[0].scs &&
        exception_number == MM_VECT_PENDSV && handler_sec == cpu->sec_state &&
        return_pc == (cpu->r[15] & ~1u) && xpsr == cpu->xpsr;
    scs->pend_sv = MM_FALSE;
    if (!mm_vector_read(map, handler_sec, scs->vtor_s,
                        exception_number, &cpu->r[15])) {
        return MM_FALSE;
    }
    cpu->mode = MM_HANDLER;
    return cpu->r[15] != 0u;
}

static mm_bool test_fetch_fault_handler(struct mm_cpu *cpu,
                                        struct mm_memmap *map,
                                        struct mm_scs *scs,
                                        mm_u32 fault_pc,
                                        mm_u32 fault_xpsr,
                                        mm_u32 addr,
                                        mm_bool is_exec)
{
    if (cpu == 0 || map == 0 || scs == 0 ||
        fault_pc != fetch_fault_handler_addr ||
        addr != fetch_fault_handler_addr || fault_xpsr != cpu->xpsr ||
        !is_exec) {
        return MM_FALSE;
    }
    fetch_fault_handler_calls++;
    return fetch_fault_handler_result;
}

static mm_bool test_usage_fault_handler(struct mm_cpu *cpu,
                                       struct mm_memmap *map,
                                       struct mm_scs *scs,
                                       mm_u32 fault_pc,
                                       mm_u32 fault_xpsr,
                                       mm_u32 ufsr_bits)
{
    usage_fault_handler_calls++;
    usage_fault_state_owned = cpu != 0 && map != 0 && scs != 0 &&
        fault_pc == usage_fault_handler_addr && fault_xpsr == cpu->xpsr &&
        ufsr_bits == (1u << 16);
    return usage_fault_handler_result;
}

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
static void test_usart_poll(void) { note_target_setup('A'); }
static void test_spi_poll(void) { note_target_setup('B'); }
static void test_eth_poll(void) { note_target_setup('C'); }

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

static int test_session_poll_quantum(void)
{
    struct mm_execution_state state;
    if (!mm_execution_state_init(&state)) {
        return 1;
    }
    state.target.usart_poll = test_usart_poll;
    state.target.spi_poll = test_spi_poll;
    state.target.eth_poll = test_eth_poll;
    state.clock.cycles_since_poll = 7u;
    target_setup_count = 0u;
    if (mm_execution_state_poll_target_devices_if_due(&state, 8u) ||
        target_setup_count != 0u || state.clock.cycles_since_poll != 7u) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    if (!mm_execution_state_poll_target_devices_if_due(&state, 7u) ||
        target_setup_count != 3u ||
        memcmp(target_setup_order, "ABC", 3u) != 0 ||
        state.clock.cycles_since_poll != 0u) {
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

static int test_session_sleep_wake(void)
{
    struct mm_execution_state state;
    struct mm_cpu *cpu;
    struct mm_nvic *nvic;
    mm_execution_state_init(&state);
    mm_scs_init(&state.cores[0].scs, 0x410fc241u);
    mm_nvic_init(&state.cores[0].nvic);
    cpu = &state.cores[0].cpu;
    nvic = &state.cores[0].nvic;

    if (mm_execution_state_cycles_until_wake(&state, 0u, 0u) !=
            (mm_u64)-1 ||
        mm_execution_state_cycles_until_wake(&state, 0u, 9u) != 9u ||
        mm_execution_state_cycles_until_wake(&state, 2u, 9u) !=
            (mm_u64)-1) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    state.cores[0].scs.systick_ctrl_s = 1u;
    state.cores[0].scs.systick_load_s = 11u;
    state.cores[0].scs.systick_val_s = 7u;
    state.cores[0].scs.systick_ctrl_ns = 1u;
    state.cores[0].scs.systick_load_ns = 5u;
    state.cores[0].scs.systick_val_ns = 3u;
    if (mm_execution_state_cycles_until_wake(&state, 0u, 0u) != 3u ||
        mm_execution_state_cycles_until_wake(&state, 0u, 2u) != 2u) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    state.cores[0].scs.systick_ctrl_ns = 0u;
    if (mm_execution_state_cycles_until_wake(&state, 0u, 0u) != 7u) {
        mm_execution_state_destroy(&state);
        return 1;
    }

    cpu->sleeping = MM_TRUE;
    cpu->sleep_wfe = MM_TRUE;
    cpu->event_reg = MM_TRUE;
    if (!mm_execution_state_wake_sleeping_core(&state, 0u) || cpu->sleeping ||
        cpu->sleep_wfe || cpu->event_reg) {
        mm_execution_state_destroy(&state);
        return 1;
    }

    cpu->sleeping = MM_TRUE;
    cpu->sleep_wfe = MM_TRUE;
    mm_nvic_set_pending(nvic, 4u, MM_TRUE);
    if (!mm_execution_state_wake_sleeping_core(&state, 0u) || cpu->sleeping) {
        mm_execution_state_destroy(&state);
        return 1;
    }

    cpu->sleeping = MM_TRUE;
    cpu->sleep_wfe = MM_FALSE;
    nvic->enable_mask[0] = 0u;
    if (mm_execution_state_wake_sleeping_core(&state, 0u) || !cpu->sleeping) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    nvic->enable_mask[0] = 1u << 4;
    if (!mm_execution_state_wake_sleeping_core(&state, 0u) || cpu->sleeping) {
        mm_execution_state_destroy(&state);
        return 1;
    }

    cpu->sleeping = MM_TRUE;
    state.cores[0].scs.pend_sv = MM_TRUE;
    if (!mm_execution_state_wake_sleeping_core(&state, 0u) || cpu->sleeping) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_destroy(&state);
    return 0;
}

static int test_session_pending_dispatch(void)
{
    struct mm_execution_state state;
    mm_u32 vector_addr;
    mm_bool done = MM_FALSE;
    enum mm_execution_irq_dispatch_result result;
    mm_execution_state_init(&state);
    if (!mm_execution_state_select_target(&state, "stm32h563") ||
        !mm_execution_state_allocate_storage(&state)) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_init_memory_map(&state);
    if (!mm_execution_state_configure_memory(&state)) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_scs_init(&state.cores[0].scs, 0x410fc241u);
    mm_nvic_init(&state.cores[0].nvic);
    state.cores[0].scs.vtor_s = state.target.ram_base_s;
    vector_addr = state.target.ram_base_s + (MM_VECT_PENDSV * 4u);
    state.cores[0].cpu.sec_state = MM_SECURE;
    state.cores[0].cpu.mode = MM_THREAD;
    state.cores[0].cpu.msp_s = state.target.ram_base_s + 0x1000u;
    state.cores[0].cpu.r[13] = state.cores[0].cpu.msp_s;
    state.cores[0].cpu.r[15] = 0x1235u;
    state.cores[0].cpu.xpsr = 0x21000000u;
    if (!mm_memmap_write_ram_raw(&state.map, vector_addr, 4u,
                                 state.target.ram_base_s + 0x201u)) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    state.cores[0].scs.pend_sv = MM_TRUE;
    irq_callback_state_owned = MM_FALSE;
    result = mm_execution_state_dispatch_pending(
        &state, 0u, MM_EXECUTION_IRQ_SKIP_BLOCKED,
        test_session_irq_enter, &state, &done);
    if (result != MM_EXECUTION_IRQ_DELIVERED || !irq_callback_state_owned ||
        state.cores[0].scs.pend_sv || done ||
        state.cores[0].cpu.r[15] != state.target.ram_base_s + 0x201u ||
        state.cores[0].cpu.mode != MM_HANDLER ||
        state.cores[0].cpu.exc_depth != 0u) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    if (mm_execution_state_dispatch_pending(
            &state, 2u, MM_EXECUTION_IRQ_SKIP_BLOCKED,
            test_session_irq_enter, &state, &done) != MM_EXECUTION_IRQ_NONE) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_destroy(&state);
    return 0;
}

static int test_session_target_registration(void)
{
    struct mm_execution_state state;
    int pins = 0;
    if (!mm_execution_state_init(&state) ||
        !mm_execution_state_select_target(&state, "stm32h563")) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_init_memory_map(&state);
    if (!mm_execution_state_register_target_mmio(&state) ||
        state.map.mmio.region_count == 0u ||
        !mm_gpio_bank_info(0u, 0, 0, &pins) || pins == 0) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    if (!mm_execution_state_allocate_storage(&state) ||
        !mm_execution_state_configure_memory(&state) ||
        !mm_execution_state_bind_target_flash(
            &state, state.storage.flash, state.target.flash_size_s, 0)) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_destroy(&state);
    return 0;
}

static int test_session_cycle_advance(void)
{
    struct mm_execution_state state;
    mm_execution_state_init(&state);
    mm_execution_state_advance_cycles(&state, 0u, 7u, MM_FALSE, 0u);
    mm_execution_state_advance_cycles(&state, 0u, 5u, MM_FALSE, 0u);
    if (state.clock.cycle_total != 12u ||
        state.clock.virtual_cycles != 12u ||
        state.clock.cycles_since_poll != 12u) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    state.cores[0].cpu.r[15] = 0x1235u;
    if (!mm_execution_state_advance_for_instruction(&state, 0u) ||
        state.clock.cycle_total != 13u ||
        state.clock.virtual_cycles != 13u ||
        state.clock.cycles_since_poll != 13u ||
        mm_execution_state_advance_for_instruction(&state, 2u) ||
        state.clock.cycle_total != 13u) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_advance_cycles(&state, 2u, 9u, MM_FALSE, 0u);
    if (state.clock.cycle_total != 13u ||
        state.clock.virtual_cycles != 13u ||
        state.clock.cycles_since_poll != 13u) {
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

static int test_session_step_decoded(void)
{
    struct mm_execution_state state;
    struct mm_execute_ctx context;
    struct mm_fetch_result fetch;
    struct mm_decoded decoded;
    enum mm_exec_status exec_status;
    enum mm_execution_instruction_outcome outcome;
    mm_bool done = MM_FALSE;

    mm_execution_state_init(&state);
    memset(&context, 0, sizeof(context));
    memset(&fetch, 0, sizeof(fetch));
    memset(&decoded, 0, sizeof(decoded));
    fetch.pc_fetch = 0x1000u;
    fetch.len = 2u;
    decoded.kind = MM_OP_ADD_IMM;
    decoded.len = 2u;
    decoded.rd = 0u;
    decoded.imm = 1u;
    context.done = &done;
    context.opt_gdb = MM_FALSE;
    state.cores[0].cpu.r[15] = 0x1003u;
    state.cores[0].cpu.r[0] = 7u;
    state.cores[0].it_pattern = 1u;
    state.cores[0].it_remaining = 1u;
    state.cores[0].it_cond = MM_COND_EQ;
    state.cores[0].cpu.xpsr = itstate_set(0u, 0x08u);

    outcome = mm_execution_state_step_decoded(
        &state, 0u, &context, &fetch, &decoded, MM_FALSE, &exec_status);
    if (outcome != MM_EXECUTION_INSTRUCTION_SKIPPED ||
        exec_status != MM_EXEC_CONTINUE || state.cores[0].cpu.r[0] != 7u ||
        state.cores[0].it_remaining != 0u) {
        mm_execution_state_destroy(&state);
        return 1;
    }

    decoded.kind = MM_OP_NOP;
    state.cores[0].it_remaining = 0u;
    outcome = mm_execution_state_step_decoded(
        &state, 0u, &context, &fetch, &decoded, MM_TRUE, &exec_status);
    if (outcome != MM_EXECUTION_INSTRUCTION_EXECUTED ||
        exec_status != MM_EXEC_OK || done) {
        mm_execution_state_destroy(&state);
        return 1;
    }

    decoded.kind = MM_OP_IT;
    decoded.imm = ((mm_u32)MM_COND_EQ << 4) | 0x08u;
    state.cores[0].it_remaining = 1u;
    state.cores[0].cpu.r[15] = 0x1003u;
    outcome = mm_execution_state_step_decoded(
        &state, 0u, &context, &fetch, &decoded, MM_FALSE, &exec_status);
    if (outcome != MM_EXECUTION_INSTRUCTION_EXECUTED ||
        exec_status != MM_EXEC_OK) {
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

static int test_single_live_session_and_signal_clock(void)
{
    struct mm_execution_state first;
    struct mm_execution_state second;
    if (!mm_execution_state_init(&first)) {
        return 1;
    }
    if (mm_execution_state_init(&second) ||
        mm_execution_state_signal_cycles_until_event(&second) !=
            (mm_u64)-1 ||
        mm_execution_state_signal_now_ns(&second) != 0u ||
        mm_execution_state_signal_set_cpu_hz(&second, 64000000u) ||
        mm_execution_state_signal_prepare_for_run(&second) ||
        mm_execution_state_signal_check_trigger(&second, 0u) ||
        mm_execution_state_signal_set_vdd_mv(&second, 3300) ||
        mm_execution_state_signal_set_gpio_vil_mv(&second, 1525) ||
        mm_execution_state_signal_set_gpio_vih_mv(&second, 1775) ||
        mm_execution_state_signal_load(&second, "unused.sigc") ||
        mm_execution_state_signal_bind(&second, "vin", 0, 5,
                                       MM_SIGNAL_ROLE_GPIO) != -1 ||
        mm_execution_state_signal_set_trigger(&second, 1u) ||
        mm_execution_state_signal_trigger_set(&second) ||
        mm_execution_state_signal_triggered(&second)) {
        mm_execution_state_destroy(&first);
        return 1;
    }
    if (!mm_execution_state_signal_set_cpu_hz(&first, 64000000u) ||
        mm_execution_state_signal_set_cpu_hz(&first, 0u) ||
        !mm_execution_state_signal_prepare_for_run(&first) ||
        !mm_execution_state_signal_check_trigger(&first, 0u) ||
        !mm_execution_state_signal_set_vdd_mv(&first, 3300) ||
        !mm_execution_state_signal_set_gpio_vil_mv(&first, 1525) ||
        !mm_execution_state_signal_set_gpio_vih_mv(&first, 1775) ||
        mm_execution_state_signal_gpio_vil_mv(&first) != 1525 ||
        mm_execution_state_signal_gpio_vih_mv(&first) != 1775 ||
        !mm_execution_state_signal_set_trigger(&first, 1u) ||
        !mm_execution_state_signal_trigger_set(&first) ||
        !mm_execution_state_signal_check_trigger(&first, 0u) ||
        mm_execution_state_signal_triggered(&first) ||
        !mm_execution_state_signal_check_trigger(&first, 1u) ||
        !mm_execution_state_signal_triggered(&first)) {
        mm_execution_state_destroy(&first);
        return 1;
    }
    mm_execution_state_advance_cycles(&first, 0u, 64000000u,
                                      MM_FALSE, 0u);
    if (mm_execution_state_signal_now_ns(&first) != 1000000000u ||
        mm_execution_state_signal_cycles_until_event(&first) !=
            (mm_u64)-1) {
        mm_execution_state_destroy(&first);
        return 1;
    }
    mm_execution_state_destroy(&second);
    mm_execution_state_destroy(&first);
    if (mm_signal_now_ns() != 0u ||
        !mm_execution_state_init(&second) ||
        mm_execution_state_signal_now_ns(&second) != 0u) {
        mm_execution_state_destroy(&second);
        return 1;
    }
    mm_execution_state_destroy(&second);
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
    struct mm_fetch_result fetch;
    struct mm_decoded decoded;
    mm_u8 image[88] = { 0 };
    mm_u32 loaded_word = 0u;
    mm_bool fpu_ok = MM_FALSE;
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
    {
        struct mm_execute_ctx context;
        enum mm_exec_status exec_status = MM_EXEC_CONTINUE;
        enum mm_execution_instruction_outcome outcome;
        mm_bool done = MM_FALSE;
        memset(&context, 0, sizeof(context));
        context.done = &done;
        outcome = mm_execution_state_step_decoded(
            &state, 0u, &context, &fetch, &decoded, MM_TRUE,
            &exec_status);
        if (outcome != MM_EXECUTION_INSTRUCTION_EXECUTED ||
            exec_status != MM_EXEC_OK || done ||
            state.cores[0].cpu.r[0] != 1u ||
            state.cores[0].cpu.r[15] != state.target.flash_base_s + 3u) {
            fprintf(stderr, "bounded step: outcome=%d status=%d done=%u r0=%08x pc=%08x expected=%08x\n",
                    (int)outcome, (int)exec_status, (unsigned)done,
                    state.cores[0].cpu.r[0], state.cores[0].cpu.r[15],
                    state.target.flash_base_s + 3u);
            mm_execution_state_destroy(&state);
            return 1;
        }
        state.cores[0].cpu.r[0] = 0u;
        state.cores[0].cpu.r[15] = state.target.flash_base_s | 1u;
        memset(&context, 0, sizeof(context));
        context.done = &done;
        outcome = mm_execution_state_step_instruction(
            &state, 0u, &context, test_fpu_allowed, test_decode,
            test_fetch_fault_handler, &fetch, &decoded, &fpu_ok,
            &exec_status);
        if (outcome != MM_EXECUTION_INSTRUCTION_EXECUTED ||
            exec_status != MM_EXEC_OK || state.cores[0].cpu.r[0] != 1u ||
            state.cores[0].cpu.r[15] != state.target.flash_base_s + 3u) {
            mm_execution_state_destroy(&state);
            return 1;
        }
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.undefined = MM_TRUE;
    fetch.pc_fetch = state.target.flash_base_s + 2u;
    usage_fault_handler_calls = 0u;
    usage_fault_handler_addr = fetch.pc_fetch;
    usage_fault_handler_result = MM_FALSE;
    usage_fault_state_owned = MM_FALSE;
    if (mm_execution_state_raise_undefined(
            &state, 0u, test_usage_fault_handler, &fetch, &decoded) !=
            MM_EXECUTION_FAULT_UNHANDLED ||
        usage_fault_handler_calls != 1u || !usage_fault_state_owned) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    usage_fault_handler_result = MM_TRUE;
    if (mm_execution_state_raise_undefined(
            &state, 0u, test_usage_fault_handler, &fetch, &decoded) !=
            MM_EXECUTION_FAULT_HANDLED ||
        usage_fault_handler_calls != 2u || !usage_fault_state_owned) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    decoded.undefined = MM_FALSE;
    if (mm_execution_state_raise_undefined(
            &state, 0u, test_usage_fault_handler, &fetch, &decoded) !=
        MM_EXECUTION_FAULT_INVALID) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    state.cores[0].cpu.r[15] = 0xdead0001u;
    fetch_fault_handler_calls = 0u;
    fetch_fault_handler_addr = 0xdead0000u;
    fetch_fault_handler_result = MM_FALSE;
    {
        struct mm_execute_ctx context;
        enum mm_exec_status exec_status = MM_EXEC_CONTINUE;
        memset(&context, 0, sizeof(context));
        if (mm_execution_state_step_instruction(
                &state, 0u, &context, test_fpu_allowed, test_decode,
                test_fetch_fault_handler, &fetch, &decoded, &fpu_ok,
                &exec_status) !=
                MM_EXECUTION_INSTRUCTION_FETCH_FAULT_UNHANDLED ||
        !fetch.fault || fetch_fault_handler_calls != 1u) {
            mm_execution_state_destroy(&state);
            return 1;
        }
        fetch_fault_handler_result = MM_TRUE;
        if (mm_execution_state_step_instruction(
                &state, 0u, &context, test_fpu_allowed, test_decode,
                test_fetch_fault_handler, &fetch, &decoded, &fpu_ok,
                &exec_status) !=
                MM_EXECUTION_INSTRUCTION_FETCH_FAULT_HANDLED ||
        !fetch.fault || fetch_fault_handler_calls != 2u) {
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
    write_le32(state.storage.flash,
               state.target.ram_base_s + state.target.ram_size_s);
    write_le32(state.storage.flash + 4u,
               state.target.flash_base_s + 0x101u);
    mm_execution_state_reset_cores(&state, state.target.core_count,
                                   state.target.flash_base_s,
                                   state.target.flash_base_ns);
    mm_scs_init(&state.cores[0].scs, 0x410fc241u);
    if (!mm_execution_state_apply_reset_vector(&state, MM_SECURE) ||
        mm_cpu_get_active_sp(&state.cores[0].cpu) !=
            state.target.ram_base_s + state.target.ram_size_s ||
        state.cores[0].cpu.r[15] != (state.target.flash_base_s + 0x101u) ||
        state.cores[0].cpu.mode != MM_THREAD ||
        state.cores[0].cpu.sec_state != MM_SECURE ||
        state.cores[0].scs.vtor_s != state.cores[0].cpu.vtor_s ||
        state.cores[0].scs.vtor_ns != state.cores[0].cpu.vtor_ns) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_destroy(&state);
    return 0;
}

static int test_session_core_system_setup(void)
{
    struct mm_execution_state state;
    mm_u32 i;
    if (!mm_execution_state_init(&state) ||
        !mm_execution_state_select_target(&state, "stm32h563")) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_init_memory_map(&state);
    mm_execution_state_reset_core_system(&state);
    if (!mm_execution_state_init_core_system(&state, 0x410fc241u) ||
        state.cores[0].scs.cpuid != 0x410fc241u ||
        state.core_sys.dwt.vcycles != &state.clock.virtual_cycles ||
        state.map.mmio.region_count == 0u) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    if ((state.target.flags & MM_TARGET_FLAG_FPU) != 0u) {
        for (i = 0u; i < state.target.core_count; ++i) {
            if ((state.cores[i].scs.cpacr_s & 0x00F00000u) != 0x00F00000u ||
                (state.cores[i].scs.cpacr_ns & 0x00F00000u) != 0x00F00000u ||
                (state.cores[i].scs.nsacr & ((1u << 10) | (1u << 11))) !=
                    ((1u << 10) | (1u << 11))) {
                mm_execution_state_destroy(&state);
                return 1;
            }
        }
    }
    mm_execution_state_destroy(&state);

    if (!mm_execution_state_init(&state) ||
        !mm_execution_state_select_target(&state, "imxrt700")) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_init_memory_map(&state);
    mm_execution_state_reset_core_system(&state);
    if (!mm_execution_state_init_core_system(&state, 0x410fc241u) ||
        state.scs_mux.core_count != 2u ||
        state.scs_mux.scs[0] != &state.cores[0].scs ||
        state.scs_mux.scs[1] != &state.cores[1].scs ||
        state.scs_mux.nvic[0] != &state.cores[0].nvic ||
        state.scs_mux.nvic[1] != &state.cores[1].nvic) {
        mm_execution_state_destroy(&state);
        return 1;
    }
    mm_execution_state_destroy(&state);
    return 0;
}

static int test_exception_fpu_and_secure_state_helpers(void)
{
    struct mm_cpu cpu;
    struct mm_scs scs;
    int i;
    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    cpu.mode = MM_THREAD;
    cpu.sec_state = MM_SECURE;
    cpu.priv_s = MM_FALSE;
    scs.fpu_present = MM_TRUE;
    scs.cpacr_s = 0x00f00000u;
    scs.fpu_present = MM_FALSE;
    if (mm_execution_exception_fpu_access_allowed(&cpu, &scs)) return 1;
    scs.fpu_present = MM_TRUE;
    if (!mm_execution_exception_fpu_access_allowed(&cpu, &scs)) return 1;
    cpu.priv_s = MM_TRUE;
    scs.cpacr_s = 0x00500000u;
    if (mm_execution_exception_fpu_access_allowed(&cpu, &scs)) return 1;
    scs.cpacr_s = 0x00f00000u;
    if (!mm_execution_exception_fpu_access_allowed(&cpu, &scs)) return 1;
    cpu.sec_state = MM_NONSECURE;
    cpu.priv_ns = MM_TRUE;
    scs.cpacr_ns = 0x00f00000u;
    if (mm_execution_exception_fpu_access_allowed(&cpu, &scs)) return 1;
    scs.nsacr = (1u << 10) | (1u << 11);
    if (!mm_execution_exception_fpu_access_allowed(&cpu, &scs)) return 1;
    cpu.sec_state = MM_SECURE;
    cpu.fp_active = MM_TRUE;
    cpu.control_s = 1u << 2;
    scs.fpccr = (1u << 31) | (1u << 30);
    if (!mm_execution_exception_fpu_auto_preserve_enabled(&cpu, &scs) ||
        !mm_execution_exception_fpu_lazy_preserve_enabled(&cpu, &scs)) {
        return 1;
    }
    scs.fpccr = 1u << 31;
    if (!mm_execution_exception_fpu_auto_preserve_enabled(&cpu, &scs) ||
        mm_execution_exception_fpu_lazy_preserve_enabled(&cpu, &scs)) {
        return 1;
    }
    if (!mm_execution_exception_additional_state_required(
            MM_SECURE, MM_NONSECURE) ||
        mm_execution_exception_additional_state_required(
            MM_NONSECURE, MM_SECURE) ||
        mm_execution_exception_additional_state_required(
            MM_SECURE, MM_SECURE)) {
        return 1;
    }
    for (i = 0; i < 8; ++i) cpu.r[4 + i] = (mm_u32)(0x44440000u + i);
    mm_execution_exception_save_secure_callee_regs(&cpu, 2);
    for (i = 0; i < 8; ++i) {
        if (cpu.exc_callee_saved[2][i] != (mm_u32)(0x44440000u + i) ||
            cpu.r[4 + i] != 0u) return 1;
    }
    if (!cpu.exc_cross_domain[2]) return 1;
    mm_execution_exception_save_secure_callee_regs(&cpu, MM_EXC_STACK_MAX);
    scs.cfsr_s = 0x10u;
    scs.bfar_s = 0x11111111u;
    mm_execution_exception_record_bus_fault(&scs, MM_SECURE,
                                             0x20001234u, 1u << 15);
    if (scs.cfsr_s != (0x10u | (1u << 15)) ||
        scs.bfar_s != 0x20001234u) return 1;
    scs.cfsr_ns = 0u;
    scs.bfar_ns = 0x22222222u;
    mm_execution_exception_record_bus_fault(&scs, MM_NONSECURE,
                                             0x30004321u, 1u << 11);
    if (scs.cfsr_ns != (1u << 11) || scs.bfar_ns != 0x22222222u) {
        return 1;
    }
    mm_execution_exception_record_bus_fault(0, MM_SECURE, 0u, 0u);
    return 0;
}

int main(void)
{
    struct mm_execution_state state;
    struct mmio_region external_regions[2];
    if (test_exception_fpu_and_secure_state_helpers() != 0) {
        fprintf(stderr, "execution_state_test: exception state helpers failed\n");
        return 1;
    }
    if (test_single_live_session_and_signal_clock() != 0) {
        fprintf(stderr, "execution_state_test: singleton signal clock failed\n");
        return 1;
    }
    if (test_session_resource_lifecycle() != 0) {
        fprintf(stderr, "execution_state_test: session resource lifecycle failed\n");
        return 1;
    }
    if (test_target_setup_order() != 0) {
        fprintf(stderr, "execution_state_test: target setup ordering failed\n");
        return 1;
    }
    if (test_session_target_registration() != 0) {
        fprintf(stderr, "execution_state_test: target registration failed\n");
        return 1;
    }
    if (test_session_poll_quantum() != 0) {
        fprintf(stderr, "execution_state_test: target polling quantum failed\n");
        return 1;
    }
    if (test_session_it_state() != 0) {
        fprintf(stderr, "execution_state_test: IT state handling failed\n");
        return 1;
    }
    if (test_session_sleep_wake() != 0) {
        fprintf(stderr, "execution_state_test: sleep/wake decision failed\n");
        return 1;
    }
    if (test_session_pending_dispatch() != 0) {
        fprintf(stderr, "execution_state_test: pending dispatch failed\n");
        return 1;
    }
    if (test_session_cycle_advance() != 0) {
        fprintf(stderr, "execution_state_test: cycle advancement failed\n");
        return 1;
    }
    if (test_session_execute_decoded() != 0) {
        fprintf(stderr, "execution_state_test: decoded execution failed\n");
        return 1;
    }
    if (test_session_step_decoded() != 0) {
        fprintf(stderr, "execution_state_test: decoded instruction decision failed\n");
        return 1;
    }
    if (test_session_bounded_elf_load() != 0) {
        fprintf(stderr, "execution_state_test: bounded session ELF load failed\n");
        return 1;
    }
    if (test_session_core_system_setup() != 0) {
        fprintf(stderr, "execution_state_test: core-system setup failed\n");
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
