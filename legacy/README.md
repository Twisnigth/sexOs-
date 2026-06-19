# sexOs — un mini système d'exploitation x86 en assembleur

sexOs est un petit système d'exploitation **x86 32 bits** écrit intégralement en
**assembleur (NASM)**. Il démarre en mode réel 16 bits, bascule en mode protégé
32 bits, puis lance un **shell interactif** avec quelques commandes intégrées.

```
  __  __             ___  ____
 |  \/  | ___  _ __ / _ \/ ___|
 | |\/| |/ _ \| '_ \ | | \___ \
 | |  | | (_) | | | | |_| |___) |
 |_|  |_|\___/|_| |_|\___/|____/

Bienvenue dans sexOs v0.1 !
sexOs> _
```

## Fonctionnalités

- **Bootloader maison** (`boot.asm`, 512 octets) :
  - mode réel 16 bits, message d'amorçage ;
  - activation de la ligne A20 (port `0x92`) ;
  - mise en place d'une GDT plate ;
  - chargement du noyau depuis le disque via `INT 0x13` (lecture secteur par
    secteur, conversion LBA → CHS) ;
  - bascule en mode protégé 32 bits puis saut vers le noyau ;
  - signature de boot `0xAA55`.
- **Noyau** (`kernel.asm`, mode protégé 32 bits) :
  - **pilote VGA texte** (`0xB8000`) : affichage caractère/chaîne, effacement,
    retour à la ligne, **défilement**, **curseur matériel**, **couleurs** ;
  - **pilote clavier PS/2** : lecture des scancodes (`0x60`/`0x64`),
    conversion scancode → ASCII en **disposition AZERTY**, gestion de **Maj**,
    du **Retour arrière** et d'**Entrée** ;
  - **shell** : invite, lecture de ligne, analyse de la commande.
- **Commandes intégrées** : `help`, `clear`, `echo`, `about`, `reboot`,
  `fortune`, `art`.

## Dépendances (Debian / Ubuntu)

```bash
sudo apt-get update
sudo apt-get install nasm binutils qemu-system-x86 xorriso
```

| Outil               | Rôle                                            |
|---------------------|-------------------------------------------------|
| `nasm`              | assembleur                                      |
| `binutils` (`ld`, `objcopy`) | liaison du noyau et extraction du binaire |
| `qemu-system-x86`   | fournit `qemu-system-i386` pour tester          |
| `xorriso`           | génération de l'image ISO (ou `genisoimage`)    |

## Compilation et lancement

### Avec `make`

```bash
make            # construit l'image disque os.img
make run        # construit puis lance sexOs dans QEMU (disquette)
make run-hdd    # lance sexOs comme disque dur
make iso        # génère os.iso
make run-iso    # construit l'ISO et la lance (-cdrom)
make clean      # supprime les fichiers générés
```

### Sans `make`

```bash
./build.sh                          # produit os.img
qemu-system-i386 -fda os.img        # lance sexOs
./make_iso.sh                       # produit os.iso
qemu-system-i386 -cdrom os.iso      # lance depuis l'ISO
```

## Utilisation

Une fois sexOs démarré, une invite `sexOs>` apparaît. Tapez une commande puis
**Entrée** :

| Commande   | Effet                                         |
|------------|-----------------------------------------------|
| `help`     | liste les commandes                           |
| `clear`    | efface l'écran                                |
| `echo X`   | affiche le texte `X`                          |
| `about`    | nom et version du système                     |
| `reboot`   | redémarre la machine                          |
| `fortune`  | affiche une citation                          |
| `art`      | affiche un peu d'art ASCII                    |

> **Clavier AZERTY** : le clavier émulé est interprété en AZERTY. La touche
> physiquement située à l'emplacement du `Q` d'un clavier QWERTY produit donc un
> `a`, etc. La touche **Maj** donne les majuscules.

## Comment fonctionne l'ISO

L'ISO est générée en **El Torito mode « no-emulation »** (`make_iso.sh`). Le
secteur de boot d'un CD-ROM ne se lit pas comme une disquette ; on demande donc
au BIOS de charger en mémoire, en plus du secteur de boot, **tout le noyau qui
le suit** grâce à l'option `-boot-load-size`.

Le bootloader est conçu pour les deux cas :

- **disquette / disque dur** : il charge le noyau lui-même via `INT 0x13` ;
- **CD-ROM** : il détecte que le noyau est **déjà présent en mémoire** (grâce à
  une signature `0xC0DEBED5` placée à son tout début) et le recopie simplement à
  son adresse finale, sans utiliser `INT 0x13`.

Un seul et même `boot.asm` fonctionne ainsi pour `-fda`, `-hda` et `-cdrom`.

## Structure du projet

```
.
├── boot.asm      # bootloader 512 octets (mode réel -> mode protégé)
├── kernel.asm    # noyau 32 bits (VGA, clavier, shell)
├── linker.ld     # script de liaison du noyau (chargé à 0x10000)
├── Makefile      # cibles de build, run, iso, clean
├── build.sh      # build de os.img sans make
├── make_iso.sh   # génération de os.iso bootable
└── README.md
```

## Détails techniques

- **Carte mémoire** : bootloader à `0x7C00`, noyau à `0x10000` (point d'entrée à
  `0x10004`, juste après la signature de 4 octets), pile à `0x90000`, mémoire
  vidéo à `0xB8000`.
- **GDT** : modèle plat 0–4 Gio, sélecteurs `0x08` (code) et `0x10` (données).
- **Taille du noyau** : chargé sur 64 secteurs (32 Kio). Si le noyau grossit
  au-delà, augmentez `KERNEL_SECTORS` dans `boot.asm` **et** dans le `Makefile` /
  `build.sh` / `make_iso.sh` (`BOOT_LOAD_SIZE = KERNEL_SECTORS + 1`).

## Licence

Projet pédagogique, fourni tel quel, libre d'utilisation.
