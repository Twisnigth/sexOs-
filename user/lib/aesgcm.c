// =============================================================================
//  user/lib/aesgcm.c -- AES-128 (FIPS-197) + GCM (SP 800-38D)
// =============================================================================
#include "aesgcm.h"

void *memcpy(void *, const void *, unsigned long);

// --- AES-128 -----------------------------------------------------------------
static const uint8_t SBOX[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

typedef struct { uint8_t rk[11][16]; } aes_ctx;

static void aes128_init(aes_ctx *c, const uint8_t key[16]) {
    memcpy(c->rk[0], key, 16);
    uint8_t rcon = 1;
    for (int r = 1; r <= 10; r++) {
        uint8_t *prev = c->rk[r-1], *cur = c->rk[r];
        uint8_t t[4] = { SBOX[prev[13]], SBOX[prev[14]], SBOX[prev[15]], SBOX[prev[12]] };
        t[0] ^= rcon; rcon = xtime(rcon);
        for (int i = 0; i < 4; i++) cur[i] = prev[i] ^ t[i];
        for (int i = 4; i < 16; i++) cur[i] = cur[i-4] ^ prev[i];
    }
}

static void aes128_encrypt(const aes_ctx *c, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16]; for (int i = 0; i < 16; i++) s[i] = in[i] ^ c->rk[0][i];
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = SBOX[s[i]];          // SubBytes
        // ShiftRows (état en colonnes : octet i = colonne i/4, ligne i%4)
        uint8_t a[16];
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++)
                a[col*4+row] = t[((col+row)%4)*4 + row];
        if (r != 10) {
            for (int col = 0; col < 4; col++) {                  // MixColumns
                uint8_t *p = a + col*4;
                uint8_t b0=p[0],b1=p[1],b2=p[2],b3=p[3];
                p[0]=(uint8_t)(xtime(b0)^(xtime(b1)^b1)^b2^b3);
                p[1]=(uint8_t)(b0^xtime(b1)^(xtime(b2)^b2)^b3);
                p[2]=(uint8_t)(b0^b1^xtime(b2)^(xtime(b3)^b3));
                p[3]=(uint8_t)((xtime(b0)^b0)^b1^b2^xtime(b3));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = a[i] ^ c->rk[r][i];   // AddRoundKey
    }
    memcpy(out, s, 16);
}

// --- GHASH (multiplication dans GF(2^128)) -----------------------------------
static void gf_mul(uint8_t *X, const uint8_t *Y) {               // X = X · Y
    uint8_t Z[16] = {0}, V[16];
    memcpy(V, X, 16);
    for (int i = 0; i < 128; i++) {
        if ((Y[i >> 3] >> (7 - (i & 7))) & 1) for (int j = 0; j < 16; j++) Z[j] ^= V[j];
        int lsb = V[15] & 1;
        for (int j = 15; j > 0; j--) V[j] = (uint8_t)((V[j] >> 1) | ((V[j-1] & 1) << 7));
        V[0] >>= 1;
        if (lsb) V[0] ^= 0xe1;
    }
    memcpy(X, Z, 16);
}

static void ghash_blocks(uint8_t acc[16], const uint8_t H[16], const uint8_t *data, int len) {
    for (int o = 0; o < len; o += 16) {
        uint8_t blk[16]; for (int j = 0; j < 16; j++) blk[j] = (o + j < len) ? data[o + j] : 0;
        for (int j = 0; j < 16; j++) acc[j] ^= blk[j];
        gf_mul(acc, H);
    }
}

static void inc32(uint8_t ctr[16]) {
    for (int i = 15; i >= 12; i--) { if (++ctr[i]) break; }
}

static void gctr(const aes_ctx *c, const uint8_t icb[16], const uint8_t *in, int len, uint8_t *out) {
    uint8_t ctr[16]; memcpy(ctr, icb, 16);
    for (int o = 0; o < len; o += 16) {
        uint8_t ks[16]; aes128_encrypt(c, ctr, ks);
        int n = len - o; if (n > 16) n = 16;
        for (int j = 0; j < n; j++) out[o + j] = in[o + j] ^ ks[j];
        inc32(ctr);
    }
}

static void gcm_core(const aes_ctx *c, const uint8_t iv[12], const uint8_t *aad, int aadlen,
                     const uint8_t *src, int len, uint8_t *dst, uint8_t tag[16]) {
    uint8_t H[16] = {0}; aes128_encrypt(c, H, H);
    uint8_t J0[16]; memcpy(J0, iv, 12); J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=1;
    uint8_t ctr0[16]; memcpy(ctr0, J0, 16); inc32(ctr0);
    gctr(c, ctr0, src, len, dst);                                // chiffre/déchiffre
    // GHASH(aad || pad || ct || pad || len(aad)||len(ct) en bits)
    uint8_t S[16] = {0};
    ghash_blocks(S, H, aad, aadlen);
    ghash_blocks(S, H, dst, len);                                // 'dst' = ciphertext
    uint8_t lb[16] = {0};
    uint64_t abits = (uint64_t)aadlen * 8, cbits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) { lb[7-i] = (uint8_t)(abits >> (8*i)); lb[15-i] = (uint8_t)(cbits >> (8*i)); }
    for (int j = 0; j < 16; j++) S[j] ^= lb[j];
    gf_mul(S, H);
    uint8_t ej0[16]; aes128_encrypt(c, J0, ej0);
    for (int j = 0; j < 16; j++) tag[j] = S[j] ^ ej0[j];
}

void aes128gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t *aad, int aadlen,
                    const uint8_t *pt, int ptlen, uint8_t *ct, uint8_t tag[16]) {
    aes_ctx c; aes128_init(&c, key);
    gcm_core(&c, iv, aad, aadlen, pt, ptlen, ct, tag);
}

int aes128gcm_open(const uint8_t key[16], const uint8_t iv[12],
                   const uint8_t *aad, int aadlen,
                   const uint8_t *ct, int ctlen, const uint8_t tag[16], uint8_t *pt) {
    aes_ctx c; aes128_init(&c, key);
    // Recalcule le tag sur le ciphertext (GHASH) avant de déchiffrer.
    uint8_t H[16] = {0}; aes128_encrypt(&c, H, H);
    uint8_t J0[16]; memcpy(J0, iv, 12); J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=1;
    uint8_t S[16] = {0};
    ghash_blocks(S, H, aad, aadlen);
    ghash_blocks(S, H, ct, ctlen);
    uint8_t lb[16] = {0};
    uint64_t abits = (uint64_t)aadlen * 8, cbits = (uint64_t)ctlen * 8;
    for (int i = 0; i < 8; i++) { lb[7-i] = (uint8_t)(abits >> (8*i)); lb[15-i] = (uint8_t)(cbits >> (8*i)); }
    for (int j = 0; j < 16; j++) S[j] ^= lb[j];
    gf_mul(S, H);
    uint8_t ej0[16]; aes128_encrypt(&c, J0, ej0);
    uint8_t exp[16]; for (int j = 0; j < 16; j++) exp[j] = S[j] ^ ej0[j];
    int diff = 0; for (int j = 0; j < 16; j++) diff |= exp[j] ^ tag[j];
    if (diff) return -1;
    uint8_t ctr0[16]; memcpy(ctr0, J0, 16); inc32(ctr0);
    gctr(&c, ctr0, ct, ctlen, pt);
    return 0;
}
