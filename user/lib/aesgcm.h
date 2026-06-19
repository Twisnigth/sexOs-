// =============================================================================
//  user/lib/aesgcm.h -- AES-128 + GCM (AEAD) pour TLS_AES_128_GCM_SHA256
// =============================================================================
#ifndef MONOS_AESGCM_H
#define MONOS_AESGCM_H
#include <stdint.h>

// Scelle : chiffre pt -> ct (même longueur) et produit tag[16].
void aes128gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t *aad, int aadlen,
                    const uint8_t *pt, int ptlen, uint8_t *ct, uint8_t tag[16]);
// Ouvre : vérifie tag puis déchiffre ct -> pt. Renvoie 0 si OK, -1 sinon.
int  aes128gcm_open(const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t *aad, int aadlen,
                    const uint8_t *ct, int ctlen, const uint8_t tag[16], uint8_t *pt);
#endif
