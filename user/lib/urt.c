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
void serial_putc(char c) { char b = c; sys_write(&b, 1); }
void serial_write(const char *s) { sys_write(s, strlen(s)); }
