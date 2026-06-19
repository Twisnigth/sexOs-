// =============================================================================
//  kernel/sched.h -- Ordonnanceur multi-processus (ring 3) — Phase 1
// -----------------------------------------------------------------------------
//  Modèle : plusieurs tâches ring 3, chacune avec son espace d'adressage (PML4),
//  sa pile noyau (rsp0 du TSS), et son contexte de registres complet. Préemption
//  par l'IRQ du minuteur (PIT) ; commutation aussi à la sortie d'un processus
//  (exit) et lorsqu'une tâche faute (kill-on-fault).
// =============================================================================
#ifndef MONOS_SCHED_H
#define MONOS_SCHED_H

#include <stdint.h>
#include <stddef.h>
#include "idt.h"

#define SCHED_MAX_TASKS 16

// --- Messagerie inter-processus (IPC) ----------------------------------------
#define IPC_MSG_MAX  128       // octets utiles par message
#define IPC_MBOX_LEN 32        // messages en attente par tâche
typedef struct { int sender; int len; uint8_t data[IPC_MSG_MAX]; } ipc_msg_t;

typedef enum { TASK_UNUSED = 0, TASK_READY, TASK_RUNNING, TASK_ZOMBIE } task_state_t;

typedef struct task {
    int          pid;
    task_state_t state;
    uint64_t     pml4;          // espace d'adressage (adresse physique du PML4)
    uint64_t     kstack;        // base de la pile noyau (à libérer)
    uint64_t     kstack_top;    // sommet de la pile noyau (rsp0 du TSS)
    uint64_t     ctx;           // pile noyau sauvegardée -> pointe sur un registers_t
    uint64_t     fs_base;       // base FS par tâche (TLS)
    uint64_t     brk;
    uint64_t     mmap_base;
    uint64_t     shm_next;      // prochaine VA libre pour mapper de la mémoire partagée
    ipc_msg_t    mbox[IPC_MBOX_LEN];
    int          mbox_head, mbox_tail;
    int          exit_code;
    const char  *name;
} task_t;

// IPC : recherche d'une tâche par pid (pour la livraison de messages).
task_t *sched_task_by_pid(int pid);

// Crée une tâche ring 3 à partir d'un binaire « plat » chargé à 0x400000.
int  sched_new_flat_task(const char *name, const uint8_t *code, size_t len);
// Crée une tâche ring 3 à partir d'un exécutable ELF64 statique.
int  sched_new_elf_task(const char *name, const uint8_t *elf, size_t len);

// Lance l'ordonnanceur et exécute les tâches prêtes ; revient à l'appelant
// quand il n'y a plus AUCUNE tâche prête (mode démo de la Phase 1).
void sched_run_until_idle(void);

// Démarre l'ordonnanceur pour de bon : ne revient JAMAIS. S'il n'y a pas de
// tâche prête, exécute une tâche idle noyau (hlt). C'est le modèle final :
// kmain crée la tâche init (le bureau) puis appelle ceci.
void sched_start(void);

// Appelés depuis le répartiteur d'interruptions (idt.c).
registers_t *sched_on_timer(registers_t *r);     // préemption PIT
registers_t *sched_on_fault(registers_t *r);     // faute ring 3 -> tue la tâche
int          sched_active(void);

// Sortie volontaire (appelée par le syscall exit). Ne revient jamais.
void sched_task_exit(int code);

// Identité de la tâche courante (pour les syscalls).
task_t *sched_current(void);

#endif
