// =============================================================================
//  kernel/pmm.h -- Gestionnaire de mémoire physique (allocateur par bitmap)
// =============================================================================
#ifndef SEXOS_PMM_H
#define SEXOS_PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

void     pmm_init(void);
uint64_t pmm_alloc_page(void);          // renvoie une adresse PHYSIQUE (0 si échec)
uint64_t pmm_alloc_contiguous(size_t pages);  // bloc physiquement contigu (DMA, heap)
void     pmm_free_page(uint64_t phys);
uint64_t pmm_total_bytes(void);
uint64_t pmm_used_bytes(void);

#endif
