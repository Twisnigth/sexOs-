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

// Informations du framebuffer renvoyées par SYS_fb_map.
typedef struct {
    uint64_t addr;                 // adresse virtuelle (espace appelant)
    uint32_t width, height, pitch; // dimensions, octets par ligne
} fbinfo_t;

#endif
