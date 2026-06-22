// =============================================================================
//  kernel/heap.h -- Tas noyau (kmalloc / kfree)
// =============================================================================
#ifndef SEXOS_HEAP_H
#define SEXOS_HEAP_H

#include <stddef.h>
#include <stdint.h>

void  heap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);

// Buffer physiquement contigu (pour le DMA) : renvoie l'adresse virtuelle HHDM
// et écrit l'adresse physique dans *phys_out.
void *dma_alloc(size_t size, uint64_t *phys_out);

#endif
