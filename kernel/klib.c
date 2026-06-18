// =============================================================================
//  kernel/klib.c -- Implémentation de la bibliothèque de base du noyau
// =============================================================================
#include "klib.h"
#include "serial.h"
#include "io.h"

#include <stdarg.h>

// --- Mémoire -----------------------------------------------------------------
void *memset(void *dst, int value, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)value;
    return dst;
}
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

// --- Chaînes -----------------------------------------------------------------
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}
char *strcpy(char *dst, const char *src) {
    size_t i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return dst;
}
char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}
char *strcat(char *dst, const char *src) {
    size_t d = strlen(dst), i = 0;
    while (src[i]) { dst[d + i] = src[i]; i++; }
    dst[d + i] = 0;
    return dst;
}
char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}
void str_toupper(char *s) {
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32;
}

// --- Conversions -------------------------------------------------------------
int utoa(uint64_t v, char *buf, int base) {
    static const char digits[] = "0123456789abcdef";
    char tmp[65];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = digits[v % base]; v /= base; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
    return j;
}
int itoa(int64_t v, char *buf, int base) {
    if (base == 10 && v < 0) {
        buf[0] = '-';
        return 1 + utoa((uint64_t)(-v), buf + 1, 10);
    }
    return utoa((uint64_t)v, buf, base);
}

// --- printf minimal (vers le port série) -------------------------------------
void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char num[65];
    for (size_t i = 0; fmt[i]; i++) {
        if (fmt[i] != '%') { serial_putc(fmt[i]); continue; }
        i++;
        // Ignore les modificateurs de longueur ('l', 'z').
        while (fmt[i] == 'l' || fmt[i] == 'z') i++;
        switch (fmt[i]) {
            case 's': { const char *s = va_arg(ap, const char *);
                        serial_write(s ? s : "(null)"); break; }
            case 'c': { char c = (char)va_arg(ap, int); serial_putc(c); break; }
            case 'd': { int64_t v = va_arg(ap, int64_t);
                        itoa(v, num, 10); serial_write(num); break; }
            case 'u': { uint64_t v = va_arg(ap, uint64_t);
                        utoa(v, num, 10); serial_write(num); break; }
            case 'x': { uint64_t v = va_arg(ap, uint64_t);
                        utoa(v, num, 16); serial_write(num); break; }
            case 'p': { uint64_t v = (uint64_t)va_arg(ap, void *);
                        serial_write("0x"); utoa(v, num, 16); serial_write(num); break; }
            case '%': serial_putc('%'); break;
            default:  serial_putc('%'); serial_putc(fmt[i]); break;
        }
    }
    va_end(ap);
}

void panic(const char *msg) {
    kprintf("\n*** PANIQUE NOYAU : %s ***\n", msg);
    for (;;) { __asm__ volatile ("cli; hlt"); }
}
