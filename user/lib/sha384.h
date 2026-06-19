// =============================================================================
//  user/lib/sha384.h -- SHA-384 (FIPS 180-4), pour ECDSA P-384
// =============================================================================
#ifndef MONOS_SHA384_H
#define MONOS_SHA384_H
#include <stdint.h>
#include <stddef.h>
typedef struct { uint64_t h[8], len; uint8_t buf[128]; int n; } sha384_ctx;
void sha384_init(sha384_ctx *c);
void sha384_update(sha384_ctx *c, const void *data, size_t len);
void sha384_final(sha384_ctx *c, uint8_t out[48]);
void sha384(const void *data, size_t len, uint8_t out[48]);
#endif
