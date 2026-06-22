// =============================================================================
//  kernel/clip.c -- Presse-papiers noyau (tampon unique + compteur de génération)
// =============================================================================
#include "clip.h"
#include "klib.h"

static char     buf[CLIP_CAP];
static int      len;
static uint32_t gen;

void clip_kset(const char *b, int n) {
    if (n < 0) n = 0;
    if (n > CLIP_CAP) n = CLIP_CAP;
    if (b && n) memcpy(buf, b, (size_t)n);
    len = n;
    gen++;
}

int clip_kget(char *o, int max) {
    int n = len < max ? len : max;
    if (n < 0) n = 0;
    if (o && n) memcpy(o, buf, (size_t)n);
    return n;
}

uint32_t clip_kgen(void) { return gen; }
