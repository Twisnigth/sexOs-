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
#define SYS_yield       0x204      // cède le CPU (commutation coopérative)
#define SYS_pid_alive   0x205      // pid_alive(pid) -> 1 si la tâche vit, 0 sinon
// (exit/getpid réutilisent les numéros Linux 60/39)

// --- IPC : messagerie + mémoire partagée -------------------------------------
#define SYS_ipc_send    0x220      // ipc_send(dest_pid, buf, len)
#define SYS_ipc_recv    0x221      // ipc_recv(buf, maxlen, *sender) -> len ou -1 (non bloquant)
#define SYS_ipc_wait    0x226      // bloque jusqu'à l'arrivée d'un message
#define SYS_shm_create  0x222      // shm_create(size, *out_va) -> shm_id
#define SYS_shm_map     0x223      // shm_map(id, *out_va) -> 0/-1
#define SYS_comp_register 0x224    // s'enregistre comme compositeur
#define SYS_comp_pid    0x225      // pid du compositeur (0 si aucun)

// --- Framebuffer (réservé au compositeur) ------------------------------------
#define SYS_fb_map      0x210      // mappe le framebuffer ; remplit struct fbinfo

// --- Entrées -----------------------------------------------------------------
#define SYS_input_poll  0x212      // lit le prochain événement (non bloquant)

// --- Horloges ----------------------------------------------------------------
#define SYS_time_ms     0x250      // millisecondes depuis le démarrage
#define SYS_rtc_now     0x251      // remplit rtc_time_t (date/heure CMOS)
#define SYS_sysinfo     0x252      // remplit sysinfo_t (mémoire, uptime, PCI)
#define SYS_proc_list   0x253      // (index, *procinfo) -> 1=rempli / 0=au-delà

// --- Système -----------------------------------------------------------------
#define SYS_reboot      0x260      // redémarre la machine

// --- Système de fichiers (par chemin ; agit sur le VFS du noyau) -------------
#define SYS_vfs_list    0x230      // (path, index, *dirent) -> 1=entrée / 0=fin / -1=erreur
#define SYS_vfs_read    0x231      // (vfs_io*) -> octets lus
#define SYS_vfs_write   0x232      // (vfs_io*) -> octets écrits, -1 si interdit
#define SYS_vfs_create  0x233      // (path, type) -> 0/-1
#define SYS_vfs_delete  0x234      // (path) -> 0/-1
#define SYS_vfs_stat    0x236      // (path, *dirent) -> 0=ok / -1=absent

// --- Comptes / permissions ---------------------------------------------------
#define SYS_whoami      0x240      // (*userinfo)
#define SYS_can_write   0x241      // (path) -> 1/0

typedef struct { char name[64]; uint32_t type; uint64_t size; } dirent_t;  // type: 0=fichier 1=dossier
typedef struct { const char *path; uint64_t off; void *buf; uint64_t len; } vfs_io_t;
typedef struct { char name[32]; char home[96]; int is_admin; } userinfo_t;

// Informations du framebuffer renvoyées par SYS_fb_map.
typedef struct {
    uint64_t addr;                 // adresse virtuelle (espace appelant)
    uint32_t width, height, pitch; // dimensions, octets par ligne
} fbinfo_t;

// Informations système renvoyées par SYS_sysinfo (pour l'appli Paramètres).
typedef struct {
    uint32_t mem_total_mb, mem_used_mb, uptime_s, pci_count;
} sysinfo_t;

// Une entrée de la table des processus (SYS_proc_list), pour le moniteur d'activité.
//  state : 1=prêt 2=actif 3=bloqué 4=zombie (cf. task_state_t côté noyau).
typedef struct {
    int      pid;
    int      state;
    uint64_t cpu_ticks;        // tops du minuteur cumulés sur cette tâche
    uint64_t mem_kb;           // mémoire résidente approximative (Kio)
    char     name[32];
} procinfo_t;

#endif
