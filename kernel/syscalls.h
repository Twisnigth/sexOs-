// =============================================================================
//  kernel/syscalls.h -- Appels système natifs MonOS (partagé noyau/userspace)
// -----------------------------------------------------------------------------
//  Les numéros >= 0x200 sont propres à MonOS et cohabitent avec l'ABI Linux
//  (numéros 0..~300) utilisée par les binaires busybox/musl.
// =============================================================================
#ifndef MONOS_SYSCALLS_H
#define MONOS_SYSCALLS_H

#include <stdint.h>

// --- Processus ---------------------------------------------------------------
#define SYS_get_cpl     0x200      // renvoie le CPL courant (debug : 3 en ring 3)
// (exit/getpid réutilisent les numéros Linux 60/39)

// --- Framebuffer (réservé au compositeur) ------------------------------------
#define SYS_fb_map      0x210      // mappe le framebuffer ; remplit struct fbinfo

// --- Entrées -----------------------------------------------------------------
#define SYS_input_poll  0x212      // lit le prochain événement (non bloquant)

// --- Horloges ----------------------------------------------------------------
#define SYS_time_ms     0x250      // millisecondes depuis le démarrage
#define SYS_rtc_now     0x251      // remplit rtc_time_t (date/heure CMOS)
#define SYS_sysinfo     0x252      // remplit sysinfo_t (mémoire, uptime, PCI)

// --- Système -----------------------------------------------------------------
#define SYS_reboot      0x260      // redémarre la machine

// Informations du framebuffer renvoyées par SYS_fb_map.
typedef struct {
    uint64_t addr;                 // adresse virtuelle (espace appelant)
    uint32_t width, height, pitch; // dimensions, octets par ligne
} fbinfo_t;

// Informations système renvoyées par SYS_sysinfo (pour l'appli Paramètres).
typedef struct {
    uint32_t mem_total_mb, mem_used_mb, uptime_s, pci_count;
} sysinfo_t;

#endif
