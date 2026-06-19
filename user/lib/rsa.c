// =============================================================================
//  user/lib/rsa.c -- Vérification RSA (PKCS#1 v1.5 et PSS, condensé SHA-256)
//  Conforme à la RFC 8017 (PKCS#1 v2.2). Ne manipule que des données publiques.
// =============================================================================
#include "rsa.h"
#include "crypto.h"            // sha256 / sha256_ctx

void *memcpy(void *, const void *, unsigned long);

#define HLEN 32               // SHA-256

// Préfixe DigestInfo ASN.1 pour SHA-256 (RFC 8017 §9.2).
static const uint8_t SHA256_DI[] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
};

// m = sig^e mod n, écrit sur k octets (k = taille du module). 0 si OK.
static int rsa_pubop(const bn_t *n, const uint8_t *e, int elen,
                     const uint8_t *sig, int siglen, uint8_t *em, int k) {
    bn_t s, r;
    if (bn_from_be(&s, sig, siglen) != 0) return -1;
    if (bn_cmp(&s, n) >= 0) return -1;            // signature hors domaine
    bn_modexp(&r, &s, e, elen, n);
    bn_to_be(&r, em, k);
    return 0;
}

int rsa_verify_pkcs1_sha256(const bn_t *n, const uint8_t *e, int elen,
                            const uint8_t *sig, int siglen, const uint8_t hash[32]) {
    int k = siglen;
    if (k < 3 + (int)sizeof(SHA256_DI) + HLEN || k > 600) return 0;
    uint8_t em[600];
    if (rsa_pubop(n, e, elen, sig, siglen, em, k) != 0) return 0;
    // EM = 0x00 0x01 PS(0xFF..) 0x00 DigestInfo Hash
    if (em[0] != 0x00 || em[1] != 0x01) return 0;
    int i = 2;
    while (i < k && em[i] == 0xFF) i++;
    int pslen = i - 2;
    if (pslen < 8) return 0;                        // PS doit faire >= 8 octets
    if (i >= k || em[i] != 0x00) return 0;
    i++;
    if (k - i != (int)sizeof(SHA256_DI) + HLEN) return 0;
    for (int j = 0; j < (int)sizeof(SHA256_DI); j++) if (em[i + j] != SHA256_DI[j]) return 0;
    const uint8_t *h = em + i + sizeof(SHA256_DI);
    int diff = 0; for (int j = 0; j < HLEN; j++) diff |= h[j] ^ hash[j];
    return diff == 0;
}

// MGF1 avec SHA-256.
static void mgf1(const uint8_t *seed, int seedlen, uint8_t *mask, int masklen) {
    uint8_t cnt[4]; uint8_t blk[32];
    int o = 0;
    for (uint32_t c = 0; o < masklen; c++) {
        cnt[0]=c>>24; cnt[1]=c>>16; cnt[2]=c>>8; cnt[3]=c;
        sha256_ctx ctx; sha256_init(&ctx);
        sha256_update(&ctx, seed, seedlen);
        sha256_update(&ctx, cnt, 4);
        sha256_final(&ctx, blk);
        int n = masklen - o; if (n > 32) n = 32;
        for (int j = 0; j < n; j++) mask[o + j] = blk[j];
        o += n;
    }
}

int rsa_verify_pss_sha256(const bn_t *n, const uint8_t *e, int elen,
                          const uint8_t *sig, int siglen, const uint8_t hash[32]) {
    // emBits/emLen dérivés de la VRAIE taille du module (RFC 8017 §8.1.2).
    int modBits = bn_bitlen(n);
    int emBits = modBits - 1;
    int emLen = (emBits + 7) / 8;
    int topclear = 8 * emLen - emBits;             // bits de tête à forcer à 0
    int sLen = HLEN;                               // salt = hLen (rsa_pss_rsae_sha256)
    if (emLen < HLEN + sLen + 2 || emLen > 600) return 0;

    uint8_t em[600];
    { bn_t s, r;
      if (bn_from_be(&s, sig, siglen) != 0) return 0;
      if (bn_cmp(&s, n) >= 0) return 0;
      bn_modexp(&r, &s, e, elen, n);
      bn_to_be(&r, em, emLen); }

    if (em[emLen - 1] != 0xbc) return 0;
    int dblen = emLen - HLEN - 1;
    const uint8_t *maskedDB = em;
    const uint8_t *H = em + dblen;

    // Les 'topclear' bits de poids fort de maskedDB doivent être nuls.
    if (topclear > 0 && (em[0] >> (8 - topclear)) != 0) return 0;

    uint8_t dbmask[600];
    mgf1(H, HLEN, dbmask, dblen);
    uint8_t db[600];
    for (int i = 0; i < dblen; i++) db[i] = maskedDB[i] ^ dbmask[i];
    db[0] &= (uint8_t)(0xFF >> topclear);          // efface les bits forcés à 0

    // DB = PS(0x00..) || 0x01 || salt
    int i = 0; while (i < dblen - sLen - 1 && db[i] == 0) i++;
    if (i != dblen - sLen - 1 || db[i] != 0x01) return 0;
    const uint8_t *salt = db + dblen - sLen;

    // M' = (0x00)*8 || mHash || salt ; H' = SHA256(M') ; comparer à H.
    uint8_t hp[32];
    sha256_ctx ctx; sha256_init(&ctx);
    uint8_t z8[8] = {0,0,0,0,0,0,0,0};
    sha256_update(&ctx, z8, 8);
    sha256_update(&ctx, hash, HLEN);
    sha256_update(&ctx, salt, sLen);
    sha256_final(&ctx, hp);
    int diff = 0; for (int j = 0; j < HLEN; j++) diff |= hp[j] ^ H[j];
    return diff == 0;
}
