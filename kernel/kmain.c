// =============================================================================
//  kernel/kmain.c -- Point d'entrée et orchestration du noyau MonOS v2
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
#include "ps2.h"
#include "rtc.h"
#include "pci.h"
#include "vfs.h"
#include "users.h"
#include "net.h"
#include "crypto.h"
#include "ssh.h"
#include "desktop.h"

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
    uint32_t white  = fb_rgb(0xff, 0xff, 0xff);
    uint32_t pink   = fb_rgb(0xff, 0x7a, 0xb0);

    canvas_fill(c, bg);
    canvas_fill_rect(c, 0, 0, c->width, 6, accent);

    int cx = c->width / 2;
    int ty = c->height / 5;
    canvas_draw_string(c, "MonOS", cx - canvas_text_width("MonOS", 6) / 2, ty, white, 6);
    const char *sub = "version 2.0  --  demarrage du systeme";
    canvas_draw_string(c, sub, cx - canvas_text_width(sub, 2) / 2, ty + 6 * 16 + 16, accent, 2);

    // Mascotte verticale (D | | | 8).
    const char *m[] = { "D", "|", "|", "|", "8" };
    for (int i = 0; i < 5; i++)
        canvas_draw_string(c, m[i], cx - 4 * 4, ty + 6 * 16 + 70 + i * 16 * 4, pink, 4);
}

// -----------------------------------------------------------------------------
//  Point d'entrée
// -----------------------------------------------------------------------------
void kmain(void) {
    serial_init();
    kprintf("\n=== MonOS v2 : demarrage du noyau ===\n");

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

    // --- Horloge + bus PCI ---------------------------------------------------
    rtc_init();
    pci_init();

    // --- Phase 3 : entrées (clavier + souris PS/2) ---------------------------
    ps2_init();

    sti();                   // on active les interruptions
    kprintf("[cpu] interruptions activees\n");

    // --- Cryptographie (CSPRNG + validation par vecteurs de test) ------------
    csprng_init();
    crypto_selftest();

    // --- Réseau (carte e1000 + configuration automatique par DHCP) -----------
    net_init();
    if (nic_present()) net_dhcp();

    // --- Système de fichiers + comptes utilisateurs --------------------------
    vfs_init();
    users_init();

    // --- Phases 4-9 : bureau graphique (login, fenetres, applications) -------
    kprintf("[boot] lancement du bureau\n");
    desktop_run();

    for (;;) hlt();
}
