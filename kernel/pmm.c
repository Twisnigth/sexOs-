// =============================================================================
//  kernel/pmm.c -- Allocateur de pages physiques par bitmap
// -----------------------------------------------------------------------------
//  On parcourt la carte mémoire fournie par Limine, on place un bitmap (1 bit
//  par page de 4 Kio) dans la première zone utilisable assez grande, puis on
//  marque libres uniquement les pages réellement utilisables. Les pages sont
//  accédées via la fenêtre HHDM (physique + offset).
// =============================================================================
#include "pmm.h"
#include "boot.h"
#include "klib.h"

static uint8_t *bitmap;         // 1 = utilisée, 0 = libre
static uint64_t total_pages;
static uint64_t used_pages;
static uint64_t highest_addr;

static inline void bm_set(uint64_t page)   { bitmap[page / 8] |=  (1 << (page % 8)); }
static inline void bm_clear(uint64_t page) { bitmap[page / 8] &= ~(1 << (page % 8)); }
static inline int  bm_test(uint64_t page)  { return bitmap[page / 8] & (1 << (page % 8)); }

void pmm_init(void) {
    struct limine_memmap_response *mm = boot_memmap();
    if (!mm) { kprintf("[pmm] pas de carte memoire !\n"); return; }

    // 1) Trouver l'adresse physique utilisable la plus haute (on ignore les
    //    zones réservées hautes, qui gonfleraient inutilement le bitmap).
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE &&
            e->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) continue;
        uint64_t top = e->base + e->length;
        if (top > highest_addr) highest_addr = top;
    }
    total_pages = highest_addr / PAGE_SIZE;
    uint64_t bitmap_size = (total_pages + 7) / 8;

    // 2) Placer le bitmap dans la première zone utilisable assez grande.
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_size) {
            bitmap = (uint8_t *)phys_to_virt(e->base);
            // On marque tout occupé par défaut...
            memset(bitmap, 0xFF, bitmap_size);
            // ... puis on libère les zones réellement utilisables.
            for (uint64_t j = 0; j < mm->entry_count; j++) {
                struct limine_memmap_entry *u = mm->entries[j];
                if (u->type != LIMINE_MEMMAP_USABLE) continue;
                for (uint64_t a = u->base; a < u->base + u->length; a += PAGE_SIZE)
                    bm_clear(a / PAGE_SIZE);
            }
            // Réserver les pages occupées par le bitmap lui-même.
            for (uint64_t a = e->base; a < e->base + bitmap_size; a += PAGE_SIZE)
                bm_set(a / PAGE_SIZE);
            break;
        }
    }

    // 3) Compter les pages utilisées.
    used_pages = 0;
    for (uint64_t p = 0; p < total_pages; p++) if (bm_test(p)) used_pages++;

    kprintf("[pmm] %u Mio total, %u Mio utilisables\n",
            (uint32_t)(pmm_total_bytes() / (1024 * 1024)),
            (uint32_t)((total_pages - used_pages) * PAGE_SIZE / (1024 * 1024)));
}

uint64_t pmm_alloc_page(void) {
    for (uint64_t p = 1; p < total_pages; p++) {   // on évite la page 0
        if (!bm_test(p)) {
            bm_set(p);
            used_pages++;
            uint64_t phys = p * PAGE_SIZE;
            memset(phys_to_virt(phys), 0, PAGE_SIZE);   // page propre
            return phys;
        }
    }
    return 0;   // plus de mémoire
}

uint64_t pmm_alloc_contiguous(size_t pages) {
    if (pages == 0) return 0;
    uint64_t run = 0, start = 0;
    for (uint64_t p = 1; p < total_pages; p++) {
        if (!bm_test(p)) {
            if (run == 0) start = p;
            if (++run == pages) {
                for (uint64_t q = start; q < start + pages; q++) {
                    bm_set(q);
                    used_pages++;
                }
                memset(phys_to_virt(start * PAGE_SIZE), 0, pages * PAGE_SIZE);
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

void pmm_free_page(uint64_t phys) {
    uint64_t p = phys / PAGE_SIZE;
    if (p < total_pages && bm_test(p)) {
        bm_clear(p);
        used_pages--;
    }
}

uint64_t pmm_total_bytes(void) { return total_pages * PAGE_SIZE; }
uint64_t pmm_used_bytes(void)  { return used_pages * PAGE_SIZE; }
