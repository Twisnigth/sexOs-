// =============================================================================
//  kernel/vmm.h -- Mémoire virtuelle : mappage de pages (MMIO notamment)
// =============================================================================
#ifndef MONOS_VMM_H
#define MONOS_VMM_H

#include <stdint.h>
#include <stddef.h>

#define PTE_PRESENT 0x001
#define PTE_WRITE   0x002
#define PTE_USER    0x004
#define PTE_PWT     0x008
#define PTE_PCD     0x010

// Mappe une page virtuelle -> physique dans l'espace courant.
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

// Mappe une région MMIO physique dans la fenêtre HHDM (cache désactivé).
void vmm_map_mmio(uint64_t phys, size_t size);

#endif
