// =============================================================================
//  kernel/syscalls.h -- Appels système natifs sexOs (partagé noyau/userspace)
// -----------------------------------------------------------------------------
//  Les numéros >= 0x200 sont propres à sexOs et cohabitent avec l'ABI Linux
//  (numéros 0..~300) utilisée par les binaires busybox/musl.
// =============================================================================
#ifndef SEXOS_SYSCALLS_H
#define SEXOS_SYSCALLS_H

#include <stdint.h>

// --- Processus ---------------------------------------------------------------
#define SYS_get_cpl     0x200      // renvoie le CPL courant (debug : 3 en ring 3)
#define SYS_yield       0x204      // cède le CPU (commutation coopérative)
#define SYS_pid_alive   0x205      // pid_alive(pid) -> 1 si la tâche vit, 0 sinon
#define SYS_spawn       0x206      // spawn(app_id) -> pid (>=1) / -1 (réservé compositeur)
// (exit/getpid réutilisent les numéros Linux 60/39)

// Applications lançables à la demande via SYS_spawn (le menu du compositeur les
// lance). L'ordre DOIT correspondre à la table g_apps[] du noyau (kernel/proc.c).
#define APP_TERMINAL    0
#define APP_FILES       1
#define APP_CLOCK       2
#define APP_MONITOR     3
#define APP_WEB         4
#define APP_SETTINGS    5
#define APP_COUNT       6

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

// --- Réseau : sockets TCP NON BLOQUANTES + DNS (pour le navigateur ring 3) ----
#define SYS_net_info    0x270      // (*netinfo_t) -> 0
#define SYS_dns_resolve 0x271      // (name, *ip)  -> 1=résolu / 0=en cours / -1=échec
#define SYS_tcp_open    0x272      // (ip, port)   -> id (>=0) / -1
#define SYS_tcp_state   0x273      // (id)         -> 0=connexion / 1=établi / 2=fermé / -1
#define SYS_tcp_send    0x274      // (id, buf, len) -> octets acceptés / -1
#define SYS_tcp_recv    0x275      // (id, buf, len) -> octets / 0=rien / -1=fermé
#define SYS_tcp_close   0x276      // (id)
#define SYS_random      0x277      // (buf, len) : octets aléatoires (CSPRNG noyau)
#define SYS_ssh_hostkey 0x278      // (buf, max) -> texte cle d'hote + empreinte
#define SYS_ssh_keygen  0x279      // (buf, max) -> genere une cle, l'ajoute, rend la privee
#define SYS_ping_send   0x27a      // (ip) : envoie un echo ICMP (non bloquant)
#define SYS_ping_got    0x27b      // -> 1 si la reponse echo est arrivee

// --- Système de fichiers (par chemin ; agit sur le VFS du noyau) -------------
#define SYS_vfs_list    0x230      // (path, index, *dirent) -> 1=entrée / 0=fin / -1=erreur
#define SYS_vfs_read    0x231      // (vfs_io*) -> octets lus
#define SYS_vfs_write   0x232      // (vfs_io*) -> octets écrits, -1 si interdit
#define SYS_vfs_create  0x233      // (path, type) -> 0/-1
#define SYS_vfs_delete  0x234      // (path) -> 0/-1
#define SYS_vfs_save    0x235      // (vfs_io*) -> remplace tout le fichier (tronque)
#define SYS_vfs_stat    0x236      // (path, *dirent) -> 0=ok / -1=absent

// --- Presse-papiers (copier/coller, partagé entre applications) ---------------
#define SYS_clip_set    0x254      // (buf, len) : copie dans le presse-papiers
#define SYS_clip_get    0x255      // (buf, max) -> taille copiée

// --- Comptes / permissions ---------------------------------------------------
#define SYS_whoami      0x240      // (*userinfo)
#define SYS_can_write   0x241      // (path) -> 1/0
#define SYS_users_list  0x242      // (index, *userinfo) -> 1=rempli / 0=fin
#define SYS_login       0x243      // (nom, mot_de_passe) -> 0=ok / -1 (change l'utilisateur courant)
#define SYS_passwd      0x244      // (ancien, nouveau) -> 0=ok / -1

// --- Client SSH (se connecter vers une autre machine) ------------------------
#define SYS_ssh_exec    0x27c      // (sshreq_t*) -> taille de la sortie / -1

// Requête de client SSH (SYS_ssh_exec) : exécute 'command' sur ip:port.
typedef struct {
    uint32_t    ip; uint16_t port;
    const char *user, *password, *command;
    char       *out; int outmax;
} sshreq_t;

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

// État de l'interface réseau renvoyé par SYS_net_info (ordre hôte).
typedef struct { uint32_t ip, mask, gateway, dns; int up; } netinfo_t;

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
