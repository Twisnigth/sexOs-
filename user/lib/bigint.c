// =============================================================================
//  user/lib/bigint.c -- Entiers multi-précision (vérification RSA)
// =============================================================================
#include "bigint.h"

static int top_bit(const uint32_t *v, int len);

void bn_zero(bn_t *a) { for (int i = 0; i < BN_LIMBS; i++) a->v[i] = 0; }

int bn_from_be(bn_t *a, const uint8_t *b, int len) {
    bn_zero(a);
    // ignore les zéros de tête
    int i = 0; while (i < len && b[i] == 0) i++;
    int n = len - i;
    if (n > BN_LIMBS * 4) return -1;
    for (int k = 0; k < n; k++) {
        uint8_t byte = b[len - 1 - k];
        a->v[k / 4] |= (uint32_t)byte << (8 * (k % 4));
    }
    return 0;
}

int bn_cmp(const bn_t *a, const bn_t *b) {
    for (int i = BN_LIMBS - 1; i >= 0; i--) {
        if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1;
    }
    return 0;
}

void bn_to_be(const bn_t *a, uint8_t *out, int len) {
    for (int k = 0; k < len; k++) {
        int byte = len - 1 - k;                  // position dans out (gros-boutiste)
        out[byte] = (k < BN_LIMBS * 4) ? (uint8_t)(a->v[k / 4] >> (8 * (k % 4))) : 0;
    }
}

int bn_bytelen(const bn_t *a) {
    for (int k = BN_LIMBS * 4 - 1; k >= 0; k--)
        if ((uint8_t)(a->v[k / 4] >> (8 * (k % 4)))) return k + 1;
    return 0;
}

int bn_bitlen(const bn_t *a) { return top_bit(a->v, BN_LIMBS) + 1; }

// Indice du bit de poids fort (ou -1 si nul). Travaille sur 'len' limbes.
static int top_bit(const uint32_t *v, int len) {
    for (int i = len - 1; i >= 0; i--) {
        if (v[i]) {
            uint32_t x = v[i]; int b = 31;
            while (!(x & 0x80000000u)) { x <<= 1; b--; }
            return i * 32 + b;
        }
    }
    return -1;
}

static int get_bit(const uint32_t *v, int bit) { return (v[bit >> 5] >> (bit & 31)) & 1; }

// r -= m (suppose r >= m), tableaux de 'len' limbes.
static void sub_in_place(uint32_t *r, const uint32_t *m, int len) {
    uint64_t borrow = 0;
    for (int i = 0; i < len; i++) {
        uint64_t t = (uint64_t)r[i] - m[i] - borrow;
        r[i] = (uint32_t)t;
        borrow = (t >> 63) & 1;
    }
}

// compare deux tableaux de 'len' limbes : -1/0/1
static int cmp_n(const uint32_t *a, const uint32_t *b, int len) {
    for (int i = len - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

// r = a mod n.  a : 2*BN_LIMBS limbes ; n,r : BN_LIMBS limbes.
//  Division binaire (un bit à la fois), bornée au bit de poids fort de a.
//  La largeur de travail W est calée sur la taille du module (le reste reste
//  < n) : un module 256 bits n'entraîne pas de calcul sur 4096 bits (ECDSA).
static void bn_mod_2n(bn_t *r, const uint32_t *a, const bn_t *n) {
    uint32_t rem[BN_LIMBS + 1];
    for (int i = 0; i <= BN_LIMBS; i++) rem[i] = 0;
    uint32_t nn[BN_LIMBS + 1];
    for (int i = 0; i <= BN_LIMBS; i++) nn[i] = (i < BN_LIMBS) ? n->v[i] : 0;
    int nbits = top_bit(n->v, BN_LIMBS);
    if (nbits < 0) { bn_zero(r); return; }        // module nul -> 0
    int W = nbits / 32 + 2; if (W > BN_LIMBS + 1) W = BN_LIMBS + 1;
    int hb = top_bit(a, 2 * BN_LIMBS);
    if (hb < 0) { bn_zero(r); return; }
    for (int bit = hb; bit >= 0; bit--) {
        uint32_t carry = 0;                       // rem <<= 1
        for (int i = 0; i < W; i++) {
            uint32_t nc = rem[i] >> 31;
            rem[i] = (rem[i] << 1) | carry;
            carry = nc;
        }
        rem[0] |= get_bit(a, bit);
        if (cmp_n(rem, nn, W) >= 0) sub_in_place(rem, nn, W);
    }
    for (int i = 0; i < BN_LIMBS; i++) r->v[i] = rem[i];
}

// p = a * b  (p : 2*BN_LIMBS limbes).
static void bn_mul(uint32_t *p, const bn_t *a, const bn_t *b) {
    for (int i = 0; i < 2 * BN_LIMBS; i++) p[i] = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        if (!a->v[i]) continue;
        uint64_t carry = 0;
        for (int j = 0; j < BN_LIMBS; j++) {
            uint64_t t = (uint64_t)a->v[i] * b->v[j] + p[i + j] + carry;
            p[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        p[i + BN_LIMBS] += (uint32_t)carry;
    }
}

// r = a * b mod n
void bn_modmul(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *n) {
    uint32_t p[2 * BN_LIMBS];
    bn_mul(p, a, b);
    bn_mod_2n(r, p, n);
}

void bn_modexp(bn_t *out, const bn_t *base, const uint8_t *exp, int elen, const bn_t *mod) {
    bn_t result, b = *base;
    bn_zero(&result); result.v[0] = 1;
    // réduit la base au cas où base >= mod
    { uint32_t tmp[2 * BN_LIMBS]; for (int i = 0; i < BN_LIMBS; i++) { tmp[i] = b.v[i]; tmp[i + BN_LIMBS] = 0; } bn_mod_2n(&b, tmp, mod); }
    // parcourt les bits de l'exposant du plus significatif au moins significatif
    int started = 0;
    for (int i = 0; i < elen; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            if (started) bn_modmul(&result, &result, &result, mod);   // carré
            int e = (exp[i] >> bit) & 1;
            if (e) {
                if (!started) started = 1;       // premier bit à 1 : result reste = base au tour suivant
                bn_modmul(&result, &result, &b, mod);
            }
        }
    }
    *out = result;
}
