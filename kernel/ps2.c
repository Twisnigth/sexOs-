// =============================================================================
//  kernel/ps2.c -- Clavier (IRQ1, AZERTY) + souris (IRQ12) PS/2
// =============================================================================
#include "ps2.h"
#include "input.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "klib.h"
#include "framebuffer.h"

#define PS2_DATA 0x60
#define PS2_CMD  0x64
#define PS2_STAT 0x64

// -----------------------------------------------------------------------------
//  Tables scancode (jeu 1) -> ASCII, disposition AZERTY (cf. OS legacy)
// -----------------------------------------------------------------------------
static const char keymap[0x3A] = {
    0,0, '1','2','3','4','5','6','7','8','9','0','-','=', '\b', 0,
    'a','z','e','r','t','y','u','i','o','p', 0,0, '\n', 0,
    'q','s','d','f','g','h','j','k','l','m', 0,0, 0, 0,
    'w','x','c','v','b','n', ',',';',':','!', 0, '*', 0, ' '
};
static const char keymap_shift[0x3A] = {
    0,0, '1','2','3','4','5','6','7','8','9','0','_','+', '\b', 0,
    'A','Z','E','R','T','Y','U','I','O','P', 0,0, '\n', 0,
    'Q','S','D','F','G','H','J','K','L','M', 0,0, 0, 0,
    'W','X','C','V','B','N', '?','.','/','!', 0, '*', 0, ' '
};

static uint8_t modifiers;        // état des modificateurs
static bool extended;            // dernier octet = préfixe 0xE0

// -----------------------------------------------------------------------------
//  Synchronisation du contrôleur
// -----------------------------------------------------------------------------
static void ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(PS2_STAT) & 2)) return;
}
static void ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++) if (inb(PS2_STAT) & 1) return;
}
static void ps2_cmd(uint8_t c) { ps2_wait_write(); outb(PS2_CMD, c); }
static void mouse_write(uint8_t v) {
    ps2_cmd(0xD4);              // l'octet suivant va à la souris
    ps2_wait_write(); outb(PS2_DATA, v);
}
static uint8_t mouse_read(void) { ps2_wait_read(); return inb(PS2_DATA); }

// -----------------------------------------------------------------------------
//  Clavier
// -----------------------------------------------------------------------------
static void keyboard_irq(registers_t *r) {
    (void)r;
    uint8_t sc = inb(PS2_DATA);

    if (sc == 0xE0) { extended = true; return; }

    bool released = sc & 0x80;
    uint8_t code = sc & 0x7F;

    event_t e = {0};
    e.type = EV_KEY;
    e.pressed = !released;

    // Touches étendues (flèches, etc.).
    if (extended) {
        extended = false;
        switch (code) {
            case 0x48: e.key = KEY_UP; break;
            case 0x50: e.key = KEY_DOWN; break;
            case 0x4B: e.key = KEY_LEFT; break;
            case 0x4D: e.key = KEY_RIGHT; break;
            case 0x47: e.key = KEY_HOME; break;
            case 0x4F: e.key = KEY_END; break;
            case 0x53: e.key = KEY_DELETE; break;
            case 0x49: e.key = KEY_PAGEUP; break;
            case 0x51: e.key = KEY_PAGEDOWN; break;
            default: return;
        }
        e.mods = modifiers;
        input_push(&e);
        return;
    }

    // Modificateurs.
    if (code == 0x2A || code == 0x36) {           // Maj
        if (released) modifiers &= ~MOD_SHIFT; else modifiers |= MOD_SHIFT;
        return;
    }
    if (code == 0x1D) {                           // Ctrl
        if (released) modifiers &= ~MOD_CTRL; else modifiers |= MOD_CTRL;
        return;
    }
    if (code == 0x38) {                           // Alt
        if (released) modifiers &= ~MOD_ALT; else modifiers |= MOD_ALT;
        return;
    }

    if (released) return;                         // on n'émet que les appuis

    e.mods = modifiers;
    if (code == 0x01) { e.key = KEY_ESC; input_push(&e); return; }
    if (code == 0x0F) { e.key = KEY_TAB; input_push(&e); return; }

    if (code < 0x3A) {
        char c = (modifiers & MOD_SHIFT) ? keymap_shift[code] : keymap[code];
        if (c == '\n') { e.key = KEY_ENTER; }
        else if (c == '\b') { e.key = KEY_BACKSPACE; }
        else if (c) { e.ch = c; }
        else return;
        input_push(&e);
    }
}

// -----------------------------------------------------------------------------
//  Souris
// -----------------------------------------------------------------------------
static int32_t mouse_x, mouse_y;
static uint8_t mouse_buttons;
static uint8_t mouse_cycle, mouse_packet[3];

static void mouse_irq(registers_t *r) {
    (void)r;
    uint8_t status = inb(PS2_STAT);
    if (!(status & 0x20)) { inb(PS2_DATA); return; }   // pas un octet souris

    uint8_t data = inb(PS2_DATA);
    switch (mouse_cycle) {
        case 0:
            if (!(data & 0x08)) return;     // resynchronisation (bit 3 toujours 1)
            mouse_packet[0] = data;
            mouse_cycle = 1;
            break;
        case 1:
            mouse_packet[1] = data;
            mouse_cycle = 2;
            break;
        case 2: {
            mouse_packet[2] = data;
            mouse_cycle = 0;

            uint8_t flags = mouse_packet[0];
            int dx = mouse_packet[1];
            int dy = mouse_packet[2];
            if (flags & 0x10) dx |= 0xFFFFFF00;   // extension de signe X
            if (flags & 0x20) dy |= 0xFFFFFF00;   // extension de signe Y
            if (flags & 0xC0) { dx = 0; dy = 0; } // dépassement : on ignore

            mouse_x += dx;
            mouse_y -= dy;                         // Y écran inversé
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_x >= (int32_t)fb_width())  mouse_x = fb_width() - 1;
            if (mouse_y >= (int32_t)fb_height()) mouse_y = fb_height() - 1;
            mouse_buttons = flags & 0x07;

            event_t e = {0};
            e.type = EV_MOUSE;
            e.mx = mouse_x; e.my = mouse_y;
            e.dx = dx; e.dy = -dy;
            e.buttons = mouse_buttons;
            input_push(&e);
            break;
        }
    }
}

void ps2_mouse_pos(int32_t *x, int32_t *y) { *x = mouse_x; *y = mouse_y; }
uint8_t ps2_mouse_buttons(void) { return mouse_buttons; }

// -----------------------------------------------------------------------------
//  Initialisation
// -----------------------------------------------------------------------------
void ps2_init(void) {
    input_init();
    mouse_x = fb_width() / 2;
    mouse_y = fb_height() / 2;

    // Vide le tampon de sortie.
    while (inb(PS2_STAT) & 1) inb(PS2_DATA);

    // Active le périphérique auxiliaire (souris).
    ps2_cmd(0xA8);

    // Lit l'octet de configuration, active les IRQ clavier (bit0) et souris (bit1).
    ps2_cmd(0x20);
    uint8_t config = mouse_read();
    config |= 0x03;
    config &= ~0x40;                 // désactive la traduction ? on garde le set par défaut
    config |= 0x40;                  // (on conserve la traduction set 1)
    ps2_cmd(0x60);
    ps2_wait_write(); outb(PS2_DATA, config);

    // Réglages souris : défauts, puis cadence d'échantillonnage 200 Hz (curseur
    // plus fluide que les 100 Hz par défaut), puis activation du report.
    mouse_write(0xF6); mouse_read();     // ACK (valeurs par défaut)
    mouse_write(0xF3); mouse_read();     // set sample rate
    mouse_write(200);  mouse_read();     // 200 échantillons/s
    mouse_write(0xF4); mouse_read();     // ACK (active le report de données)

    irq_register(1, keyboard_irq);
    irq_register(12, mouse_irq);
    pic_clear_mask(1);
    pic_clear_mask(12);
    pic_clear_mask(2);               // cascade vers le PIC esclave

    kprintf("[ps2] clavier (AZERTY) + souris initialises\n");
}
