// =============================================================================
//  kernel/idt.h -- Table des interruptions (IDT) et répartition
// =============================================================================
#ifndef MONOS_IDT_H
#define MONOS_IDT_H

#include <stdint.h>

// État des registres au moment de l'interruption (cf. isr.asm).
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} registers_t;

typedef void (*irq_handler_t)(registers_t *);

void idt_init(void);
// Enregistre un gestionnaire pour une IRQ matérielle (0-15).
void irq_register(uint8_t irq, irq_handler_t handler);

#endif
