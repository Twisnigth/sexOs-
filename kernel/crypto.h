// =============================================================================
//  kernel/crypto.h -- Couche cryptographique (primitives modernes pour SSH)
// -----------------------------------------------------------------------------
//  S'appuie sur Monocypher (X25519, Ed25519, ChaCha20, Poly1305, SHA-512,
//  domaine public / BSD-2) + SHA-256 maison + un CSPRNG ChaCha20.
// =============================================================================
#ifndef MONOS_CRYPTO_H
#define MONOS_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"

// --- SHA-256 -----------------------------------------------------------------
typedef struct {
    uint32_t state[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

// --- CSPRNG ------------------------------------------------------------------
void csprng_init(void);
void csprng_bytes(void *buf, size_t len);

// --- Banc d'essai (vecteurs connus) : renvoie true si tout passe -------------
int crypto_selftest(void);

#endif
