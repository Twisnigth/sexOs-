// =============================================================================
//  kernel/vmm.c -- Mappage de pages dans les tables fournies par Limine
// -----------------------------------------------------------------------------
//  On marche les 4 niveaux de pagination (PML4 -> PDPT -> PD -> PT) de l'espace
//  d'adressage courant (CR3), en créant les tables intermédiaires manquantes.
//  Sert surtout à mapper les BAR MMIO des périphériques (e1000...) que le HHDM
//  de Limine ne couvre pas.
// =============================================================================
#include "vmm.h"
#include "pmm.h"
#include "boot.h"
#include "klib.h"

#define ADDR_MASK 0x000FFFFFFFFFF000ULL

static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void invlpg(uint64_t v) {
    __asm__ volatile ("invlpg (%0)" : : "r"(v) : "memory");
}

// Renvoie un pointeur (via HHDM) vers la table de niveau suivant, en la créant
// si nécessaire (les tables intermédiaires sont marquées USER pour autoriser
// l'accès ring 3 ; la protection réelle est portée par l'entrée feuille).
static uint64_t *next_table(uint64_t *table, int idx, bool create) {
    if (!(table[idx] & PTE_PRESENT)) {
        if (!create) return NULL;
        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;
        memset(phys_to_virt(phys), 0, 4096);
        table[idx] = phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    return (uint64_t *)phys_to_virt(table[idx] & ADDR_MASK);
}

static void map_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pdpt = next_table(pml4, (virt >> 39) & 0x1FF, true); if (!pdpt) return;
    uint64_t *pd   = next_table(pdpt, (virt >> 30) & 0x1FF, true); if (!pd) return;
    uint64_t *pt   = next_table(pd,   (virt >> 21) & 0x1FF, true); if (!pt) return;
    pt[(virt >> 12) & 0x1FF] = (phys & ADDR_MASK) | flags | PTE_PRESENT;
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    map_in((uint64_t *)phys_to_virt(read_cr3() & ADDR_MASK), virt, phys, flags);
    invlpg(virt);
}

void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    map_in((uint64_t *)phys_to_virt(pml4_phys & ADDR_MASK), virt, phys, flags);
}

uint64_t vmm_new_address_space(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return 0;
    uint64_t *np = (uint64_t *)phys_to_virt(phys);
    memset(np, 0, 4096);
    uint64_t *cur = (uint64_t *)phys_to_virt(read_cr3() & ADDR_MASK);
    for (int i = 256; i < 512; i++) np[i] = cur[i];   // partage de la moitié haute (noyau)
    return phys;
}

uint64_t vmm_current_cr3(void) { return read_cr3(); }
void vmm_switch(uint64_t pml4_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

void vmm_map_mmio(uint64_t phys, size_t size) {
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = (phys + size + 0xFFF) & ~0xFFFULL;
    for (uint64_t p = start; p < end; p += 4096)
        vmm_map_page((uint64_t)phys_to_virt(p), p, PTE_WRITE | PTE_PCD | PTE_PWT);
}
