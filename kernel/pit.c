// =============================================================================
//  kernel/pit.c -- Minuteur PIT 8254 sur IRQ0
// =============================================================================
#include "pit.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "klib.h"

#define PIT_FREQ 1193182u

static volatile uint64_t ticks = 0;
static uint32_t frequency = 100;

static void pit_irq(registers_t *r) {
    (void)r;
    ticks++;
}

void pit_init(uint32_t hz) {
    frequency = hz;
    uint32_t divisor = PIT_FREQ / hz;
    outb(0x43, 0x36);                     // canal 0, mode 3 (onde carrée)
    outb(0x40, divisor & 0xFF);           // octet bas
    outb(0x40, (divisor >> 8) & 0xFF);    // octet haut
    irq_register(0, pit_irq);
    pic_clear_mask(0);
    kprintf("[pit] minuteur a %u Hz\n", hz);
}

uint64_t pit_ticks(void) { return ticks; }

uint64_t pit_ms(void) {
    return (ticks * 1000) / frequency;
}

void pit_sleep_ms(uint32_t ms) {
    uint64_t target = pit_ms() + ms;
    while (pit_ms() < target) {
        __asm__ volatile ("hlt");
    }
}
