# Dépôt de paquets d'exemple pour `pacman` (MonOS)

Ce dossier est un **dépôt de paquets prêt à servir**. `pacman`, dans MonOS,
télécharge depuis un **serveur HTTP** : il suffit de servir ce dossier.

Contenu :
- `repo.db` — l'index des paquets (nom, version, dépendances, SHA-256)
- `hello-1.0.pkg`, `cowsay-1.0.pkg` — deux paquets d'exemple
- `mkrepo.py` — script pour (re)générer / ajouter des paquets

---

## 1. Lancer le serveur (sur votre machine HÔTE, pas dans MonOS)

Dans ce dossier :

```bash
python3 -m http.server 8000      # Linux / macOS
python  -m http.server 8000      # Windows (si Python est installé)
```

### Windows sans Python (le plus simple)
Si `python` renvoie le message du Microsoft Store, utilisez le script PowerShell
fourni. Clic droit sur **PowerShell → Exécuter en tant qu'administrateur**, placez-
vous dans ce dossier (`cd C:\chemin\vers\repo`), puis :

```powershell
powershell -ExecutionPolicy Bypass -File serve.ps1
```

Il affiche les adresses IP à utiliser côté MonOS. (Le mode administrateur est
nécessaire pour que la VM puisse joindre l'hôte.)

Laissez cette fenêtre ouverte : c'est votre « miroir » de paquets.

## 2. Pointer MonOS vers ce serveur, puis installer

Cela dépend de **comment MonOS accède au réseau** :

### a) Sous QEMU (`make run-net`)
L'hôte est toujours à l'adresse `10.0.2.2`, et c'est **déjà l'adresse par défaut**.
Dans le terminal de MonOS, directement :

```
pacman -Sy            # synchronise l'index
pacman -S cowsay      # installe cowsay (+ sa dépendance hello), vérifie le SHA-256
pacman -Q             # liste ce qui est installé
pacman -R cowsay      # désinstalle
```

### b) Sous VMware / VirtualBox
La VM et l'hôte ont des IP différentes. Trouvez l'**IP de l'hôte** sur le réseau
de la VM (souvent `192.168.x.y` — `ipconfig` sous Windows, `ip a` sous Linux),
puis dans MonOS :

```
pacman -Sr 192.168.x.y:8000   # configure l'adresse du dépôt (remplacez l'IP)
pacman -Sy
pacman -S cowsay
```

> Astuce : depuis MonOS, `ping 192.168.x.y` permet de vérifier que l'hôte est
> bien joignable avant d'essayer pacman.

## 3. Vérifier que l'installation a marché

```
cat /usr/share/cowsay/cow.txt
cat /usr/share/hello/message.txt
```

---

## Ajouter vos propres paquets

Éditez `mkrepo.py` (liste `PACKAGES`), ajoutez un paquet avec ses fichiers et
ses dépendances, puis régénérez le dépôt :

```bash
python3 mkrepo.py
```

Relancez le serveur HTTP si besoin, puis `pacman -Sy` dans MonOS.

## Pourquoi ce n'est pas « comme Arch Linux »

Il n'existe pas de miroir MonOS public sur internet : le format de paquet
(`MONPAC1`) est propre à ce projet. `pacman` installe donc depuis **un dépôt que
vous hébergez vous-même** (ce dossier). C'est volontairement minimal et
pédagogique : un index texte + des fichiers, intégrité garantie par SHA-256.
