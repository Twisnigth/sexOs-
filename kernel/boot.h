// =============================================================================
//  kernel/boot.h -- Accès aux informations fournies par Limine
// =============================================================================
#ifndef MONOS_BOOT_H
#define MONOS_BOOT_H

#include <stdint.h>
#include "limine.h"

uint64_t                        boot_hhdm_offset(void);   // décalage de la fenêtre HHDM
struct limine_memmap_response  *boot_memmap(void);        // carte mémoire
struct limine_framebuffer      *boot_framebuffer(void);   // 1er framebuffer
void                           *boot_rsdp(void);          // pointeur ACPI RSDP

// Conversion adresse physique <-> virtuelle via la fenêtre HHDM.
static inline void *phys_to_virt(uint64_t phys) {
    extern uint64_t boot_hhdm_offset(void);
    return (void *)(phys + boot_hhdm_offset());
}

#endif
