// =============================================================================
//  kernel/elf.h -- Chargeur ELF64 (binaires statiques)
// =============================================================================
#ifndef MONOS_ELF_H
#define MONOS_ELF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Charge les segments PT_LOAD du binaire dans l'espace d'adressage 'pml4'.
// Renvoie le point d'entrée (0 en cas d'erreur) et la fin des segments (brk).
// 'pages_out' (peut être NULL) reçoit le nombre de pages physiques mappées.
uint64_t elf_load(uint64_t pml4, const uint8_t *data, size_t len,
                  uint64_t *brk_end, uint64_t *pages_out);

#endif
