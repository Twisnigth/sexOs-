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

## Reste à faire (honnêteté sur le périmètre)

- **IPC compositeur ↔ applications** : aujourd'hui le bureau est UN seul processus
  ring 3 (compositeur + applis dans le même espace). Pour faire de chaque appli un
  **processus isolé** (un crash d'appli ne touche pas le compositeur), il faut une
  primitive IPC (mémoire partagée des buffers de fenêtre + messages d'événements).
- **Commandes réseau / SSH / pacman / busybox du terminal** : indisponibles dans
  le bureau ring 3 (elles dépendent de pilotes noyau) ; elles nécessiteraient des
  syscalls réseau dédiés. Stubs pour l'instant.
- **Filesystem partagé** : le bureau a son propre VFS en mémoire (distinct de celui
  du noyau utilisé par sshd/pacman). Les unifier demanderait une API VFS par
  syscalls.
- **sshd** n'est plus interrogé pendant que le bureau tourne (serait à confier à
  une tâche noyau dédiée).
- État `brk`/`mmap_base` encore partiellement global.

## Fichiers clés

- `kernel/sched.c`, `kernel/sched.h`, `kernel/switch.asm` — ordonnanceur.
- `kernel/idt.c`, `kernel/isr.asm` — préemption + kill-on-fault.
- `kernel/proc.c`, `kernel/syscalls.h` — appels système.
- `user/lib/`, `user/gfxdemo.c`, `user/wmserver.c` — runtime + programmes ring 3.
