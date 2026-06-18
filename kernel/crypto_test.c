// =============================================================================
//  kernel/crypto_test.c -- Validation de la couche crypto par vecteurs connus
// =============================================================================
#include "crypto.h"
#include "klib.h"

static int eq(const uint8_t *a, const uint8_t *b, int n) {
    return memcmp(a, b, n) == 0;
}

int crypto_selftest(void) {
    int fail = 0;

    // --- SHA-256("abc") (FIPS 180-4) -----------------------------------------
    {
        static const uint8_t expect[32] = {
            0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
            0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
        uint8_t out[32];
        sha256("abc", 3, out);
        if (!eq(out, expect, 32)) { kprintf("[crypto] SHA-256 ECHEC\n"); fail++; }
        else kprintf("[crypto] SHA-256 OK\n");
    }

    // --- SHA-512("abc") (FIPS 180-4) -----------------------------------------
    {
        static const uint8_t expect[64] = {
            0xdd,0xaf,0x35,0xa1,0x93,0x61,0x7a,0xba,0xcc,0x41,0x73,0x49,0xae,0x20,0x41,0x31,
            0x12,0xe6,0xfa,0x4e,0x89,0xa9,0x7e,0xa2,0x0a,0x9e,0xee,0xe6,0x4b,0x55,0xd3,0x9a,
            0x21,0x92,0x99,0x2a,0x27,0x4f,0xc1,0xa8,0x36,0xba,0x3c,0x23,0xa3,0xfe,0xeb,0xbd,
            0x45,0x4d,0x44,0x23,0x64,0x3c,0xe8,0x0e,0x2a,0x9a,0xc9,0x4f,0xa5,0x4c,0xa4,0x9f };
        uint8_t out[64];
        crypto_sha512(out, (const uint8_t *)"abc", 3);
        if (!eq(out, expect, 64)) { kprintf("[crypto] SHA-512 ECHEC\n"); fail++; }
        else kprintf("[crypto] SHA-512 OK\n");
    }

    // --- X25519 : échange Diffie-Hellman (round-trip) ------------------------
    {
        uint8_t a_sk[32], b_sk[32], a_pk[32], b_pk[32], s1[32], s2[32];
        csprng_bytes(a_sk, 32); csprng_bytes(b_sk, 32);
        crypto_x25519_public_key(a_pk, a_sk);
        crypto_x25519_public_key(b_pk, b_sk);
        crypto_x25519(s1, a_sk, b_pk);
        crypto_x25519(s2, b_sk, a_pk);
        uint8_t zero[32]; memset(zero, 0, 32);
        if (!eq(s1, s2, 32) || eq(s1, zero, 32)) { kprintf("[crypto] X25519 ECHEC\n"); fail++; }
        else kprintf("[crypto] X25519 (DH) OK\n");
    }

    // --- Ed25519 : signature + vérification ----------------------------------
    {
        uint8_t seed[32], sk[64], pk[32], sig[64];
        csprng_bytes(seed, 32);
        crypto_ed25519_key_pair(sk, pk, seed);
        const char *msg = "MonOS SSH host key test";
        crypto_ed25519_sign(sig, sk, (const uint8_t *)msg, strlen(msg));
        int ok = crypto_ed25519_check(sig, pk, (const uint8_t *)msg, strlen(msg));
        sig[0] ^= 1;   // corruption -> doit échouer
        int bad = crypto_ed25519_check(sig, pk, (const uint8_t *)msg, strlen(msg));
        if (ok != 0 || bad == 0) { kprintf("[crypto] Ed25519 ECHEC\n"); fail++; }
        else kprintf("[crypto] Ed25519 (sign/verify) OK\n");
    }

    // --- ChaCha20-Poly1305 : chiffrement authentifié (round-trip) ------------
    {
        uint8_t key[32], nonce[24], mac[16];
        csprng_bytes(key, 32); csprng_bytes(nonce, 24);
        const char *pt = "Message secret pour SSH";
        int n = strlen(pt);
        uint8_t ct[64], dt[64];
        crypto_aead_lock(ct, mac, key, nonce, NULL, 0, (const uint8_t *)pt, n);
        int r = crypto_aead_unlock(dt, mac, key, nonce, NULL, 0, ct, n);
        if (r != 0 || !eq(dt, (const uint8_t *)pt, n)) { kprintf("[crypto] AEAD ECHEC\n"); fail++; }
        else kprintf("[crypto] ChaCha20-Poly1305 OK\n");
    }

    kprintf("[crypto] banc d'essai : %d echec(s)\n", fail);
    return fail;
}
