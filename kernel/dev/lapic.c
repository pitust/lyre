#include <stdint.h>
#include <stddef.h>
#include <dev/lapic.h>
#include <sys/idt.h>
#include <sys/cpu.h>
#include <lib/print.h>
#include <lib/misc.h>
#include <dev/pit.h>
#include <mm/vmm.h>

#define LAPIC_REG_ID 0x20 // LAPIC ID
#define LAPIC_REG_EOI 0x0b0 // End of interrupt
#define LAPIC_REG_SPURIOUS 0x0f0
#define LAPIC_REG_CMCI 0x2f0 // LVT Corrected machine check interrupt
#define LAPIC_REG_ICR0 0x300 // Interrupt command register
#define LAPIC_REG_ICR1 0x310
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_TIMER_INITCNT 0x380 // Initial count register
#define LAPIC_REG_TIMER_CURCNT 0x390 // Current count register
#define LAPIC_REG_TIMER_DIV 0x3e0
#define LAPIC_EOI_ACK 0x00

static uint8_t timer_vec = 0;

static inline uint32_t lapic_read(uint32_t reg) {
    return *((volatile uint32_t *)((uintptr_t)0xfee00000 + VMM_HIGHER_HALF + reg));
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    *((volatile uint32_t *)((uintptr_t)0xfee00000 + VMM_HIGHER_HALF + reg)) = val;
}

static inline void lapic_timer_stop(void) {
    lapic_write(LAPIC_REG_TIMER_INITCNT, 0);
    lapic_write(LAPIC_REG_LVT_TIMER, 1 << 16);
}

static void lapic_timer_handler(int vector, struct cpu_ctx *ctx) {
    lapic_eoi();
    if (this_cpu()->timer_function != NULL) {
        this_cpu()->timer_function(vector, ctx);
    }
}

// Enable for all cores
void lapic_init(void) {
    ASSERT((rdmsr(0x1b) & 0xfffff000) == 0xfee00000);

    // Configure spurious IRQ
    lapic_write(LAPIC_REG_SPURIOUS, lapic_read(LAPIC_REG_SPURIOUS) | (1 << 8) | 0xff);

    // Timer interrupt
    if (timer_vec == 0) {
        timer_vec = idt_allocate_vector();
        idt_set_ist(timer_vec, 1);
    }
    isr[timer_vec] = lapic_timer_handler;
    lapic_write(LAPIC_REG_LVT_TIMER, lapic_read(LAPIC_REG_LVT_TIMER) | (1 << 8) | timer_vec);

    lapic_timer_calibrate();
}

void lapic_eoi(void) {
    lapic_write(LAPIC_REG_EOI, LAPIC_EOI_ACK);
}

void lapic_timer_oneshot(uint32_t us, void *function) {
    lapic_timer_stop();

    bool old_int_state = interrupt_toggle(false);
    this_cpu()->timer_function = function;
    interrupt_toggle(old_int_state);

    uint32_t ticks = us * (this_cpu()->lapic_freq / 1000000);
    lapic_write(LAPIC_REG_LVT_TIMER, timer_vec);
    lapic_write(LAPIC_REG_TIMER_DIV, 0);
    lapic_write(LAPIC_REG_TIMER_INITCNT, ticks);
}

void lapic_send_ipi(uint32_t lapic_id, uint32_t vec) {
    lapic_write(LAPIC_REG_ICR1, lapic_id << 24);
    lapic_write(LAPIC_REG_ICR0, vec);
}

void lapic_timer_calibrate(void) {
    lapic_timer_stop();

    // Initialize PIT
    lapic_write(LAPIC_REG_LVT_TIMER, (1 << 16) | 0xff);
    lapic_write(LAPIC_REG_TIMER_DIV, 0);
    pit_set_reload_value(0xffff); // Reset PIT

    int init_tick = pit_get_current_count();
    int samples = 0xfffff;
    lapic_write(LAPIC_REG_TIMER_INITCNT, (uint32_t)samples);
    while (lapic_read(LAPIC_REG_TIMER_CURCNT) != 0);
    int final_tick = pit_get_current_count();
    int total_ticks = init_tick - final_tick;
    this_cpu()->lapic_freq = (samples / total_ticks) * PIT_DIVIDEND;
    lapic_timer_stop();
}

uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_REG_ID);
}
