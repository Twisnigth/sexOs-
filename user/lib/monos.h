// =============================================================================
//  user/lib/monos.h -- Petite bibliothèque d'appels système (ring 3 MonOS)
// =============================================================================
#ifndef MONOS_USER_LIB_H
#define MONOS_USER_LIB_H

#include <stdint.h>
#include "input.h"        // event_t, KEY_*, MOUSE_* (via -Ikernel)
#include "syscalls.h"     // fbinfo_t, SYS_* (via -Ikernel)

static inline long _sc0(long n) {
    long r; __asm__ volatile ("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory");
    return r;
}
static inline long _sc1(long n, long a1) {
    long r; __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return r;
}
static inline long _sc3(long n, long a1, long a2, long a3) {
    long r; __asm__ volatile ("syscall" : "=a"(r)
             : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return r;
}

// Allocation : s'appuie sur l'ABI Linux mmap (rsi = taille).
static inline void    *sys_alloc(unsigned long n) { return (void *)_sc3(9, 0, (long)n, 0); }
static inline int      sys_fb_map(fbinfo_t *fi)   { return (int)_sc1(SYS_fb_map, (long)fi); }
static inline int      sys_input_poll(event_t *e) { return (int)_sc1(SYS_input_poll, (long)e); }
static inline uint64_t sys_time_ms(void)          { return (uint64_t)_sc0(SYS_time_ms); }
static inline int      sys_get_cpl(void)          { return (int)_sc0(SYS_get_cpl); }
static inline void     sys_write(const char *s, unsigned long n) { _sc3(1, 1, (long)s, (long)n); }
static inline void     sys_exit(int code)         { _sc1(60, code); for (;;) {} }
static inline void     sys_reboot(void)           { _sc0(SYS_reboot); for (;;) {} }

#endif
