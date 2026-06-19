// =============================================================================
//  user/lib/rsa.h -- Vérification de signature RSA (PKCS#1 v1.5 + PSS, SHA-256)
// =============================================================================
#ifndef MONOS_RSA_H
#define MONOS_RSA_H

#include <stdint.h>
#include "bigint.h"

// Vérifie une signature RSA (clé publique = modulus n + exposant e gros-boutiste)
// sur 'hash' (SHA-256, 32 octets). Renvoie 1 si valide, 0 sinon.
int rsa_verify_pkcs1_sha256(const bn_t *n, const uint8_t *e, int elen,
                            const uint8_t *sig, int siglen, const uint8_t hash[32]);
int rsa_verify_pss_sha256(const bn_t *n, const uint8_t *e, int elen,
                          const uint8_t *sig, int siglen, const uint8_t hash[32]);

#endif
