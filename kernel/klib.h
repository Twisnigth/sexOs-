// =============================================================================
//  kernel/klib.h -- Bibliothèque de base du noyau (pas de libc)
// =============================================================================
#ifndef SEXOS_KLIB_H
#define SEXOS_KLIB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// --- Mémoire -----------------------------------------------------------------
void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

// --- Chaînes -----------------------------------------------------------------
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
void   str_toupper(char *s);

// --- Conversions -------------------------------------------------------------
int  itoa(int64_t v, char *buf, int base);    // base 10 ou 16 (signé en base 10)
int  utoa(uint64_t v, char *buf, int base);   // non signé

// --- Sortie formatée ---------------------------------------------------------
//  Affiche sur le port série (utile pour le débogage). Formats : %s %c %d %u
//  %x %p %% et largeur simple via %lu/%lx (les 'l' sont ignorés, tout est 64).
void kprintf(const char *fmt, ...);

// --- Panique -----------------------------------------------------------------
void panic(const char *msg);

#endif
