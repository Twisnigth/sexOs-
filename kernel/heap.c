// =============================================================================
//  kernel/heap.c -- Tas noyau : allocateur à liste libre avec fusion
// -----------------------------------------------------------------------------
//  Le tas est constitué d'une ou plusieurs "arènes" : des blocs physiquement
//  contigus obtenus du PMM et accédés via la fenêtre HHDM. Chaque arène est
//  découpée en blocs chaînés par ordre d'adresse ; on fusionne les blocs libres
//  adjacents au sein d'une même arène.
// =============================================================================
#include "heap.h"
#include "pmm.h"
#include "boot.h"
#include "klib.h"

#include <stdint.h>

#define HEAP_MAGIC   0x4D4F4E4F          // "MONO"
#define ARENA_PAGES  4096                // 16 Mio par arène
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

typedef struct block {
    uint32_t magic;
    uint32_t free;
    uint64_t size;          // taille de la charge utile
    struct block *next;     // bloc suivant par ordre d'adresse
} block_t;

static block_t *head;       // premier bloc de la liste

// Ajoute une nouvelle arène au tas. Renvoie son premier bloc (libre).
static block_t *add_arena(size_t min_payload) {
    size_t need = min_payload + sizeof(block_t);
    size_t pages = (need + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < ARENA_PAGES) pages = ARENA_PAGES;

    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) return NULL;

    block_t *b = (block_t *)phys_to_virt(phys);
    b->magic = HEAP_MAGIC;
    b->free  = 1;
    b->size  = pages * PAGE_SIZE - sizeof(block_t);
    b->next  = NULL;

    // Insère en tête de liste (l'ordre global importe peu, la fusion se fait
    // par adjacence physique au sein de l'arène).
    b->next = head;
    head = b;
    return b;
}

void heap_init(void) {
    head = NULL;
    if (!add_arena(0)) {
        kprintf("[heap] impossible d'allouer l'arene initiale !\n");
        return;
    }
    kprintf("[heap] pret (%u Mio par arene)\n",
            (uint32_t)(ARENA_PAGES * PAGE_SIZE / (1024 * 1024)));
}

// Découpe un bloc s'il est nettement plus grand que demandé.
static void split_block(block_t *b, size_t size) {
    if (b->size >= size + sizeof(block_t) + 32) {
        block_t *n = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
        n->magic = HEAP_MAGIC;
        n->free  = 1;
        n->size  = b->size - size - sizeof(block_t);
        n->next  = b->next;
        b->next  = n;
        b->size  = size;
    }
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN_UP(size, 16);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (block_t *b = head; b; b = b->next) {
            if (b->free && b->size >= size) {
                split_block(b, size);
                b->free = 0;
                return (uint8_t *)b + sizeof(block_t);
            }
        }
        // Aucun bloc ne convient : on ajoute une arène et on réessaie.
        if (!add_arena(size)) break;
    }
    kprintf("[heap] kmalloc(%u) a echoue\n", (uint32_t)size);
    return NULL;
}

void *kcalloc(size_t n, size_t size) {
    size_t total = n * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

// Fusionne les blocs libres physiquement adjacents (au sein d'une arène).
static void coalesce(void) {
    for (block_t *b = head; b; b = b->next) {
        while (b->free && b->next && b->next->free &&
               (uint8_t *)b + sizeof(block_t) + b->size == (uint8_t *)b->next) {
            b->size += sizeof(block_t) + b->next->size;
            b->next = b->next->next;
        }
    }
}

void kfree(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    if (b->magic != HEAP_MAGIC) {
        kprintf("[heap] kfree d'un pointeur invalide %p\n", ptr);
        return;
    }
    b->free = 1;
    coalesce();
}

void *krealloc(void *ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }
    block_t *b = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    if (b->size >= size) return ptr;       // déjà assez grand
    void *n = kmalloc(size);
    if (!n) return NULL;
    memcpy(n, ptr, b->size);
    kfree(ptr);
    return n;
}

void *dma_alloc(size_t size, uint64_t *phys_out) {
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) { if (phys_out) *phys_out = 0; return NULL; }
    if (phys_out) *phys_out = phys;
    return phys_to_virt(phys);
}
