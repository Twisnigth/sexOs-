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
| `gfxdemo` | `cpl=3` ; dégradé plein écran **dessiné depuis le ring 3** + souris (`docs/ring3-gfx.png`) |
| **Compositeur / WM** (`wmserver`) | bureau à CPL 3 : fenêtres déplaçables/fermables, possède framebuffer + entrées (`docs/ring3-desktop.png`) |

Preuves disponibles : appel système `SYS_get_cpl` (renvoie 3), faute sur `cli`
depuis le ring 3 (cs=0x1b, RPL 3), et `info registers` du moniteur QEMU.

## Appels système

ABI Linux (pour busybox/musl) : `write, writev, read, brk, mmap, munmap,
arch_prctl, clock_gettime, uname, exit`…

Natifs MonOS (≥ 0x200, cf. `kernel/syscalls.h`) : `get_cpl (0x200)`,
`fb_map (0x210)`, `input_poll (0x212)`, `time_ms (0x250)`.

## Reste à faire (honnêteté sur le périmètre)

- **Portage des 5 applications historiques en ring 3** (terminal, explorateur,
  paramètres, éditeur, « à propos »). Elles détiennent aujourd'hui des pointeurs
  noyau `vfs_node_t*` et appellent directement VFS/comptes. Il faut d'abord une
  **API VFS par handles** (`open/read/write/readdir/stat/...`) et des syscalls
  comptes, puis réécrire leur logique fichiers. C'est le gros du travail restant.
- **IPC compositeur ↔ applications** (mémoire partagée des buffers de fenêtre +
  messages d'événements) pour faire de chaque application un **processus séparé**.
- **Bascule définitive** : faire de `init` (PID 1) le seul point de départ en
  ring 3, et retirer `desktop_run` (ring 0) de `kmain`. Aujourd'hui le bureau
  historique ring 0 tourne encore derrière les démos ring 3.
- État `brk`/`mmap_base` encore partiellement global (par défaut mono-processus).

## Fichiers clés

- `kernel/sched.c`, `kernel/sched.h`, `kernel/switch.asm` — ordonnanceur.
- `kernel/idt.c`, `kernel/isr.asm` — préemption + kill-on-fault.
- `kernel/proc.c`, `kernel/syscalls.h` — appels système.
- `user/lib/`, `user/gfxdemo.c`, `user/wmserver.c` — runtime + programmes ring 3.
