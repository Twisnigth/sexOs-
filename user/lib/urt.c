// =============================================================================
//  user/lib/urt.c -- Mini-runtime pour les programmes ring 3 (compositeur/apps)
//  Fournit ce que gcc -ffreestanding attend (memset/memcpy) + quelques utilitaires.
// =============================================================================
#include "sexos.h"

void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d;
}
unsigned long strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) break;
    }
    return 0;
}
char *strcat(char *d, const char *s) {
    char *r = d; while (*d) d++; while ((*d++ = *s++)) {} return r;
}
void utoa(unsigned long v, char *out) {
    char t[24]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (i) out[j++] = t[--i];
    out[j] = 0;
}
void *memmove(void *d, const void *s, unsigned long n) {
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) { while (n--) *a++ = *b++; }
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}

void serial_putc(char c) { char b = c; sys_write(&b, 1); }
void serial_write(const char *s) { sys_write(s, strlen(s)); }

// --- Tas pour les applications (liste libre simple sur une arène mmap) --------
//  Beaucoup d'applis se contentent de tampons statiques, mais les décodeurs
//  d'images (PNG/JPEG) ont besoin d'allouer du temporaire ; on fournit donc un
//  malloc/free minimal commun, identique à celui de la libOS du bureau.
typedef struct ublk { unsigned long size; int free; struct ublk *next; } ublk_t;
#define UARENA (32u * 1024 * 1024)
static ublk_t *uheap;

static void uheap_init(void) {
    uheap = (ublk_t *)sys_alloc(UARENA);
    if (!uheap) return;
    uheap->size = UARENA - sizeof(ublk_t); uheap->free = 1; uheap->next = 0;
}

void *malloc(unsigned long n) {
    if (!uheap) uheap_init();
    if (!uheap) return 0;
    n = (n + 15) & ~15ul;
    for (ublk_t *b = uheap; b; b = b->next) {
        if (b->free && b->size >= n) {
            if (b->size >= n + sizeof(ublk_t) + 16) {
                ublk_t *nb = (ublk_t *)((unsigned char *)(b + 1) + n);
                nb->size = b->size - n - sizeof(ublk_t); nb->free = 1; nb->next = b->next;
                b->size = n; b->next = nb;
            }
            b->free = 0;
            return b + 1;
        }
    }
    return 0;
}

void free(void *p) {
    if (!p) return;
    ublk_t *b = (ublk_t *)p - 1; b->free = 1;
    if (b->next && b->next->free) { b->size += sizeof(ublk_t) + b->next->size; b->next = b->next->next; }
}

void *calloc(unsigned long a, unsigned long b) {
    unsigned long n = a * b; void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
