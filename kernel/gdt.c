// =============================================================================
//  kernel/gdt.c -- GDT 64 bits (code/données noyau + utilisateur) et TSS
// =============================================================================
#include "gdt.h"
#include "klib.h"

extern void gdt_flush(uint64_t gdtr);
extern void tss_flush(uint16_t sel);

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint64_t gdt[7];          // 5 entrées simples + TSS (2 mots de 8 octets)
static struct tss tss;
static struct gdtr gdtr;

// Pile d'interruption (utilisée comme rsp0 du TSS).
static uint8_t kernel_int_stack[16384] __attribute__((aligned(16)));

// Construit un descripteur 8 octets classique.
static uint64_t make_entry(uint8_t access, uint8_t flags) {
    uint64_t d = 0;
    d |= 0xFFFFULL;                 // limite 0:15
    d |= (uint64_t)access << 40;    // octet d'accès
    d |= (uint64_t)(flags & 0x0F) << 52; // drapeaux (dont bit L)
    d |= 0xFULL << 48;              // limite 16:19
    return d;
}

void gdt_init(void) {
    gdt[0] = 0;                                  // nul
    gdt[1] = make_entry(0x9A, 0xA);              // code noyau (exécutable, L=1)
    gdt[2] = make_entry(0x92, 0xC);              // données noyau
    gdt[3] = make_entry(0xFA, 0xA);              // code utilisateur (DPL=3)
    gdt[4] = make_entry(0xF2, 0xC);              // données utilisateur (DPL=3)

    // --- Descripteur TSS (16 octets, occupe gdt[5] et gdt[6]) ----------------
    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = (uint64_t)(kernel_int_stack + sizeof(kernel_int_stack));
    tss.iopb_offset = sizeof(tss);

    uint64_t base  = (uint64_t)&tss;
    uint64_t limit = sizeof(tss) - 1;
    uint64_t lo = 0;
    lo |= limit & 0xFFFF;
    lo |= (base & 0xFFFFFF) << 16;
    lo |= 0x89ULL << 40;                 // type TSS disponible, présent
    lo |= ((limit >> 16) & 0xF) << 48;
    lo |= ((base >> 24) & 0xFF) << 56;
    gdt[5] = lo;
    gdt[6] = (base >> 32) & 0xFFFFFFFF;  // base 32:63

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)&gdt;
    gdt_flush((uint64_t)&gdtr);
    tss_flush(SEL_TSS);

    kprintf("[gdt] GDT + TSS installes (rsp0=%p)\n", (void *)tss.rsp0);
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
