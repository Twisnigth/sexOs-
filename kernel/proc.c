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
#include "syscalls.h"
#include "framebuffer.h"
#include "input.h"
#include "rtc.h"
#include "pci.h"
#include "io.h"
#include "vfs.h"
#include "users.h"
#include "net.h"
#include "crypto.h"

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

// --- IPC : table de mémoire partagée + pid du compositeur --------------------
#define SHM_MAX   32
#define SHM_PAGES 512                  // jusqu'à 2 Mio par objet partagé
typedef struct { int used; int npages; uint64_t pages[SHM_PAGES]; } shm_obj_t;
static shm_obj_t shms[SHM_MAX];
static int compositor_pid;

// --- Applications lançables à la demande (SYS_spawn par le menu du compositeur) -
//  Les binaires ELF sont embarqués par user_blobs.asm. L'ordre correspond aux
//  constantes APP_* de syscalls.h.
extern uint8_t uterm_start[],  uterm_end[];
extern uint8_t ufiles_start[], ufiles_end[];
extern uint8_t uclock_start[], uclock_end[];
extern uint8_t umon_start[],   umon_end[];
extern uint8_t uweb_start[],   uweb_end[];
extern uint8_t usettings_start[], usettings_end[];
static const struct { const char *name; uint8_t *start, *end; } g_apps[] = {
    { "terminal",    uterm_start,     uterm_end     },
    { "explorateur", ufiles_start,    ufiles_end    },
    { "horloge",     uclock_start,    uclock_end    },
    { "moniteur",    umon_start,      umon_end      },
    { "navigateur",  uweb_start,      uweb_end      },
    { "parametres",  usettings_start, usettings_end },
};

// Mappe les pages de 'o' dans l'espace courant à une VA libre de la tâche.
static uint64_t shm_map_into(task_t *t, shm_obj_t *o) {
    uint64_t aspace = vmm_current_cr3() & 0x000FFFFFFFFFF000ULL;
    uint64_t va = t->shm_next;
    for (int i = 0; i < o->npages; i++)
        vmm_map_page_in(aspace, va + (uint64_t)i * 4096, o->pages[i], PTE_USER | PTE_WRITE);
    t->shm_next += (uint64_t)(o->npages + 1) * 4096;   // +1 page de garde
    return va;
}

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

long syscall_dispatch(sysargs_t *a);     // défini plus bas

// Point d'entrée unifié des appels système (cf. usermode.asm). Reçoit le
// contexte complet de la tâche et renvoie le contexte à reprendre : la MÊME
// tâche pour un appel normal, une AUTRE pour yield / attente bloquante / exit.
registers_t *syscall_enter(registers_t *r) {
    task_t *me = sched_current();
    switch (r->rax) {
    case SYS_yield:
        if (me) me->state = TASK_READY;
        return sched_switch_from(r);
    case SYS_ipc_wait:
        if (me && me->mbox_head == me->mbox_tail) {  // boîte vide -> on dort
            me->state = TASK_BLOCKED;
            return sched_switch_from(r);
        }
        r->rax = 0;
        return r;
    case SYS_exit: case SYS_exit_group:
        if (sched_active() && me) {
            me->exit_code = (int)r->rdi;
            me->state = TASK_ZOMBIE;
            return sched_switch_from(r);
        }
        exit_code = (int)r->rdi;
        user_exit();                                  // legacy proc_run (ne revient pas)
        return r;
    default: {
        sysargs_t a = { r->rax, r->rdi, r->rsi, r->rdx, r->r10, r->r8, r->r9 };
        r->rax = (uint64_t)syscall_dispatch(&a);
        return r;
    }
    }
}

long syscall_dispatch(sysargs_t *a) {
    // L'appel système s'exécute dans l'espace d'adressage de l'appelant : on
    // mappe toujours dans le PML4 courant (correct pour CHAQUE tâche ordonnancée,
    // contrairement à l'ancien cur_pml4 global mono-processus).
    uint64_t aspace = vmm_current_cr3() & 0x000FFFFFFFFFF000ULL;
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
                vmm_map_page_in(aspace, va, p, PTE_USER | PTE_WRITE);
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
            vmm_map_page_in(aspace, base + off, p, PTE_USER | PTE_WRITE);
        }
        sched_account_pages(len / 4096);
        return base;
    }
    case SYS_arch_prctl:
        if (a->rdi == 0x1002) { wrmsr(MSR_FSBASE, a->rsi); return 0; }   // ARCH_SET_FS
        return -22;                                                       // -EINVAL
    case SYS_munmap: return 0;                        // libéré paresseusement
    case SYS_set_tid_address: return 1;
    case SYS_set_robust_list: return 0;
    case SYS_getpid: { task_t *c = sched_current(); return c ? c->pid : 1; }
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
            strcpy(u + 0*65, "sexOs");
            strcpy(u + 1*65, "sexos");
            strcpy(u + 2*65, "2.0");
            strcpy(u + 3*65, "sexOs v2 x86_64");
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
    // --- Appels système natifs sexOs (>= 0x200) -----------------------------
    case SYS_get_cpl: {
        uint16_t cs; __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
        // NB : ici on est en ring 0 (dans le syscall) ; l'appelant était ring 3.
        (void)cs; return 3;
    }
    case SYS_fb_map: {
        fbinfo_t *fi = (fbinfo_t *)a->rdi;
        uint64_t phys  = fb_phys();
        uint32_t pitch = fb_pitch();
        uint32_t h     = fb_height();
        uint64_t bytes = (uint64_t)pitch * h;
        uint64_t va    = 0xE0000000ULL;            // VA utilisateur du framebuffer
        uint64_t cur   = vmm_current_cr3() & 0x000FFFFFFFFFF000ULL;
        for (uint64_t off = 0; off < bytes; off += 4096)
            vmm_map_page_in(cur, va + off, phys + off, PTE_USER | PTE_WRITE | PTE_PWT);
        if (fi) { fi->addr = va; fi->width = fb_width(); fi->height = h; fi->pitch = pitch; }
        sched_account_pages(bytes / 4096);   // framebuffer mappé chez le compositeur
        return 0;
    }
    case SYS_input_poll: {
        event_t *e = (event_t *)a->rdi;
        return input_poll(e) ? 1 : 0;
    }
    case SYS_time_ms:
        return (long)pit_ms();
    case SYS_rtc_now: {
        rtc_time_t *t = (rtc_time_t *)a->rdi;
        if (t) rtc_now(t);
        return 0;
    }
    case SYS_sysinfo: {
        sysinfo_t *si = (sysinfo_t *)a->rdi;
        if (si) {
            si->mem_total_mb = (uint32_t)(pmm_total_bytes() / (1024 * 1024));
            si->mem_used_mb  = (uint32_t)(pmm_used_bytes() / (1024 * 1024));
            si->uptime_s     = (uint32_t)(pit_ms() / 1000);
            si->pci_count    = (uint32_t)pci_device_count();
        }
        return 0;
    }
    case SYS_reboot:
        outb(0x64, 0xFE);
        return 0;
    // --- Réseau : sockets TCP non bloquantes + DNS (navigateur ring 3) -------
    case SYS_net_info: {
        netinfo_t *ni = (netinfo_t *)a->rdi;
        if (ni) {
            ni->ip = netif.ip; ni->mask = netif.mask;
            ni->gateway = netif.gateway; ni->dns = netif.dns;
            ni->up = netif.up ? 1 : 0;
        }
        return 0;
    }
    case SYS_dns_resolve: {
        char name[128];
        const char *src = (const char *)a->rdi;
        int i = 0; if (src) for (; src[i] && i < 127; i++) name[i] = src[i];
        name[i] = 0;
        ip4_t ip = 0;
        int r = dns_query(name, &ip);
        if (r == 1 && a->rsi) *(uint32_t *)a->rsi = ip;
        return r;
    }
    case SYS_random:     csprng_bytes((void *)a->rdi, (size_t)a->rsi); return 0;
    case SYS_tcp_open:   return tcp_open((ip4_t)a->rdi, (uint16_t)a->rsi);
    case SYS_tcp_state:  return tcp_state((int)a->rdi);
    case SYS_tcp_send:   return tcp_write((int)a->rdi, (const void *)a->rsi, (int)a->rdx);
    case SYS_tcp_recv:   return tcp_read((int)a->rdi, (void *)a->rsi, (int)a->rdx);
    case SYS_tcp_close:  tcp_shutdown((int)a->rdi); return 0;
    // --- IPC -----------------------------------------------------------------
    case SYS_ipc_send: {
        task_t *dst = sched_task_by_pid((int)a->rdi);
        task_t *me  = sched_current();
        if (!dst || dst->state == TASK_ZOMBIE) return -1;
        int len = (int)a->rdx; if (len > IPC_MSG_MAX) len = IPC_MSG_MAX;
        int nxt = (dst->mbox_tail + 1) % IPC_MBOX_LEN;
        if (nxt == dst->mbox_head) return -1;            // boîte pleine
        ipc_msg_t *m = &dst->mbox[dst->mbox_tail];
        m->sender = me ? me->pid : 0; m->len = len;
        memcpy(m->data, (const void *)a->rsi, len);
        dst->mbox_tail = nxt;
        sched_wake(dst);                 // réveille la tâche si elle dormait
        return 0;
    }
    case SYS_ipc_recv: {
        task_t *me = sched_current();
        if (!me || me->mbox_head == me->mbox_tail) return -1;   // vide (non bloquant)
        ipc_msg_t *m = &me->mbox[me->mbox_head];
        int len = m->len; if (len > (int)a->rsi) len = (int)a->rsi;
        memcpy((void *)a->rdi, m->data, len);
        if (a->rdx) *(int *)a->rdx = m->sender;          // pid de l'émetteur
        me->mbox_head = (me->mbox_head + 1) % IPC_MBOX_LEN;
        return len;
    }
    case SYS_shm_create: {
        uint64_t size = a->rdi;
        int np = (int)((size + 4095) / 4096);
        if (np <= 0 || np > SHM_PAGES) return -1;
        int id = -1;
        for (int i = 0; i < SHM_MAX; i++) if (!shms[i].used) { id = i; break; }
        if (id < 0) return -1;
        shms[id].used = 1; shms[id].npages = np;
        for (int i = 0; i < np; i++) {
            uint64_t p = pmm_alloc_page();
            if (!p) { shms[id].used = 0; return -1; }
            shms[id].pages[i] = p;
        }
        uint64_t va = shm_map_into(sched_current(), &shms[id]);
        if (a->rsi) *(uint64_t *)a->rsi = va;
        sched_account_pages((uint64_t)np);   // mémoire partagée attribuée au créateur
        return id;
    }
    case SYS_shm_map: {
        int id = (int)a->rdi;
        if (id < 0 || id >= SHM_MAX || !shms[id].used) return -1;
        uint64_t va = shm_map_into(sched_current(), &shms[id]);
        if (a->rsi) *(uint64_t *)a->rsi = va;
        return 0;
    }
    case SYS_comp_register: {
        task_t *c = sched_current(); compositor_pid = c ? c->pid : 0; return 0;
    }
    case SYS_comp_pid:
        return compositor_pid;
    case SYS_proc_list: {
        task_t *t = sched_task_at((int)a->rdi);
        if (!t) return 0;
        procinfo_t *pi = (procinfo_t *)a->rsi;
        if (pi) {
            pi->pid       = t->pid;
            pi->state     = (int)t->state;
            pi->cpu_ticks = t->cpu_ticks;
            pi->mem_kb    = t->mem_pages * 4;
            const char *nm = t->name ? t->name : "?";
            int i = 0; for (; nm[i] && i < 31; i++) pi->name[i] = nm[i]; pi->name[i] = 0;
        }
        return 1;
    }
    case SYS_pid_alive: {
        task_t *t = sched_task_by_pid((int)a->rdi);
        return (t && t->state != TASK_UNUSED && t->state != TASK_ZOMBIE) ? 1 : 0;
    }
    case SYS_spawn: {
        // Réservé au compositeur : lui seul lance des applications (sécurité).
        task_t *me = sched_current();
        if (!me || me->pid != compositor_pid) return -1;
        int id = (int)a->rdi;
        if (id < 0 || id >= (int)(sizeof g_apps / sizeof g_apps[0])) return -1;
        return sched_new_elf_task(g_apps[id].name, g_apps[id].start,
                                  (size_t)(g_apps[id].end - g_apps[id].start));
    }
    // --- Système de fichiers (VFS du noyau) par chemin -----------------------
    case SYS_vfs_list: {
        vfs_node_t *d = vfs_resolve((const char *)a->rdi);
        if (!d || d->type != VFS_DIR) return -1;
        vfs_node_t *c = d->children;
        for (int i = 0; c && i < (int)a->rsi; i++) c = c->next;
        if (!c) return 0;
        dirent_t *e = (dirent_t *)a->rdx;
        if (e) { strncpy(e->name, c->name, 63); e->name[63] = 0;
                 e->type = (c->type == VFS_DIR) ? 1 : 0; e->size = c->size; }
        return 1;
    }
    case SYS_vfs_read: {
        vfs_io_t *io = (vfs_io_t *)a->rdi;
        vfs_node_t *f = vfs_resolve(io->path);
        if (!f || f->type != VFS_FILE) return -1;
        return vfs_read(f, io->off, io->buf, io->len);
    }
    case SYS_vfs_write: {
        vfs_io_t *io = (vfs_io_t *)a->rdi;
        if (!users_can_write_path(io->path)) return -1;
        vfs_node_t *f = vfs_resolve(io->path);
        if (!f) return -1;
        return vfs_write(f, io->off, io->buf, io->len);
    }
    case SYS_vfs_create: {
        const char *path = (const char *)a->rdi;
        if (!users_can_write_path(path)) return -1;
        char buf[256]; int slash = -1, i = 0;
        for (; path[i] && i < 255; i++) { buf[i] = path[i]; if (path[i] == '/') slash = i; }
        buf[i] = 0;
        if (slash < 0) return -1;
        const char *name = buf + slash + 1;
        char parent[256];
        if (slash == 0) { parent[0] = '/'; parent[1] = 0; }
        else { memcpy(parent, buf, slash); parent[slash] = 0; }
        vfs_node_t *p = vfs_resolve(parent);
        if (!p || p->type != VFS_DIR) return -1;
        return vfs_create(p, name, (a->rsi ? VFS_DIR : VFS_FILE)) ? 0 : -1;
    }
    case SYS_vfs_delete: {
        const char *path = (const char *)a->rdi;
        if (!users_can_write_path(path)) return -1;
        vfs_node_t *n = vfs_resolve(path);
        if (!n) return -1;
        return vfs_delete(n) ? 0 : -1;
    }
    case SYS_vfs_stat: {
        vfs_node_t *n = vfs_resolve((const char *)a->rdi);
        if (!n) return -1;
        dirent_t *e = (dirent_t *)a->rsi;
        if (e) { strncpy(e->name, n->name, 63); e->name[63] = 0;
                 e->type = (n->type == VFS_DIR) ? 1 : 0; e->size = n->size; }
        return 0;
    }
    case SYS_whoami: {
        userinfo_t *u = (userinfo_t *)a->rdi;
        const user_t *cu = users_current();
        if (u) {
            if (cu) { strncpy(u->name, cu->name, 31); u->name[31] = 0;
                      strncpy(u->home, cu->home, 95); u->home[95] = 0; u->is_admin = cu->is_admin ? 1 : 0; }
            else { u->name[0] = 0; strcpy(u->home, "/"); u->is_admin = 0; }
        }
        return 0;
    }
    case SYS_can_write:
        return users_can_write_path((const char *)a->rdi) ? 1 : 0;
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
    uint64_t entry = elf_load(pml4, (const uint8_t *)elf, len, &brk_end, NULL);
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
