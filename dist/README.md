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

## Ce que contient cette image
**Bureau multi-processus en ring 3** : le compositeur et chaque application sont
des **processus ring 3 isolés** reliés par IPC (messagerie + mémoire partagée).
Au démarrage : compositeur, **terminal**, **explorateur de fichiers**,
**moniteur d'activité (style btop)**, **navigateur web** et horloge. Un crash
d'application est contenu par le noyau (la tâche fautive est tuée, sa fenêtre
récupérée, le reste du système continue).

**Navigateur web (HTTP)** : `user/web.c` récupère des pages via une vraie pile
réseau (DNS + TCP + HTTP/1.1) au moyen de **sockets non bloquantes** servies par
une tâche réseau du noyau. Pour l'essayer en QEMU :
1. sur l'hôte, servez un dossier : `python3 -m http.server 8000` ;
2. lancez MonOS avec le réseau : `make run-net` ;
3. dans le navigateur, l'adresse `http://10.0.2.2:8000/` pointe vers l'hôte
   (passerelle SLIRP).

**HTTPS / TLS 1.3** est géré : suites **AES-128-GCM** et **ChaCha20-Poly1305**,
échange **X25519**, **vérification de certificat RSA, ECDSA P-256 et P-384**,
**magasin d'environ 150 AC racines** (paquet Mozilla), suivi des **redirections**
et contrôle du **nom d'hôte (SAN)**. Le navigateur affiche `[TLS verifie]` /
`[TLS non verifie]`. Page d'accueil par défaut : `https://google.com/`.
Limites résiduelles : AES-256-GCM et Ed25519 (peu courants) non gérés.

> Remarque : sous certains hyperviseurs/réseaux d'entreprise qui **interceptent
> le TLS**, le certificat présenté n'est pas celui du vrai site — le navigateur
> l'indiquera. Sur une machine à Internet ouvert, les vrais sites se chargent.

Côté noyau (exécuté au démarrage), l'image embarque aussi :
- **Réseau** (e1000 + DHCP/DNS) : `ifconfig`, `ping`, `nslookup`, `wget`.
- **SSH** client et serveur : `ssh hôte user motdepasse "commande"`.
- **Binaires Linux** statiques en ring 3 : `bb <applet>` (busybox musl).
- **Gestionnaire de paquets** : `pacman -Sy`, `-S <pkg>`, `-R <pkg>`, `-Q`.

Le réseau/SSH ont besoin d'une carte **e1000** côté hyperviseur. En QEMU,
utilisez `make run-net` (configure e1000 + redirection du port SSH 2222→22).

### Faire marcher `pacman` (hors QEMU)
`pacman` télécharge depuis un **serveur HTTP**. Par défaut il vise `10.0.2.2:8000`
(l'hôte sous QEMU). Sous **VMware/VirtualBox** cette adresse n'existe pas → il
affichera « dépôt injoignable ». Pointez-le vers un vrai serveur :
1. Sur une machine joignable depuis la VM (ex. votre PC, IP `192.168.x.y`),
   placez `repo.db` + les paquets `.pkg` dans un dossier et lancez :
   `python3 -m http.server 8000`
2. Dans MonOS : `pacman -Sr 192.168.x.y:8000` puis `pacman -Sy`, `pacman -S <pkg>`.

Sans serveur de dépôt, `pacman` ne peut rien installer : c'est une démo qui a
besoin d'une infrastructure en face (comme un vrai pacman a besoin d'un miroir).

## Reconstruire soi-même
    make iso        # regenère build/monos.iso
    make run-net    # QEMU avec réseau e1000 (DHCP/DNS) + SSH (hostfwd 2222→22)
