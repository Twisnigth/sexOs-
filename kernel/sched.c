// =============================================================================
//  kernel/sched.c -- Ordonnanceur multi-processus ring 3 (Phase 1)
// =============================================================================
#include "sched.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "gdt.h"
#include "boot.h"
#include "klib.h"
#include "serial.h"

// Primitives assembleur (switch.asm).
extern void sched_resume(registers_t *ctx);        // reprend une tâche, sans retour
extern void sched_save_and_run(registers_t *first); // sauve le noyau, lance la 1re
extern void sched_return(void);                     // revient au noyau (mode démo)

// --- MSR FS base (TLS par tâche) ---------------------------------------------
#define MSR_FSBASE 0xC0000100
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

// Sélecteurs ring 3 (cf. usermode.asm).
#define USER_CS 0x1b
#define USER_SS 0x23
#define USER_RFLAGS 0x202

#define KSTACK_SIZE     16384
#define USER_LOAD_ADDR  0x400000ULL
#define USER_STACK_TOP  0x7000000000ULL
#define USER_STACK_SZ   (64 * 1024)

static task_t tasks[SCHED_MAX_TASKS];
static task_t *current;
static int  next_pid = 1;
static int  rr_last;                 // dernier indice servi (round-robin)
static int  active;                  // ordonnanceur en cours d'exécution
static uint64_t kernel_cr3;

int     sched_active(void)  { return active; }
task_t *sched_current(void) { return current; }

// --- Création d'une tâche ----------------------------------------------------
static task_t *alloc_slot(void) {
    for (int i = 0; i < SCHED_MAX_TASKS; i++)
        if (tasks[i].state == TASK_UNUSED) return &tasks[i];
    return NULL;
}

int sched_new_flat_task(const char *name, const uint8_t *code, size_t len) {
    task_t *t = alloc_slot();
    if (!t) return -1;

    uint64_t pml4 = vmm_new_address_space();
    if (!pml4) return -1;

    // Charge le binaire « plat » à USER_LOAD_ADDR (code + données, inscriptible).
    for (uint64_t off = 0; off < len; off += 4096) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return -1;
        uint8_t *dst = (uint8_t *)phys_to_virt(phys);
        size_t n = (len - off < 4096) ? (len - off) : 4096;
        memset(dst, 0, 4096);
        memcpy(dst, code + off, n);
        vmm_map_page_in(pml4, USER_LOAD_ADDR + off, phys, PTE_USER | PTE_WRITE);
    }

    // Pile utilisateur.
    for (uint64_t off = 0; off < USER_STACK_SZ; off += 4096) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return -1;
        vmm_map_page_in(pml4, USER_STACK_TOP - USER_STACK_SZ + off, phys,
                        PTE_USER | PTE_WRITE);
    }

    // Pile noyau (sert de rsp0 et porte le contexte sauvegardé).
    uint8_t *kstack = (uint8_t *)kmalloc(KSTACK_SIZE);
    if (!kstack) return -1;
    uint64_t ktop = ((uint64_t)kstack + KSTACK_SIZE) & ~0xFULL;

    // Contexte initial : un registers_t qui, restauré + iretq, entre en ring 3.
    registers_t *f = (registers_t *)(ktop - sizeof(registers_t));
    memset(f, 0, sizeof(*f));
    f->rip    = USER_LOAD_ADDR;
    f->cs     = USER_CS;
    f->rflags = USER_RFLAGS;
    f->rsp    = USER_STACK_TOP - 16;
    f->ss     = USER_SS;

    t->pid        = next_pid++;
    t->state      = TASK_READY;
    t->pml4       = pml4;
    t->kstack     = (uint64_t)kstack;
    t->kstack_top = ktop;
    t->ctx        = (uint64_t)f;
    t->fs_base    = 0;
    t->brk        = 0;
    t->mmap_base  = 0x100000000000ULL;
    t->exit_code  = 0;
    t->name       = name;
    return t->pid;
}

// --- Sélection round-robin ---------------------------------------------------
static task_t *pick_next(void) {
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        int idx = (rr_last + 1 + i) % SCHED_MAX_TASKS;
        if (tasks[idx].state == TASK_READY) { rr_last = idx; return &tasks[idx]; }
    }
    return NULL;
}

// Installe l'espace d'adressage / la pile noyau / la base FS de la tâche.
static void install(task_t *t) {
    current = t;
    t->state = TASK_RUNNING;
    vmm_switch(t->pml4);
    tss_set_rsp0(t->kstack_top);
    wrmsr(MSR_FSBASE, t->fs_base);
}

// Repli vers le noyau quand plus aucune tâche n'est prête (mode démo).
static void back_to_kernel(void) {
    active = 0;
    current = NULL;
    vmm_switch(kernel_cr3);
    __asm__ volatile ("mov $0x10, %%ax\n\t mov %%ax, %%ds\n\t mov %%ax, %%es\n\t"
                      "mov %%ax, %%fs\n\t mov %%ax, %%gs" ::: "ax");
    wrmsr(MSR_FSBASE, 0);
    sched_return();                 // ne revient pas ici
}

// --- Points d'entrée depuis le répartiteur d'interruptions -------------------
registers_t *sched_on_timer(registers_t *r) {
    if (!active || !current) return r;
    current->ctx = (uint64_t)r;                 // sauvegarde le point de préemption
    if (current->state == TASK_RUNNING) current->state = TASK_READY;
    task_t *n = pick_next();
    if (!n) { back_to_kernel(); }               // (ne revient pas si vraiment vide)
    install(n);
    return (registers_t *)n->ctx;
}

registers_t *sched_on_fault(registers_t *r) {
    // La tâche courante a fauté en ring 3 : on la tue, le noyau survit.
    kprintf("[sched] pid=%d (%s) tue : faute ring3 (cs=%x rip=%p)\n",
            current ? current->pid : -1, current ? current->name : "?",
            (unsigned)r->cs, (void *)r->rip);
    if (current) current->state = TASK_ZOMBIE;
    task_t *n = pick_next();
    if (!n) back_to_kernel();                   // ne revient pas
    install(n);
    return (registers_t *)n->ctx;
}

void sched_task_exit(int code) {
    if (current) { current->exit_code = code; current->state = TASK_ZOMBIE; }
    kprintf("[sched] pid=%d (%s) termine, code=%d\n",
            current ? current->pid : -1, current ? current->name : "?", code);
    task_t *n = pick_next();
    if (!n) back_to_kernel();                   // ne revient pas
    install(n);
    sched_resume((registers_t *)n->ctx);        // ne revient pas
}

// --- Boucle de démarrage (mode démo Phase 1) ---------------------------------
void sched_run_until_idle(void) {
    kernel_cr3 = vmm_current_cr3();
    rr_last = SCHED_MAX_TASKS - 1;
    task_t *first = pick_next();
    if (!first) return;                         // rien à exécuter
    active = 1;
    install(first);
    sched_save_and_run((registers_t *)first->ctx);  // revient via back_to_kernel
    // On est revenu en abandonnant un contexte d'interruption/syscall : IF=0.
    __asm__ volatile ("sti");
}
