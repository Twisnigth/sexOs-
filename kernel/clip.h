// =============================================================================
//  kernel/clip.h -- Presse-papiers noyau (partagé entre applications ring 3
//  via SYS_clip_set/get, et avec l'hôte via l'agent VMware).
// =============================================================================
#ifndef SEXOS_CLIP_H
#define SEXOS_CLIP_H

#include <stdint.h>

#define CLIP_CAP 8192

void     clip_kset(const char *buf, int len);   // écrit (incrémente la génération)
int      clip_kget(char *out, int max);          // -> octets copiés
uint32_t clip_kgen(void);                         // génération (change à chaque écriture)

#endif
