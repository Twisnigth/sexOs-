# MonOS — Frontière ring 0 / ring 3 (refactor du modèle de privilèges)

Ce document décrit, sans détour, **ce qui tourne en ring 0 (noyau) et ce qui
tourne en ring 3 (utilisateur)** après le refactor du modèle de privilèges, et
ce qui reste à faire.

## Le défaut corrigé

Avant : `kmain` appelait `desktop_run()`, une boucle **ring 0** qui exécutait
le compositeur, le gestionnaire de fenêtres et les applications. Le noyau
faisait donc du travail applicatif, et la moindre faute d'une « application »
provoquait un `panic()` de tout le système.

## Modèle retenu : noyau MONOLITHIQUE

Les pilotes restent en ring 0 et sont exposés par appels système. Mettre les
pilotes en ring 3 (vrai micro-noyau) imposerait IOPL / bitmap d'E/S dans la TSS,
le mapping MMIO en espace utilisateur et la redirection des IRQ via IPC :
**hors périmètre** (bonus). C'est le choix pragmatique standard (Linux, etc.).

## Ce qui tourne en RING 0 (noyau)

| Composant | Rôle |
|-----------|------|
| GDT / IDT / **TSS (rsp0 par tâche)** | tables CPU, pile noyau par tâche |
| PIC, **PIT (préemption)** | interruptions, minuteur |
| **Ordonnanceur + commutation de contexte** | `sched.c`, `switch.asm` |
| **kill-on-fault** | une faute ring 3 tue la tâche, le noyau survit (`idt.c`) |
| PMM, VMM, tas noyau | mémoire physique/virtuelle, `kmalloc` |
| Pilotes : PS/2, framebuffer, RTC, PCI, e1000/réseau | accès matériel |
| VFS (ramfs), base des comptes | fichiers, authentification |
| Répartiteur d'appels système | `proc.c` (ABI Linux + natifs ≥ 0x200) |

## Ce qui tourne en RING 3 (CPL 3) — prouvé

| Élément | Preuve |
|---------|--------|
| Tâches `A` / `B` concurrentes | entrelacement à l'écran série (commutation de contexte) |
| Tâche `crash` (`cli`) | `#GP cs=1b` → **tuée**, le noyau survit (sépare réellement les privilèges) |
| **Le BUREAU COMPLET** | login, compositeur, WM, **terminal + explorateur + paramètres + éditeur + à propos** tournent à CPL 3 ; `info registers` montre `CPL=3, CS=0x1b` pendant que le bureau est actif (`docs/ring3-desktop-apps.png`) |

Preuves disponibles : `info registers` du moniteur QEMU (`CPL=3`, `CS=0x1b`,
`RIP` dans le code du bureau à 0x40xxxx), appel système `SYS_get_cpl` (renvoie 3),
faute sur `cli` depuis le ring 3.

## Comment le bureau tourne en ring 3

`kmain` ne contient plus AUCUNE logique applicative : après l'init matériel, il
crée la tâche `bureau` (un ELF ring 3) puis appelle `sched_start()` qui ne revient
jamais (idle `hlt` si le bureau meurt). Le bureau est un **seul processus ring 3**
construit en compilant pour l'espace utilisateur les modules portables du noyau
(`gfx.c`, `klib.c`, `vfs.c`, `users.c`) + `desktop.c`/`wm.c`/`app_*.c`, liés à une
**libOS** (`user/lib/libos.c`) qui réimplémente via syscalls les services noyau
(framebuffer, entrées, tas, horloge, infos système). Le VFS et les comptes vivent
donc DANS le processus bureau ; le noyau garde les siens pour sshd/pacman.

## Appels système

ABI Linux (pour busybox/musl) : `write, writev, read, brk, mmap, munmap,
arch_prctl, clock_gettime, uname, exit`…

Natifs MonOS (≥ 0x200, cf. `kernel/syscalls.h`) : `get_cpl (0x200)`,
`fb_map (0x210)`, `input_poll (0x212)`, `time_ms (0x250)`.

## Fait

- ✅ **`kmain` ne fait plus de travail applicatif** : il crée la tâche bureau puis
  cède la main à l'ordonnanceur (`sched_start`). Plus de `desktop_run` en ring 0.
- ✅ **Le bureau entier (compositeur + WM + 5 applications) tourne en ring 3.**

## Découpage en processus séparés (IPC) — FAIT et stable

Le bureau peut tourner en **plusieurs processus ring 3 distincts** reliés par IPC :
- **Syscalls IPC** (`kernel/proc.c`, `kernel/syscalls.h`) : `ipc_send`/`ipc_recv`
  (messagerie par mailbox par tâche), **`ipc_wait`** (attente BLOQUANTE — la tâche
  dort jusqu'à un message), **`yield`** (commutation coopérative), `shm_create`/
  `shm_map` (mémoire partagée entre espaces), `comp_register`/`comp_pid`.
- **Entrée syscall unifiée** (`usermode.asm`) : `syscall_entry` construit un
  `registers_t` identique à celui des interruptions, ce qui permet à un appel
  système de **bloquer ou céder** proprement (sauvegarde/reprise de contexte).
- **Compositeur serveur** (`user/compositor.c`, un processus) : possède le
  framebuffer + les entrées, alloue un tampon partagé (shm) par fenêtre, route
  les événements vers la fenêtre au focus, gère déplacement/focus/fermeture.
- **Bibliothèque cliente** (`user/lib/libwin.c`) + **applications, chacune un
  PROCESSUS** : `app_clock` (se rafraîchit via `yield`), `app_crash` (déréférence
  NULL pour prouver l'isolation), et surtout les **applications COMPLÈTES** :
  - **`user/term.c`** — terminal (grille 80×24, éditeur de ligne, commandes
    `ls/cd/pwd/cat/mkdir/touch/rm/whoami/date/sysinfo`),
  - **`user/files.c`** — explorateur (navigation, aperçu, création/suppression).
  - **`user/web.c`** — **navigateur web** : client HTTP/1.1 (barre d'adresse,
    rendu HTML simplifié, défilement) au-dessus de **sockets TCP non bloquantes**
    et d'une **tâche réseau** noyau (cf. `docs/ARCHITECTURE-network.md`).
  - **`user/monitor.c`** — **moniteur d'activité (style btop)** : jauge CPU +
    historique, jauge mémoire, et **table des processus** (PID, nom, état, %CPU,
    mémoire) triée par charge. Le %CPU vient des **tops du minuteur attribués à
    chaque tâche** par l'ordonnanceur (`task_t.cpu_ticks`, idle = pid 0) ; la
    mémoire d'un processus est comptabilisée à l'allocation (`task_t.mem_pages` :
    image + piles + `mmap`/`shm`/framebuffer). Exposés par **`sys_proc_list`**.
  Ces applis n'utilisent QUE des appels système : fenêtre via `libwin` (IPC),
  système de fichiers via **`sys_vfs_*`** (par chemin, donc aucun pointeur noyau
  partagé), comptes via `sys_whoami`, infos via `sys_sysinfo` / `sys_proc_list`.
  Le moniteur illustre concrètement le modèle : le **compositeur** consomme le
  plus de CPU (recomposition à chaque trame), tandis que le terminal et
  l'explorateur, **bloqués sur `ipc_wait`**, sont à **0 %** (état « dort ») —
  preuve visible que l'IPC bloquante évite le sondage actif.
- **Récupération des fenêtres orphelines** : le compositeur interroge
  `sys_pid_alive(owner)` ; quand un processus client meurt (crash tué, ou sortie),
  sa fenêtre est retirée de la composition (elle ne reste plus affichée).

Vérifié en QEMU (`docs/ring3-multiproc*.png`) :
- 3 fenêtres provenant de 3 **processus séparés**, dessinées via mémoire
  partagée et composées par le compositeur ; focus / z-order / déplacement OK.
- l'horloge avance en continu (compositeur + horloge bien vivants) ;
- **`app_crash` déréférence NULL → le noyau le TUE (kill-on-fault) → le
  compositeur et les autres applications continuent** (aucun plantage système).
- modèle **sans sondage actif** : les applis pilotées par les événements dorment
  (`ipc_wait`) et sont réveillées à la livraison ; commutations coopératives.

### Correction d'un défaut d'architecture : course au démarrage de l'ordonnanceur

`sched_start` / `sched_run_until_idle` s'exécutaient avec les interruptions
actives. Une préemption du minuteur survenant **entre `install()` et l'`iretq`
de reprise** faisait écraser le contexte de la 1ʳᵉ tâche par `sched_on_timer`
(qui enregistre un cadre noyau dans `ctx`), d'où un **`#GP` intermittent sur
`iretq`** (≈ 1 démarrage sur 2). Le handoff initial est désormais **atomique**
(`cli`) ; l'`iretq` restaure `IF=1` dans la tâche, donc la préemption reprend
normalement une fois en ring 3. Les autres chemins de commutation
(`sched_on_timer`, `sched_switch_from`) sont déjà atomiques (IF=0 dans l'ISR /
sous `FMASK`). Vérifié : **8/8 démarrages sans panique**.

## Reste à faire (honnêteté sur le périmètre)

- `wait(pid)` côté parent, priorités d'ordonnancement, IPC zéro-copie plus riche.
- **Commandes réseau / SSH / pacman / busybox du terminal** : indisponibles dans
  le bureau ring 3 (elles dépendent de pilotes noyau) ; elles nécessiteraient des
  syscalls réseau dédiés. Stubs pour l'instant.
- **sshd** n'est plus interrogé pendant que le bureau tourne (serait à confier à
  une tâche noyau dédiée).
- État `brk`/`mmap_base` encore partiellement global (sans incidence : seul le
  compositeur utilise `mmap`, une seule fois).

## Fichiers clés

- `kernel/sched.c`, `kernel/sched.h`, `kernel/switch.asm` — ordonnanceur.
- `kernel/idt.c`, `kernel/isr.asm` — préemption + kill-on-fault.
- `kernel/proc.c`, `kernel/syscalls.h` — appels système.
- `user/lib/`, `user/gfxdemo.c`, `user/wmserver.c` — runtime + programmes ring 3.
