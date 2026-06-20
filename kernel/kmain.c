// =============================================================================
//  kernel/kmain.c -- Point d'entrée et orchestration du noyau sexOs v2
// -----------------------------------------------------------------------------
//  Séquence de démarrage :
//    Limine -> kmain -> (série, framebuffer, GDT, IDT, PIC, PIT, mémoire,
//    entrées, interface graphique).
// =============================================================================
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "limine.h"
#include "boot.h"
#include "klib.h"
#include "serial.h"
#include "framebuffer.h"
#include "gfx.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "io.h"
#include "pmm.h"
#include "heap.h"
#include "vmm.h"
#include "ps2.h"
#include "rtc.h"
#include "pci.h"
#include "vfs.h"
#include "users.h"
#include "net.h"
#include "crypto.h"
#include "ssh.h"
#include "httpd.h"
#include "speaker.h"
#include "fs.h"
#include "proc.h"
#include "pkg.h"
#include "sched.h"
#include "test_user_bin.h"
#include "desktop.h"
#include "ascii_art.h"

// -----------------------------------------------------------------------------
//  Requêtes Limine
// -----------------------------------------------------------------------------
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0,
};
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0,
};
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0,
};
__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST, .revision = 0,
};
__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST, .revision = 0,
};

// Recherche un module chargé par Limine par son nom de fichier.
void *boot_module(const char *name, uint64_t *size) {
    if (!module_request.response) return NULL;
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file *f = module_request.response->modules[i];
        const char *base = f->path;
        for (const char *p = f->path; *p; p++) if (*p == '/') base = p + 1;
        if (strcmp(base, name) == 0) { if (size) *size = f->size; return f->address; }
    }
    return NULL;
}

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

// -----------------------------------------------------------------------------
//  Accesseurs (boot.h)
// -----------------------------------------------------------------------------
uint64_t boot_hhdm_offset(void) {
    return hhdm_request.response ? hhdm_request.response->offset : 0;
}
struct limine_memmap_response *boot_memmap(void) {
    return memmap_request.response;
}
struct limine_framebuffer *boot_framebuffer(void) {
    if (!framebuffer_request.response ||
        framebuffer_request.response->framebuffer_count < 1) return NULL;
    return framebuffer_request.response->framebuffers[0];
}
void *boot_rsdp(void) {
    return rsdp_request.response ? rsdp_request.response->address : NULL;
}

// -----------------------------------------------------------------------------
//  Écran d'accueil (splash) pendant l'initialisation
// -----------------------------------------------------------------------------
static void draw_splash(void) {
    canvas_t *c = fb_canvas();
    uint32_t bg     = fb_rgb(0x12, 0x14, 0x22);
    uint32_t accent = fb_rgb(0x4e, 0xc9, 0xff);
    uint32_t pink   = fb_rgb(0xff, 0x7a, 0xb0);

    canvas_fill(c, bg);
    canvas_fill_rect(c, 0, 0, c->width, 6, accent);

    // Banniere ASCII « sexOs » centree (echelle 2), suivie du sous-titre.
    int cx = c->width / 2;
    int ty = c->height / 4;
    int sc = (c->width >= 700) ? 2 : 1;
    for (int i = 0; i < SEXOS_BANNER_LINES; i++)
        canvas_draw_string(c, sexos_banner[i],
                           cx - canvas_text_width(sexos_banner[i], sc) / 2,
                           ty + i * 16 * sc, pink, sc);
    const char *sub = "version 2.0  --  demarrage du systeme";
    canvas_draw_string(c, sub, cx - canvas_text_width(sub, 2) / 2,
                       ty + SEXOS_BANNER_LINES * 16 * sc + 24, accent, 2);
}

// -----------------------------------------------------------------------------
//  Point d'entrée
// -----------------------------------------------------------------------------
// Active SSE/SSE2 (les binaires Linux compilés normalement en ont besoin).
static void enable_sse(void) {
    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);            // EM = 0 (pas d'émulation x87)
    cr0 |=  (1ULL << 1);            // MP = 1
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);   // OSFXSR | OSXMMEXCPT
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
    __asm__ volatile ("fninit");
}

void kmain(void) {
    serial_init();
    enable_sse();
    kprintf("\n=== sexOs v2 : demarrage du noyau ===\n");

    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        kprintf("[boot] revision Limine non supportee\n");
        for (;;) hlt();
    }

    // --- Affichage -----------------------------------------------------------
    struct limine_framebuffer *lfb = boot_framebuffer();
    if (!lfb || !fb_init(lfb)) {
        kprintf("[boot] pas de framebuffer !\n");
        for (;;) hlt();
    }
    kprintf("[fb] %ux%u, %u bpp\n", (uint32_t)lfb->width, (uint32_t)lfb->height, lfb->bpp);
    draw_splash();

    // --- Phase 1 : cœur CPU --------------------------------------------------
    gdt_init();
    idt_init();
    pic_remap();
    pit_init(1000);          // 1000 Hz -> résolution 1 ms

    // --- Phase 2 : mémoire ---------------------------------------------------
    pmm_init();
    heap_init();
    vmm_pat_init();          // entrée PAT Write-Combining (framebuffer rapide)

    // --- Horloge + bus PCI ---------------------------------------------------
    rtc_init();
    pci_init();

    // --- Phase 3 : entrées (clavier + souris PS/2) ---------------------------
    ps2_init();

    sti();                   // on active les interruptions
    kprintf("[cpu] interruptions activees\n");
    speaker_jingle();        // petit son de demarrage (haut-parleur PC)

    // --- Cryptographie (CSPRNG + validation par vecteurs de test) ------------
    csprng_init();
    crypto_selftest();

    // --- Ring 3 + binaires ELF Linux (Phase 4) -------------------------------
    syscall_init();
    const char *av1[] = { "test" };
    int rc = proc_run(test_user_elf, test_user_elf_len, 1, av1);
    kprintf("[proc] binaire musl-libc de test termine, code = %d\n", rc);

    // --- Démo ordonnanceur multi-processus ring 3 (refactor Phase 1) ---------
    //  Prouve : plusieurs tâches ring 3 concurrentes (A/B entrelacés), faute
    //  ring 3 -> tâche tuée sans planter le noyau, retour propre à l'idle.
    {
        extern uint8_t utest_a_start[], utest_a_end[];
        extern uint8_t utest_b_start[], utest_b_end[];
        extern uint8_t utest_crash_start[], utest_crash_end[];
        kprintf("\n--- demo ordonnanceur (Phase 1) ---\n");
        sched_new_flat_task("A", utest_a_start, (size_t)(utest_a_end - utest_a_start));
        sched_new_flat_task("B", utest_b_start, (size_t)(utest_b_end - utest_b_start));
        sched_new_flat_task("crash", utest_crash_start,
                            (size_t)(utest_crash_end - utest_crash_start));
        sched_run_until_idle();
        kprintf("\n--- fin demo ordonnanceur (noyau intact) ---\n");
    }

    // --- Réseau (carte e1000 + configuration automatique par DHCP) -----------
    net_init();
    if (nic_present()) { net_dhcp(); ssh_server_init(); httpd_init(); }

    // --- VFS + comptes côté NOYAU (partagés par sshd, pacman ET le bureau) ---
    vfs_init();
    fs_init();               // restaure l'arborescence depuis le disque si present
    users_init();
    // Session par défaut (la connexion en multi-processus n'est pas encore câblée).
    for (int i = 0; i < users_count(); i++)
        if (strcmp(users_get(i)->name, "user") == 0) { users_set_current(users_get(i)); break; }

    // --- Bureau MULTI-PROCESSUS en RING 3 (isolation par processus, IPC) -----
    //  Le compositeur et CHAQUE application sont des PROCESSUS ring 3 distincts,
    //  reliés par messagerie + mémoire partagée. IPC BLOQUANTE : une tâche en
    //  attente d'événement dort (pas de sondage actif) et est réveillée à la
    //  livraison. Un crash d'appli est contenu par le noyau (kill-on-fault).
    {
        extern uint8_t ucomp_start[], ucomp_end[];
        kprintf("[boot] lancement du compositeur (ring 3, IPC)\n");
        // Service réseau : tâche NOYAU qui pompe le NIC et fait avancer TCP/DNS
        // (le NIC n'est plus jamais sondé ailleurs après le démarrage).
        if (nic_present()) sched_new_kernel_task("reseau", net_task_run);
        //  Serveur SSH : tâche NOYAU qui accepte et sert les connexions (port 22),
        //  en E/S non bloquantes par-dessus la tâche réseau.
        if (nic_present()) sched_new_kernel_task("sshd", sshd_run);
        //  Serveur web : sert les fichiers du VFS sur le port 80.
        if (nic_present()) sched_new_kernel_task("httpd", httpd_run);
        //  AUCUNE application n'est lancée au démarrage : seul le compositeur
        //  (le bureau) tourne. L'utilisateur lance les applications À LA DEMANDE
        //  depuis le menu du dock, qui appelle SYS_spawn (terminal, explorateur,
        //  horloge, moniteur, navigateur, paramètres). Chaque appli reste un
        //  PROCESSUS ring 3 distinct relié au compositeur par IPC + mémoire
        //  partagée ; un crash d'appli est contenu par le noyau.
        sched_new_elf_task("compositeur", ucomp_start, (size_t)(ucomp_end - ucomp_start));
    }
    sched_start();                          // ne revient jamais

    for (;;) hlt();                          // (inatteignable)
}
