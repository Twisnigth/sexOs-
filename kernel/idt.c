// =============================================================================
//  kernel/idt.c -- Installation de l'IDT et répartition des interruptions
// =============================================================================
#include "idt.h"
#include "gdt.h"
#include "pic.h"
#include "klib.h"
#include "io.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern void idt_flush(uint64_t idtr);

static struct idt_entry idt[256];
static struct idtr idtr;
static irq_handler_t irq_handlers[16];

// Déclarations des stubs assembleur (isr.asm).
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

static void idt_set(int n, void (*handler)(void), uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    idt[n].offset_low  = addr & 0xFFFF;
    idt[n].selector    = SEL_KCODE;
    idt[n].ist         = 0;
    idt[n].type_attr   = type_attr;       // 0x8E = interrupt gate, présent, DPL0
    idt[n].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[n].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[n].zero        = 0;
}

static const char *exception_names[] = {
    "Division par zero", "Debug", "NMI", "Point d'arret", "Depassement",
    "BOUND", "Opcode invalide", "Peripherique indisponible", "Double faute",
    "Depassement de segment coproc.", "TSS invalide", "Segment absent",
    "Faute de pile", "Protection generale", "Faute de page", "Reserve",
    "Erreur x87", "Verification d'alignement", "Verification machine",
    "Exception SIMD", "Virtualisation", "Securite control-flow"
};

// Appelé depuis isr.asm.
void isr_dispatch(registers_t *r) {
    if (r->int_no < 32) {
        const char *name = (r->int_no < sizeof(exception_names) / sizeof(char *))
                           ? exception_names[r->int_no] : "Inconnue";
        kprintf("\n[EXCEPTION] %u : %s\n", r->int_no, name);
        kprintf("  err=%x rip=%p cs=%x rflags=%x\n",
                r->err_code, (void *)r->rip, r->cs, r->rflags);
        kprintf("  rax=%p rbx=%p rcx=%p rdx=%p\n",
                (void *)r->rax, (void *)r->rbx, (void *)r->rcx, (void *)r->rdx);
        if (r->int_no == 14) {
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            kprintf("  cr2 (adresse fautive) = %p\n", (void *)cr2);
        }
        panic("exception non recuperable");
    } else if (r->int_no >= 32 && r->int_no < 48) {
        uint8_t irq = (uint8_t)(r->int_no - 32);
        if (irq_handlers[irq]) irq_handlers[irq](r);
        pic_send_eoi(irq);
    }
}

void irq_register(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) irq_handlers[irq] = handler;
}

void idt_init(void) {
    void (*isrs[32])(void) = {
        isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,isr10,isr11,
        isr12,isr13,isr14,isr15,isr16,isr17,isr18,isr19,isr20,isr21,isr22,
        isr23,isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31
    };
    void (*irqs[16])(void) = {
        irq0,irq1,irq2,irq3,irq4,irq5,irq6,irq7,
        irq8,irq9,irq10,irq11,irq12,irq13,irq14,irq15
    };

    for (int i = 0; i < 32; i++) idt_set(i, isrs[i], 0x8E);
    for (int i = 0; i < 16; i++) idt_set(32 + i, irqs[i], 0x8E);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;
    idt_flush((uint64_t)&idtr);

    kprintf("[idt] IDT installee (256 vecteurs)\n");
}
