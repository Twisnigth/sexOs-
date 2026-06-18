// =============================================================================
//  kernel/input.h -- File d'événements d'entrée unifiée (clavier + souris)
// =============================================================================
#ifndef MONOS_INPUT_H
#define MONOS_INPUT_H

#include <stdint.h>
#include <stdbool.h>

// Codes de touches spéciales (en plus des caractères ASCII).
enum {
    KEY_NONE = 0,
    KEY_ENTER = 0x100, KEY_BACKSPACE, KEY_ESC, KEY_TAB,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_DELETE, KEY_PAGEUP, KEY_PAGEDOWN,
    KEY_LSHIFT, KEY_RSHIFT, KEY_CTRL, KEY_ALT
};

// Modificateurs (champ bitmask).
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4

#define MOUSE_LEFT   1
#define MOUSE_RIGHT  2
#define MOUSE_MIDDLE 4

typedef enum { EV_KEY, EV_MOUSE } event_type_t;

typedef struct {
    event_type_t type;
    // --- clavier ---
    char     ch;          // caractère ASCII (0 si aucun)
    uint16_t key;         // touche spéciale (KEY_*) ou 0
    bool     pressed;     // true = appui, false = relâchement
    uint8_t  mods;        // modificateurs actifs
    // --- souris ---
    int32_t  mx, my;      // position absolue
    int32_t  dx, dy;      // déplacement relatif
    uint8_t  buttons;     // état des boutons (MOUSE_*)
} event_t;

void input_init(void);
void input_push(const event_t *e);
bool input_poll(event_t *out);   // non bloquant ; false si file vide

#endif
