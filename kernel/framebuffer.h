// =============================================================================
//  kernel/framebuffer.h -- Framebuffer matériel fourni par Limine
// =============================================================================
#ifndef SEXOS_FRAMEBUFFER_H
#define SEXOS_FRAMEBUFFER_H

#include "gfx.h"
#include "limine.h"

// Initialise depuis la réponse Limine. Renvoie false si pas de framebuffer.
bool fb_init(struct limine_framebuffer *lfb);

// Canvas correspondant à l'écran physique.
canvas_t *fb_canvas(void);

// Compose une couleur au format natif du framebuffer.
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_pitch(void);
uint64_t fb_phys(void);          // adresse physique (pour mapper en ring 3)

#endif
