// =============================================================================
//  kernel/proc.h -- Ring 3, appels système, exécution de binaires
// =============================================================================
#ifndef SEXOS_PROC_H
#define SEXOS_PROC_H

#include <stdint.h>
#include <stddef.h>

void syscall_init(void);                 // configure syscall/sysret (MSR)
// Exécute un binaire ELF statique en ring 3. Renvoie le code de sortie.
int  proc_run(const void *elf, size_t len, int argc, const char **argv);
// Définit où va la sortie standard des processus (par défaut : port série).
void proc_set_output(void (*fn)(const char *, int));

#endif
