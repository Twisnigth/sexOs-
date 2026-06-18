// =============================================================================
//  kernel/kmain.c -- Point d'entrée du noyau MonOS v2 (Phase 0)
// -----------------------------------------------------------------------------
//  Phase 0 : on démarre via Limine (UEFI ou BIOS), on récupère un framebuffer
//  linéaire (obtenu par Limine via GOP sous UEFI / VBE sous BIOS), et on affiche
//  un écran d'accueil (splash) avec le titre, les informations du framebuffer et
//  la mascotte verticale.
//
//  Le reste de l'OS (mémoire, interruptions, pilotes, interface graphique...)
//  sera ajouté phase par phase.
// =============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "limine.h"
#include "font8x16.h"

// -----------------------------------------------------------------------------
//  Requêtes adressées à Limine
// -----------------------------------------------------------------------------
//  On annonce la révision de base du protocole supportée, puis on encadre nos
//  requêtes par les marqueurs attendus par le bootloader.

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

// Demande d'un framebuffer linéaire.
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

// -----------------------------------------------------------------------------
//  Petites fonctions utilitaires (pas de libc en environnement autonome)
// -----------------------------------------------------------------------------
static size_t k_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

// Convertit un entier non signé en chaîne décimale. Renvoie la longueur écrite.
static int u64_to_dec(uint64_t v, char *buf) {
    char tmp[21];
    int i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
    return j;
}

// -----------------------------------------------------------------------------
//  Mini-pilote framebuffer (suffisant pour la Phase 0)
// -----------------------------------------------------------------------------
typedef struct {
    uint8_t  *base;       // adresse du framebuffer
    uint64_t  width;
    uint64_t  height;
    uint64_t  pitch;      // octets par ligne
    uint16_t  bpp;        // bits par pixel
    uint8_t   r_shift, g_shift, b_shift;
} fb_t;

static fb_t fb;

// Compose une couleur 32 bits selon la disposition des canaux du framebuffer.
static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << fb.r_shift) |
           ((uint32_t)g << fb.g_shift) |
           ((uint32_t)b << fb.b_shift);
}

static inline void put_pixel(uint64_t x, uint64_t y, uint32_t color) {
    if (x >= fb.width || y >= fb.height) return;
    uint32_t *px = (uint32_t *)(fb.base + y * fb.pitch + x * 4);
    *px = color;
}

// Remplit tout l'écran d'une couleur unie.
static void fill_screen(uint32_t color) {
    for (uint64_t y = 0; y < fb.height; y++) {
        uint32_t *row = (uint32_t *)(fb.base + y * fb.pitch);
        for (uint64_t x = 0; x < fb.width; x++) row[x] = color;
    }
}

// Remplit un rectangle.
static void fill_rect(uint64_t x0, uint64_t y0, uint64_t w, uint64_t h, uint32_t color) {
    for (uint64_t y = y0; y < y0 + h && y < fb.height; y++)
        for (uint64_t x = x0; x < x0 + w && x < fb.width; x++)
            put_pixel(x, y, color);
}

// Dessine un caractère 8x16 avec un facteur d'échelle entier.
static void draw_char(char c, uint64_t x, uint64_t y, uint32_t fg, int scale) {
    const uint8_t *glyph = font8x16[(uint8_t)c];
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                fill_rect(x + (uint64_t)col * scale, y + (uint64_t)row * scale,
                          scale, scale, fg);
            }
        }
    }
}

// Dessine une chaîne (sans gestion de retour à la ligne).
static void draw_string(const char *s, uint64_t x, uint64_t y, uint32_t fg, int scale) {
    uint64_t cx = x;
    for (size_t i = 0; s[i]; i++) {
        draw_char(s[i], cx, y, fg, scale);
        cx += (uint64_t)FONT_WIDTH * scale;
    }
}

// Dessine une chaîne centrée horizontalement.
static void draw_string_centered(const char *s, uint64_t y, uint32_t fg, int scale) {
    uint64_t w = (uint64_t)k_strlen(s) * FONT_WIDTH * scale;
    uint64_t x = (fb.width > w) ? (fb.width - w) / 2 : 0;
    draw_string(s, x, y, fg, scale);
}

// -----------------------------------------------------------------------------
//  Mascotte verticale : "8==D" pivoté de 90° vers la gauche.
//  Le gland (D) est en haut, le fût au milieu, la base (8) en bas.
// -----------------------------------------------------------------------------
static void draw_mascotte(uint64_t cx, uint64_t y, uint32_t color, int scale) {
    static const char *lines[] = { "D", "|", "|", "|", "8" };
    uint64_t cw = (uint64_t)FONT_WIDTH * scale;
    uint64_t ch = (uint64_t)FONT_HEIGHT * scale;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        draw_string(lines[i], cx - cw / 2, y + i * ch, color, scale);
    }
}

// Boucle d'arrêt définitif.
static void hcf(void) {
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

// -----------------------------------------------------------------------------
//  Point d'entrée appelé par Limine
// -----------------------------------------------------------------------------
void kmain(void) {
    // Vérifie que Limine supporte bien la révision de base demandée.
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        hcf();
    }

    // Vérifie qu'on a bien reçu au moins un framebuffer.
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }

    struct limine_framebuffer *lfb = framebuffer_request.response->framebuffers[0];
    fb.base    = (uint8_t *)lfb->address;
    fb.width   = lfb->width;
    fb.height  = lfb->height;
    fb.pitch   = lfb->pitch;
    fb.bpp     = lfb->bpp;
    fb.r_shift = lfb->red_mask_shift;
    fb.g_shift = lfb->green_mask_shift;
    fb.b_shift = lfb->blue_mask_shift;

    // --- Couleurs du thème ---------------------------------------------------
    uint32_t bg     = rgb(0x12, 0x14, 0x22); // bleu nuit
    uint32_t accent = rgb(0x4e, 0xc9, 0xff); // cyan clair
    uint32_t white  = rgb(0xff, 0xff, 0xff);
    uint32_t grey   = rgb(0x9a, 0xa0, 0xb4);
    uint32_t pink   = rgb(0xff, 0x7a, 0xb0); // mascotte

    // --- Fond + bandeau supérieur --------------------------------------------
    fill_screen(bg);
    fill_rect(0, 0, fb.width, 6, accent);

    // --- Titre ---------------------------------------------------------------
    uint64_t cy = fb.height / 6;
    draw_string_centered("MonOS", cy, white, 6);
    draw_string_centered("version 2.0  --  Phase 0 : boot UEFI/BIOS via Limine",
                         cy + 6 * FONT_HEIGHT + 16, accent, 2);

    // --- Mascotte centrée ----------------------------------------------------
    draw_mascotte(fb.width / 2, cy + 6 * FONT_HEIGHT + 80, pink, 4);

    // --- Informations sur le framebuffer (preuve qu'on l'a bien lu) ----------
    char line[96];
    char num[24];
    size_t p = 0;
    const char *prefix = "Framebuffer : ";
    for (size_t i = 0; prefix[i]; i++) line[p++] = prefix[i];
    u64_to_dec(fb.width, num);  for (size_t i = 0; num[i]; i++) line[p++] = num[i];
    line[p++] = ' '; line[p++] = 'x'; line[p++] = ' ';
    u64_to_dec(fb.height, num); for (size_t i = 0; num[i]; i++) line[p++] = num[i];
    line[p++] = ' '; line[p++] = 'x'; line[p++] = ' ';
    u64_to_dec(fb.bpp, num);    for (size_t i = 0; num[i]; i++) line[p++] = num[i];
    const char *suffix = " bpp";
    for (size_t i = 0; suffix[i]; i++) line[p++] = suffix[i];
    line[p] = 0;
    draw_string_centered(line, fb.height - 120, grey, 2);

    draw_string_centered("Demarrage reussi. (noyau 64 bits, framebuffer actif)",
                         fb.height - 80, white, 2);

    // Phase 0 : rien d'autre à faire pour l'instant.
    hcf();
}
