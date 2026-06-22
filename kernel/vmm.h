// =============================================================================
//  kernel/vmm.h -- Mémoire virtuelle : mappage de pages (MMIO notamment)
// =============================================================================
#ifndef SEXOS_VMM_H
#define SEXOS_VMM_H

#include <stdint.h>
#include <stddef.h>

#define PTE_PRESENT 0x001
#define PTE_WRITE   0x002
#define PTE_USER    0x004
#define PTE_PWT     0x008
#define PTE_PCD     0x010
#define PTE_PAT     0x080      // bit PAT (page 4 Kio) : sélectionne l'entrée PAT

// Combinaison sélectionnant l'entrée PAT n°7 (programmée en Write-Combining par
// vmm_pat_init) : écritures rapides vers le framebuffer (au lieu de write-through).
#define PTE_WC      (PTE_PAT | PTE_PCD | PTE_PWT)

// Mappe une page virtuelle -> physique dans l'espace courant.
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

// Mappe une page dans un espace d'adressage donné (PML4 physique).
void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

// Mappe une région MMIO physique dans la fenêtre HHDM (cache désactivé).
void vmm_map_mmio(uint64_t phys, size_t size);

// Crée un nouvel espace d'adressage (PML4) partageant la moitié haute (noyau).
uint64_t vmm_new_address_space(void);
uint64_t vmm_current_cr3(void);
void     vmm_switch(uint64_t pml4_phys);

// Programme l'entrée PAT n°7 en Write-Combining (pour le framebuffer).
void     vmm_pat_init(void);

#endif
