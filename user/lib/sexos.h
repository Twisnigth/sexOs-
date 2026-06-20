// =============================================================================
//  user/lib/sexos.h -- Petite bibliothèque d'appels système (ring 3 sexOs)
// =============================================================================
#ifndef SEXOS_USER_LIB_H
#define SEXOS_USER_LIB_H

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
static inline long _sc2(long n, long a1, long a2) {
    long r; __asm__ volatile ("syscall" : "=a"(r)
             : "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
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
static inline int      sys_getpid(void)           { return (int)_sc0(39); }

// --- IPC ---------------------------------------------------------------------
static inline int  sys_ipc_send(int pid, const void *buf, int len) {
    return (int)_sc3(SYS_ipc_send, pid, (long)buf, len);
}
static inline int  sys_ipc_recv(void *buf, int max, int *sender) {
    return (int)_sc3(SYS_ipc_recv, (long)buf, max, (long)sender);
}
static inline long sys_shm_create(unsigned long size, uint64_t *va) {
    return _sc2(SYS_shm_create, (long)size, (long)va);
}
static inline int  sys_shm_map(int id, uint64_t *va) {
    return (int)_sc2(SYS_shm_map, id, (long)va);
}
static inline void sys_comp_register(void) { _sc0(SYS_comp_register); }
static inline int  sys_comp_pid(void)      { return (int)_sc0(SYS_comp_pid); }
static inline void sys_yield(void)         { _sc0(SYS_yield); }
static inline void sys_ipc_wait(void)      { _sc0(SYS_ipc_wait); }
static inline int  sys_pid_alive(int pid)  { return (int)_sc1(SYS_pid_alive, pid); }
static inline int  sys_spawn(int app)      { return (int)_sc1(SYS_spawn, app); }

// --- Système de fichiers / comptes / infos -----------------------------------
static inline int  sys_vfs_list(const char *p, int i, dirent_t *e) { return (int)_sc3(SYS_vfs_list, (long)p, i, (long)e); }
static inline long sys_vfs_read(vfs_io_t *io)   { return _sc1(SYS_vfs_read, (long)io); }
static inline long sys_vfs_write(vfs_io_t *io)  { return _sc1(SYS_vfs_write, (long)io); }
static inline int  sys_vfs_create(const char *p, int type) { return (int)_sc2(SYS_vfs_create, (long)p, type); }
static inline int  sys_vfs_delete(const char *p) { return (int)_sc1(SYS_vfs_delete, (long)p); }
static inline int  sys_vfs_stat(const char *p, dirent_t *e) { return (int)_sc2(SYS_vfs_stat, (long)p, (long)e); }
static inline void sys_whoami(userinfo_t *u)    { _sc1(SYS_whoami, (long)u); }
static inline int  sys_can_write(const char *p) { return (int)_sc1(SYS_can_write, (long)p); }
static inline void sys_rtc(void *t)             { _sc1(SYS_rtc_now, (long)t); }
static inline void sys_sysinfo(void *s)         { _sc1(SYS_sysinfo, (long)s); }
static inline int  sys_proc_list(int i, procinfo_t *p) { return (int)_sc2(SYS_proc_list, i, (long)p); }

// --- Réseau : sockets TCP non bloquantes + DNS -------------------------------
static inline void sys_net_info(netinfo_t *ni)          { _sc1(SYS_net_info, (long)ni); }
static inline int  sys_dns_resolve(const char *n, uint32_t *ip) { return (int)_sc2(SYS_dns_resolve, (long)n, (long)ip); }
static inline int  sys_tcp_open(uint32_t ip, int port)  { return (int)_sc2(SYS_tcp_open, (long)ip, port); }
static inline int  sys_tcp_state(int id)                { return (int)_sc1(SYS_tcp_state, id); }
static inline int  sys_tcp_send(int id, const void *b, int n) { return (int)_sc3(SYS_tcp_send, id, (long)b, n); }
static inline int  sys_tcp_recv(int id, void *b, int n) { return (int)_sc3(SYS_tcp_recv, id, (long)b, n); }
static inline void sys_tcp_close(int id)                { _sc1(SYS_tcp_close, id); }
static inline void sys_random(void *b, unsigned long n) { _sc2(SYS_random, (long)b, (long)n); }

// --- SSH : clé d'hôte + génération de clé (gérées par le noyau) --------------
static inline int sys_ssh_hostkey(char *b, int max) { return (int)_sc2(SYS_ssh_hostkey, (long)b, max); }
static inline int sys_ssh_keygen(char *b, int max)  { return (int)_sc2(SYS_ssh_keygen, (long)b, max); }

#endif
