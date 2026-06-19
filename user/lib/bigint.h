// =============================================================================
//  user/lib/bigint.h -- Entiers multi-précision pour la vérification RSA (TLS)
// -----------------------------------------------------------------------------
//  Taille fixe (jusqu'à 4096 bits). Seules les opérations nécessaires à la
//  VÉRIFICATION de signature RSA sont fournies (exponentiation modulaire avec
//  exposant public). Pas de code secret ici : tout est public (clé + signature),
//  donc le temps constant n'est pas requis.
// =============================================================================
#ifndef SEXOS_BIGINT_H
#define SEXOS_BIGINT_H

#include <stdint.h>

#define BN_LIMBS 128                 // 128 * 32 = 4096 bits
typedef struct { uint32_t v[BN_LIMBS]; } bn_t;   // limbes 32 bits, petit-boutiste

void bn_zero(bn_t *a);
// Charge un grand entier depuis des octets gros-boutistes (DER). 0 si trop grand.
int  bn_from_be(bn_t *a, const uint8_t *b, int len);
int  bn_cmp(const bn_t *a, const bn_t *b);       // -1 / 0 / 1
void bn_to_be(const bn_t *a, uint8_t *out, int len);   // écrit 'len' octets gros-boutistes
int  bn_bytelen(const bn_t *a);                  // nombre d'octets significatifs
int  bn_bitlen(const bn_t *a);                   // nombre de bits significatifs
// out = base ^ exp mod mod  (exp en octets gros-boutistes : exposant public).
void bn_modexp(bn_t *out, const bn_t *base, const uint8_t *exp, int elen, const bn_t *mod);
// r = a * b mod n  (utilisé par l'arithmétique de courbe ECDSA).
void bn_modmul(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *n);

#endif
