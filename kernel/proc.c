// =============================================================================
//  kernel/proc.c -- Appels système (ABI Linux x86-64) et exécution ring 3
// -----------------------------------------------------------------------------
//  Modèle : un processus à la fois, espace d'adressage isolé (PML4 par
//  processus partageant la moitié haute du noyau). L'entrée des appels système
//  se fait via l'instruction syscall (cf. usermode.asm).
// =============================================================================
#include "proc.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "boot.h"
#include "klib.h"
#include "serial.h"
#include "pit.h"
#include "sched.h"

// Registres transmis par syscall_entry (ordre identique à l'empilement asm).
typedef struct {
    uint64_t rax, rdi, rsi, rdx, r10, r8, r9;
} sysargs_t;

extern void enter_user(uint64_t entry, uint64_t user_stack);
extern void user_exit(void);
extern void syscall_entry(void);
extern uint64_t kernel_rsp;

// --- MSR ---------------------------------------------------------------------
#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_FMASK  0xC0000084
#define MSR_FSBASE 0xC0000100

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi; __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static uint8_t syscall_stack[32768] __attribute__((aligned(16)));

// --- État du processus courant ----------------------------------------------
static uint64_t cur_brk;
static uint64_t mmap_base = 0x100000000000ULL;   // zone pour les mmap anonymes
static uint64_t cur_pml4;
static int exit_code;

// Sortie standard (par défaut : port série ; redirigeable vers le terminal).
static void (*out_fn)(const char *, int);
void proc_set_output(void (*fn)(const char *, int)) { out_fn = fn; }
static void user_write(const char *s, int n) {
    if (out_fn) out_fn(s, n);
    else for (int i = 0; i < n; i++) serial_putc(s[i]);
}

void syscall_init(void) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);                       // SCE
    wrmsr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x700);                                     // IF/DF/TF effacés à l'entrée
    kernel_rsp = (uint64_t)(syscall_stack + sizeof(syscall_stack));
    kprintf("[proc] appels systeme (syscall/sysret) actives\n");
}

// --- Dispatcher d'appels système (sous-ensemble de l'ABI Linux x86-64) -------
#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_fstat 5
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_brk 12
#define SYS_getpid 39
#define SYS_getuid 102
#define SYS_getgid 104
#define SYS_geteuid 107
#define SYS_getegid 108
#define SYS_ioctl 16
#define SYS_writev 20
#define SYS_arch_prctl 158
#define SYS_set_tid_address 218
#define SYS_clock_gettime 228
#define SYS_uname 63
#define SYS_exit 60
#define SYS_exit_group 231
#define SYS_openat 257
#define SYS_set_robust_list 273

long syscall_dispatch(sysargs_t *a) {
    switch (a->rax) {
    case SYS_write:
        if (a->rdi == 1 || a->rdi == 2) { user_write((const char *)a->rsi, (int)a->rdx); return a->rdx; }
        return a->rdx;
    case SYS_writev: {
        // iovec[] = {base, len} ; on écrit chaque segment.
        struct { uint64_t base, len; } *iov = (void *)a->rsi;
        long total = 0;
        for (uint64_t i = 0; i < a->rdx; i++) {
            if (a->rdi == 1 || a->rdi == 2) user_write((const char *)iov[i].base, (int)iov[i].len);
            total += iov[i].len;
        }
        return total;
    }
    case SYS_read: return 0;                          // stdin vide (EOF)
    case SYS_brk:
        if (a->rdi == 0) return cur_brk;
        if (a->rdi > cur_brk) {
            for (uint64_t va = (cur_brk + 0xFFF) & ~0xFFFULL; va < a->rdi; va += 4096) {
                uint64_t p = pmm_alloc_page(); if (!p) break;
                vmm_map_page_in(cur_pml4, va, p, PTE_USER | PTE_WRITE);
            }
            cur_brk = a->rdi;
        }
        return cur_brk;
    case SYS_mmap: {
        uint64_t len = (a->rdx /*prot? non*/, a->rsi);   // rsi = longueur
        len = (len + 0xFFF) & ~0xFFFULL;
        uint64_t base = mmap_base; mmap_base += len + 0x1000;
        for (uint64_t off = 0; off < len; off += 4096) {
            uint64_t p = pmm_alloc_page(); if (!p) return -12;   // -ENOMEM
            vmm_map_page_in(cur_pml4, base + off, p, PTE_USER | PTE_WRITE);
        }
        return base;
    }
    case SYS_arch_prctl:
        if (a->rdi == 0x1002) { wrmsr(MSR_FSBASE, a->rsi); return 0; }   // ARCH_SET_FS
        return -22;                                                       // -EINVAL
    case SYS_munmap: return 0;                        // libéré paresseusement
    case SYS_set_tid_address: return 1;
    case SYS_set_robust_list: return 0;
    case SYS_getpid: return 1;
    case SYS_getuid: case SYS_geteuid: case SYS_getgid: case SYS_getegid: return 0;
    case SYS_ioctl: return -25;                       // -ENOTTY (stdout non-tty)
    case SYS_close: return 0;
    case SYS_clock_gettime: {
        uint64_t ms = pit_ms();
        uint64_t *ts = (uint64_t *)a->rsi;             // {tv_sec, tv_nsec}
        if (ts) { ts[0] = ms / 1000; ts[1] = (ms % 1000) * 1000000ULL; }
        return 0;
    }
    case SYS_uname: {                                  // struct utsname (6 x 65 octets)
        char *u = (char *)a->rdi;
        if (u) {
            memset(u, 0, 6 * 65);
            strcpy(u + 0*65, "MonOS");
            strcpy(u + 1*65, "monos");
            strcpy(u + 2*65, "2.0");
            strcpy(u + 3*65, "MonOS v2 x86_64");
            strcpy(u + 4*65, "x86_64");
        }
        return 0;
    }
    case SYS_fstat: case SYS_openat: return -2;        // -ENOENT (FS non exposé)
    case SYS_exit: case SYS_exit_group:
        if (sched_active()) sched_task_exit((int)a->rdi);  // tâche ordonnancée
        exit_code = (int)a->rdi;
        user_exit();                                    // ne revient pas (legacy)
        return 0;
    default:
        kprintf("[sys] non gere : num=%u\n", a->rax);
        return -38;                                     // -ENOSYS
    }
}

// --- Mise en place de la pile initiale (argc/argv/envp/auxv) ------------------
#define USER_STACK_TOP 0x7000000000ULL
#define USER_STACK_SZ  (64 * 1024)

#define AT_NULL 0
#define AT_PAGESZ 6
#define AT_RANDOM 25

static uint64_t setup_stack(int argc, const char **argv) {
    uint64_t top = USER_STACK_TOP;
    uint64_t argp[32];
    // Chaînes argv en haut de pile.
    for (int i = 0; i < argc && i < 32; i++) {
        int l = 0; while (argv[i][l]) l++; l++;
        top -= l; memcpy((void *)top, argv[i], l); argp[i] = top;
    }
    // 16 octets aléatoires pour AT_RANDOM (canari de pile glibc/musl).
    top -= 16; csprng_bytes((void *)top, 16); uint64_t rnd = top;
    top &= ~0xFULL;

    // Construction du vecteur initial.
    uint64_t vec[64]; int n = 0;
    vec[n++] = argc;
    for (int i = 0; i < argc; i++) vec[n++] = argp[i];
    vec[n++] = 0;                       // fin argv
    vec[n++] = 0;                       // envp vide
    vec[n++] = AT_PAGESZ; vec[n++] = 4096;
    vec[n++] = AT_RANDOM; vec[n++] = rnd;
    vec[n++] = AT_NULL;   vec[n++] = 0;
    if (n & 1) vec[n++] = 0;            // garde l'alignement 16

    uint64_t sp = (top - (uint64_t)n * 8) & ~0xFULL;
    memcpy((void *)sp, vec, n * 8);
    return sp;
}

int proc_run(const void *elf, size_t len, int argc, const char **argv) {
    uint64_t pml4 = vmm_new_address_space();
    if (!pml4) return -1;
    uint64_t brk_end = 0;
    uint64_t entry = elf_load(pml4, (const uint8_t *)elf, len, &brk_end);
    if (!entry) { kprintf("[proc] ELF invalide\n"); return -1; }

    // Pile utilisateur.
    for (uint64_t off = 0; off < USER_STACK_SZ; off += 4096) {
        uint64_t p = pmm_alloc_page();
        vmm_map_page_in(pml4, USER_STACK_TOP - USER_STACK_SZ + off, p, PTE_USER | PTE_WRITE);
    }

    cur_brk = brk_end;
    cur_pml4 = pml4;
    mmap_base = 0x100000000000ULL;
    exit_code = 0;

    uint64_t kernel_cr3 = vmm_current_cr3();
    vmm_switch(pml4);                          // bascule dans l'espace du processus
    uint64_t sp = setup_stack(argc, argv);
    enter_user(entry, sp);                     // -> ring 3 ; revient via exit
    vmm_switch(kernel_cr3);                     // retour à l'espace noyau
    // Restaure les segments noyau (l'ABI utilisateur les a laissés en 0x23,
    // et arch_prctl a modifié la base FS) : sinon le noyau hérite d'un état
    // utilisateur qui casse les routines suivantes.
    __asm__ volatile (
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t mov %%ax, %%es\n\t mov %%ax, %%fs\n\t mov %%ax, %%gs"
        ::: "ax");
    wrmsr(MSR_FSBASE, 0);
    // L'appel système exit a laissé IF=0 (FMASK) ; on réactive les interruptions.
    __asm__ volatile ("sti");
    return exit_code;
}
