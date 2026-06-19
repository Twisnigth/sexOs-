// =============================================================================
//  kernel/elf.c -- Chargement de binaires ELF64 statiques en espace utilisateur
// =============================================================================
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "boot.h"
#include "klib.h"

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed)) Elf64_Phdr;

#define PT_LOAD 1
#define PF_W    2

uint64_t elf_load(uint64_t pml4, const uint8_t *data, size_t len,
                  uint64_t *brk_end, uint64_t *pages_out) {
    uint64_t npages = 0;
    if (len < sizeof(Elf64_Ehdr)) return 0;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    if (eh->e_ident[0]!=0x7F || eh->e_ident[1]!='E' || eh->e_ident[2]!='L' || eh->e_ident[3]!='F')
        return 0;
    if (eh->e_ident[4] != 2) return 0;          // ELFCLASS64
    if (eh->e_machine != 0x3E) return 0;        // x86-64

    uint64_t max_end = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff + (size_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t flags = PTE_USER;
        if (ph->p_flags & PF_W) flags |= PTE_WRITE;

        uint64_t va_start = ph->p_vaddr & ~0xFFFULL;
        uint64_t va_end   = ph->p_vaddr + ph->p_memsz;
        for (uint64_t va = va_start; va < va_end; va += 4096) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) return 0;
            uint8_t *page = (uint8_t *)phys_to_virt(phys);   // zéro déjà fait par le PMM
            // Copie les octets du fichier qui tombent dans cette page.
            for (int b = 0; b < 4096; b++) {
                uint64_t gaddr = va + b;
                if (gaddr >= ph->p_vaddr && gaddr < ph->p_vaddr + ph->p_filesz)
                    page[b] = data[ph->p_offset + (gaddr - ph->p_vaddr)];
            }
            vmm_map_page_in(pml4, va, phys, flags);
            npages++;
        }
        if (va_end > max_end) max_end = va_end;
    }
    if (brk_end) *brk_end = (max_end + 0xFFF) & ~0xFFFULL;
    if (pages_out) *pages_out = npages;
    return eh->e_entry;
}
