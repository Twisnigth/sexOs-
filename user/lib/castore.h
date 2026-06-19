// =============================================================================
//  user/lib/castore.h -- Magasin d'autorités de certification racines (DER)
// -----------------------------------------------------------------------------
//  Petit magasin embarqué. Pour le réseau public réel il faudrait y mettre le
//  paquet complet (Mozilla, ~150 AC) ; ici on embarque la racine de test.
// =============================================================================
#ifndef SEXOS_CASTORE_H
#define SEXOS_CASTORE_H
#include <stdint.h>
int            castore_count(void);
const uint8_t *castore_der(int i, int *len);
#endif
