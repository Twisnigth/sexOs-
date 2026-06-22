// =============================================================================
//  user/imgdec.h -- Décodeur d'images multi-formats (PNG / JPEG / BMP / PPM)
// -----------------------------------------------------------------------------
//  Détecte le format d'après l'en-tête et décode vers un tampon de pixels
//  0x00RRGGBB. Renvoie 1 (succès, remplit *w et *h) ou 0 (échec / non géré).
//  Les pixels sont écrits en out[y * (*w) + x] (l'appelant garantit (*w)<=maxw
//  et (*h)<=maxh, sinon le décodage échoue).
// =============================================================================
#ifndef SEXOS_IMGDEC_H
#define SEXOS_IMGDEC_H

#include <stdint.h>

int img_decode(const uint8_t *data, int n, uint32_t *out,
               int maxw, int maxh, int *w, int *h);

#endif
