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

// --- Réduction de Barrett (rapide) -------------------------------------------
//  Remplace la réduction bit-à-bit (~512 itérations) par ~2 multiplications, en
//  mettant en cache mu = floor(b^(2k)/n) (b=2^32, k=limbes de n) pour le module
//  courant. C'est l'opération chaude de toute la crypto (RSA, ECDSA, ECDHE).
#define BW (2 * BN_LIMBS + 4)

static void mul_raw(uint32_t *out, const uint32_t *a, int al, const uint32_t *b, int bl) {
    for (int i = 0; i < al + bl; i++) out[i] = 0;
    for (int i = 0; i < al; i++) {
        if (!a[i]) continue;
        uint64_t c = 0;
        for (int j = 0; j < bl; j++) {
            uint64_t t = (uint64_t)a[i] * b[j] + out[i + j] + c;
            out[i + j] = (uint32_t)t; c = t >> 32;
        }
        out[i + bl] += (uint32_t)c;
    }
}
// q = floor(num / den), tableaux de 'len' limbes (bit-à-bit ; appelé 1x par module).
static void long_div(uint32_t *q, const uint32_t *num, const uint32_t *den, int len) {
    uint32_t rem[BW]; for (int i = 0; i < len; i++) { rem[i] = 0; q[i] = 0; }
    int hb = top_bit(num, len);
    for (int bit = hb; bit >= 0; bit--) {
        uint32_t carry = 0;
        for (int i = 0; i < len; i++) { uint32_t nc = rem[i] >> 31; rem[i] = (rem[i] << 1) | carry; carry = nc; }
        rem[0] |= get_bit(num, bit);
        if (cmp_n(rem, den, len) >= 0) { sub_in_place(rem, den, len); q[bit >> 5] |= (1u << (bit & 31)); }
    }
}

static uint32_t cache_mu[BW];        // floor(b^(2k)/n), k+1 limbes
static bn_t     cache_n; static int cache_k, cache_ok;

static void barrett_setup(const bn_t *n, int k) {
    int len = 2 * k + 2;
    uint32_t num[BW]; for (int i = 0; i < len; i++) num[i] = 0; num[2 * k] = 1;   // b^(2k)
    uint32_t den[BW]; for (int i = 0; i < len; i++) den[i] = (i < BN_LIMBS) ? n->v[i] : 0;
    long_div(cache_mu, num, den, len);
    cache_n = *n; cache_k = k; cache_ok = 1;
}

// r = p mod n  (p : 2*BN_LIMBS limbes < n^2), n a k limbes. Barrett (HAC 14.42).
static void barrett_reduce(bn_t *r, const uint32_t *p, const bn_t *n, int k) {
    int kp1 = k + 1;
    uint32_t q1[BW]; for (int i = 0; i <= k; i++) q1[i] = p[(k - 1) + i];      // p >> b^(k-1)
    uint32_t q2[BW]; mul_raw(q2, q1, k + 1, cache_mu, k + 1);                  // q1 * mu
    uint32_t q3[BW]; for (int i = 0; i <= k; i++) q3[i] = q2[(k + 1) + i];     // q2 >> b^(k+1)
    uint32_t qn[BW]; mul_raw(qn, q3, k + 1, n->v, k);                          // q3 * n
    uint32_t rr[BW]; uint64_t borrow = 0;                                      // r1 - r2 mod b^(k+1)
    for (int i = 0; i < kp1; i++) {
        uint64_t t = (uint64_t)p[i] - qn[i] - borrow;
        rr[i] = (uint32_t)t; borrow = (t >> 63) & 1;
    }
    uint32_t nn[BW]; for (int i = 0; i < kp1; i++) nn[i] = (i < k) ? n->v[i] : 0;
    for (int t = 0; t < 3 && cmp_n(rr, nn, kp1) >= 0; t++) sub_in_place(rr, nn, kp1);
    bn_zero(r); for (int i = 0; i < k && i < BN_LIMBS; i++) r->v[i] = rr[i];
}

// r = a * b mod n
void bn_modmul(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *n) {
    uint32_t p[2 * BN_LIMBS];
    bn_mul(p, a, b);
    int nb = top_bit(n->v, BN_LIMBS);
    if (nb < 0) { bn_zero(r); return; }
    int k = nb / 32 + 1;
    if (k < 2 || 2 * k + 2 > BW) { bn_mod_2n(r, p, n); return; }   // repli (très grand module)
    if (!cache_ok || cache_k != k || bn_cmp(n, &cache_n) != 0) barrett_setup(n, k);
    barrett_reduce(r, p, n, k);
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
