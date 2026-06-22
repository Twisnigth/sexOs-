// =============================================================================
//  kernel/gdt.h -- Table des descripteurs globaux (GDT) + TSS
// =============================================================================
#ifndef SEXOS_GDT_H
#define SEXOS_GDT_H

#include <stdint.h>

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x18
#define SEL_UDATA 0x20
#define SEL_TSS   0x28

void gdt_init(void);
// Définit la pile noyau utilisée lors d'une bascule ring3 -> ring0.
void tss_set_rsp0(uint64_t rsp0);

#endif
