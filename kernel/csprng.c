// =============================================================================
//  kernel/csprng.c -- Générateur pseudo-aléatoire cryptographique
// -----------------------------------------------------------------------------
//  Graine : RDSEED/RDRAND si disponibles, mélangés avec le minuteur et l'horloge.
//  Génération : flux ChaCha20 (Monocypher) ; ré-injection de RDRAND à chaque appel.
// =============================================================================
#include "crypto.h"
#include "klib.h"
#include "pit.h"
#include "rtc.h"
#include "io.h"

static uint8_t key[32];
static uint8_t nonce[8];
static uint64_t counter;
static int has_rdrand, has_rdseed;

// CPUID pour détecter RDRAND (ECX bit 30) et RDSEED (EBX bit 18, feuille 7).
static void detect_features(void) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(1),"c"(0));
    has_rdrand = (c >> 30) & 1;
    __asm__ volatile ("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(7),"c"(0));
    has_rdseed = (b >> 18) & 1;
}

static int rdrand64(uint64_t *out) {
    if (!has_rdrand) return 0;
    unsigned char ok;
    __asm__ volatile ("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
    return ok;
}
static int rdseed64(uint64_t *out) {
    if (!has_rdseed) return 0;
    unsigned char ok;
    __asm__ volatile ("rdseed %0; setc %1" : "=r"(*out), "=qm"(ok));
    return ok;
}

// Mélange 8 octets d'entropie dans la graine (via SHA-256 du tampon courant).
static void mix(uint64_t v) {
    uint8_t buf[40];
    memcpy(buf, key, 32);
    for (int i = 0; i < 8; i++) buf[32 + i] = v >> (i * 8);
    sha256(buf, sizeof(buf), key);
}

void csprng_init(void) {
    detect_features();
    memset(key, 0, 32);
    // Sources matérielles.
    uint64_t r;
    for (int i = 0; i < 16; i++) { if (rdseed64(&r) || rdrand64(&r)) mix(r); }
    // Sources faibles complémentaires (minuteur + horloge).
    mix(pit_ticks() ^ ((uint64_t)inb(0x40) << 32));
    rtc_time_t t; rtc_now(&t);
    mix(((uint64_t)t.second << 48) | ((uint64_t)t.minute << 40) |
        ((uint64_t)t.hour << 32) | t.year);
    counter = 0;
    memset(nonce, 0, 8);
    kprintf("[csprng] initialise (rdrand=%d rdseed=%d)\n", has_rdrand, has_rdseed);
}

void csprng_bytes(void *buf, size_t len) {
    // Ré-injecte de l'entropie matérielle si possible.
    uint64_t r;
    if (rdseed64(&r) || rdrand64(&r)) mix(r);

    // Génère le flux ChaCha20 (chiffrement de zéros).
    memset(buf, 0, len);
    counter = crypto_chacha20_djb((uint8_t *)buf, (const uint8_t *)buf, len,
                                  key, nonce, counter);
    // Renouvelle la clé pour la confidentialité persistante (forward secrecy).
    uint8_t newkey[32];
    memset(newkey, 0, 32);
    crypto_chacha20_djb(newkey, newkey, 32, key, nonce, counter);
    memcpy(key, newkey, 32);
    counter = 0;
}
