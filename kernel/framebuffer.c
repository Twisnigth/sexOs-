// =============================================================================
//  kernel/framebuffer.c -- Framebuffer matériel (Limine)
// =============================================================================
#include "framebuffer.h"
#include "boot.h"

static canvas_t hw;
static uint8_t r_shift, g_shift, b_shift;

bool fb_init(struct limine_framebuffer *lfb) {
    if (!lfb || !lfb->address) return false;
    hw.pixels = (uint32_t *)lfb->address;
    hw.width  = (uint32_t)lfb->width;
    hw.height = (uint32_t)lfb->height;
    hw.pitch  = (uint32_t)lfb->pitch;
    r_shift = lfb->red_mask_shift;
    g_shift = lfb->green_mask_shift;
    b_shift = lfb->blue_mask_shift;
    return true;
}

canvas_t *fb_canvas(void) { return &hw; }

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << r_shift) | ((uint32_t)g << g_shift) | ((uint32_t)b << b_shift);
}

uint32_t fb_width(void)  { return hw.width; }
uint32_t fb_height(void) { return hw.height; }
uint32_t fb_pitch(void)  { return hw.pitch; }

// Adresse PHYSIQUE du framebuffer (l'adresse Limine est dans la fenêtre HHDM).
uint64_t fb_phys(void) { return (uint64_t)hw.pixels - boot_hhdm_offset(); }
