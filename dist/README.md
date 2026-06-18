# Image bootable MonOS v2

- `monos-v2.iso` : image hybride **UEFI + BIOS legacy**, amorçable en machine
  virtuelle (QEMU, VMware, VirtualBox) ou flashable sur clé USB avec `dd` / Rufus.
- `monos-v2.iso.sha256` : somme de contrôle pour vérifier l'intégrité.

## Tester dans VMware (Workstation / Player)
1. Nouvelle VM → « J'installerai le système plus tard » (pas d'OS).
2. Système invité : **Other / Other 64-bit**.
3. Paramètres de la VM → Options → Avancé → **Type de firmware : UEFI**.
4. Lecteur CD/DVD → **Utiliser une image ISO** → choisir `monos-v2.iso`,
   cocher « Connecter à la mise sous tension ».
5. Démarrer. Menu Limine → MonOS. Connexion : `user`/`user` ou `root`/`root`.

Clavier en **AZERTY**. Souris/clavier émulés en **PS/2** (pas de pile USB) :
en cas de souris inerte, c'est que l'hyperviseur a présenté un pointeur USB.

## Reconstruire soi-même
    make iso        # regenère build/monos.iso
