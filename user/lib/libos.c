// =============================================================================
//  user/lib/libos.c -- "libOS" : implémente en RING 3, via appels système, les
//  services noyau dont dépend le bureau (framebuffer, entrées, tas, horloge…),
//  pour que desktop.c / wm.c / app_*.c se compilent quasiment sans modification.
// -----------------------------------------------------------------------------
//  Les modules portables du noyau (gfx.c, klib.c, vfs.c, users.c) sont compilés
//  tels quels dans le bureau ; le VFS et les comptes vivent donc DANS le
//  processus bureau (le noyau garde les siens pour sshd/pacman).
// =============================================================================
#include "sexos.h"
#include "gfx.h"
#include "framebuffer.h"
#include "rtc.h"
#include "net.h"
#include "pkg.h"
#include "heap.h"
#include "pit.h"
#include "ps2.h"
#include "boot.h"
#include "proc.h"
#include "klib.h"

// --- Allocateur (liste libre simple sur une arène mmap) ----------------------
typedef struct blk { unsigned long size; int free; struct blk *next; } blk_t;
static blk_t *heap_head;
#define ARENA (28u * 1024 * 1024)

static void libos_heap_init(void) {
    blk_t *h = (blk_t *)sys_alloc(ARENA);
    h->size = ARENA - sizeof(blk_t); h->free = 1; h->next = 0;
    heap_head = h;
}
void *kmalloc(size_t n) {
    n = (n + 15) & ~15UL;
    for (blk_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= n) {
            if (b->size >= n + sizeof(blk_t) + 16) {
                blk_t *nb = (blk_t *)((char *)(b + 1) + n);
                nb->size = b->size - n - sizeof(blk_t); nb->free = 1; nb->next = b->next;
                b->next = nb; b->size = n;
            }
            b->free = 0;
            return b + 1;
        }
    }
    return 0;
}
void kfree(void *p) {
    if (!p) return;
    blk_t *b = (blk_t *)p - 1; b->free = 1;
    if (b->next && b->next->free) { b->size += sizeof(blk_t) + b->next->size; b->next = b->next->next; }
}
void *kcalloc(size_t a, size_t b) {
    size_t n = a * b; void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}
void *krealloc(void *p, size_t n) {
    if (!p) return kmalloc(n);
    blk_t *b = (blk_t *)p - 1;
    if (b->size >= n) return p;
    void *q = kmalloc(n);
    if (q) { memcpy(q, p, b->size); kfree(p); }
    return q;
}

// --- Sortie série (klib.c : kprintf) -> sys_write ----------------------------
void serial_putc(char c) { char b = c; sys_write(&b, 1); }
void serial_write(const char *s) { unsigned long n = 0; while (s[n]) n++; sys_write(s, n); }
// panic() est fourni par klib.c (compilé dans le bureau).

// --- Framebuffer -------------------------------------------------------------
static fbinfo_t g_fb;
static canvas_t g_canvas;
void libos_init(void) {
    libos_heap_init();
    sys_fb_map(&g_fb);
    g_canvas.pixels = (uint32_t *)(uintptr_t)g_fb.addr;
    g_canvas.width  = g_fb.width;
    g_canvas.height = g_fb.height;
    g_canvas.pitch  = g_fb.pitch;
}
canvas_t *fb_canvas(void) { return &g_canvas; }
uint32_t  fb_width(void)  { return g_fb.width; }
uint32_t  fb_height(void) { return g_fb.height; }
uint32_t  fb_pitch(void)  { return g_fb.pitch; }
uint64_t  fb_phys(void)   { return 0; }
uint32_t  fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;   // 0xRRGGBB (QEMU)
}

// --- Entrées -----------------------------------------------------------------
bool input_poll(event_t *out) { return sys_input_poll(out) != 0; }
void input_init(void) {}
void input_push(const event_t *e) { (void)e; }
static int32_t mx_cache, my_cache;
void    ps2_mouse_pos(int32_t *x, int32_t *y) { if (x) *x = g_fb.width / 2; if (y) *y = g_fb.height / 2; (void)mx_cache; (void)my_cache; }
uint8_t ps2_mouse_buttons(void) { return 0; }

// --- Horloges ----------------------------------------------------------------
uint64_t pit_ms(void)    { return sys_time_ms(); }
uint64_t pit_ticks(void) { return sys_time_ms(); }
void     pit_sleep_ms(uint32_t ms) { uint64_t t = sys_time_ms() + ms; while (sys_time_ms() < t) {} }
void     rtc_now(rtc_time_t *out) { if (out) _sc1(SYS_rtc_now, (long)out); }
void     rtc_format(const rtc_time_t *t, char *buf) {
    // "AAAA-MM-JJ HH:MM:SS"
    static const char *d = "0123456789";
    int i = 0;
    buf[i++] = d[(t->year / 1000) % 10]; buf[i++] = d[(t->year / 100) % 10];
    buf[i++] = d[(t->year / 10) % 10];   buf[i++] = d[t->year % 10]; buf[i++] = '-';
    buf[i++] = d[t->month / 10]; buf[i++] = d[t->month % 10]; buf[i++] = '-';
    buf[i++] = d[t->day / 10];   buf[i++] = d[t->day % 10];   buf[i++] = ' ';
    buf[i++] = d[t->hour / 10];  buf[i++] = d[t->hour % 10];  buf[i++] = ':';
    buf[i++] = d[t->minute / 10];buf[i++] = d[t->minute % 10];buf[i++] = ':';
    buf[i++] = d[t->second / 10];buf[i++] = d[t->second % 10];buf[i] = 0;
}

// --- Infos système (Paramètres) ----------------------------------------------
static sysinfo_t si(void) { sysinfo_t s; _sc1(SYS_sysinfo, (long)&s); return s; }
uint64_t pmm_total_bytes(void) { return (uint64_t)si().mem_total_mb * 1024 * 1024; }
uint64_t pmm_used_bytes(void)  { return (uint64_t)si().mem_used_mb * 1024 * 1024; }
int      pci_device_count(void){ return (int)si().pci_count; }

// --- Réseau / SSH / paquets : indisponibles dans le bureau ring 3 ------------
netif_t netif;                                   // up=0 -> "reseau indisponible"
void ip_to_str(ip4_t ip, char *buf) {
    static const char *d = "0123456789"; int i = 0;
    for (int s = 24; s >= 0; s -= 8) {
        unsigned v = (ip >> s) & 0xFF;
        if (v >= 100) buf[i++] = d[v / 100];
        if (v >= 10)  buf[i++] = d[(v / 10) % 10];
        buf[i++] = d[v % 10];
        if (s) buf[i++] = '.';
    }
    buf[i] = 0;
}
bool net_ping(ip4_t dst, uint32_t *rtt) { (void)dst; (void)rtt; return false; }
bool dns_resolve(const char *n, ip4_t *o) { (void)n; (void)o; return false; }
int  http_get(const char *h, const char *p, char *b, int l) { (void)h;(void)p;(void)b;(void)l; return -1; }
int  ssh_client_exec(ip4_t ip, uint16_t port, const char *u, const char *pw,
                     const char *cmd, char *out, int outmax) {
    (void)ip;(void)port;(void)u;(void)pw;(void)cmd;(void)out;(void)outmax; return -1;
}

static pkg_out_t pkg_out;
void pkg_set_output(pkg_out_t fn) { pkg_out = fn; }
void pkg_set_repo(uint32_t ip, uint16_t port) { (void)ip; (void)port; }
static int pkg_unavail(void) { if (pkg_out) pkg_out("pacman : indisponible dans le bureau ring 3\n"); return -1; }
int pkg_sync(void)                 { return pkg_unavail(); }
int pkg_install(const char *n)     { (void)n; return pkg_unavail(); }
int pkg_remove(const char *n)      { (void)n; return pkg_unavail(); }
int pkg_query(void)                { return pkg_unavail(); }
int pkg_upgrade(void)              { return pkg_unavail(); }

// --- Divers ------------------------------------------------------------------
void  sshd_poll(void) {}
struct limine_framebuffer *boot_framebuffer(void) { return 0; }   // pas de modes GOP en ring 3
void *boot_module(const char *name, uint64_t *size) { (void)name; if (size) *size = 0; return 0; }
int   proc_run(const void *e, size_t l, int a, const char **v) { (void)e;(void)l;(void)a;(void)v; return -1; }
void  proc_set_output(void (*fn)(const char *, int)) { (void)fn; }
