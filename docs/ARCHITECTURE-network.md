# Pile réseau MonOS — du DHCP minimal à un client HTTP ring 3

Objectif : une base réseau **solide et utilisable depuis les applications ring 3**
(un navigateur web), pas seulement un DHCP de démarrage. La difficulté n'était
pas tant les protocoles (TCP/DNS/HTTP existaient déjà sous forme **bloquante**)
que leur **intégration dans le modèle multi-processus** : après `sched_start()`,
le noyau ne rend jamais la main, et un appel système s'exécute avec les
interruptions masquées (`FMASK`), donc `pit_ms()` est figé — toute boucle
d'attente active `while (pit_ms() < fin) nic_poll();` gèle la machine. C'est la
raison pour laquelle « le réseau servait à peine et SSH ne marchait pas » : plus
rien ne pompait la carte une fois le bureau lancé.

## Le défaut corrigé

Avant : tout le réseau était **synchrone** et sondait le NIC en boucle
(`tcp_connect`, `tcp_recv`, `dns_resolve`, `arp_resolve`…). Valable au démarrage
(avant l'ordonnanceur, interruptions actives), **inutilisable** ensuite.

Après : une **tâche réseau noyau** devient l'unique propriétaire du NIC, et les
applications parlent au réseau par des **sockets non bloquantes** (appels
système). Aucune application ne bloque le système ; aucune ne sonde le NIC.

## Architecture

```
  Application ring 3 (navigateur)            Tâche réseau (ring 0, ordonnancée)
  ───────────────────────────────           ──────────────────────────────────
  http_fetch() en espace utilisateur         boucle :  cli
    sys_dns_resolve / sys_tcp_open                       nic_poll()   (RX)
    sys_tcp_send / sys_tcp_recv  ── IPC ─►               tcp_tick()   (retransmit)
    (attente coopérative: sys_yield)                     dns_tick()
                                                        sti ; hlt   (~1 ms)
```

- **Tâche réseau** (`net_task_run`, `kernel/net.c`) : tâche NOYAU créée par
  `sched_new_kernel_task`, ordonnancée comme les autres (IF=1, donc le minuteur
  avance et la machine n'est jamais gelée). Son traitement (`nic_poll` +
  `tcp_tick` + `dns_tick`) tourne en **section critique `cli`** : il ne
  s'entrelace donc jamais avec un appel système socket (lui aussi IF=0). NIC,
  TCP et DNS sont **sérialisés** — pas de verrou, pas de course. `hlt` rend la
  main jusqu'au prochain top (~1 ms).

- **TCP non bloquant + piloté par minuteur** (`kernel/tcp.c`) :
  `tcp_open` (SYN, retransmis par `tcp_tick`), `tcp_state`, `tcp_write`
  (bufferise ; émission/retransmission par la tâche), `tcp_read` (vide le tampon
  de réception), `tcp_shutdown` (FIN). Fenêtre de réception **annoncée
  dynamiquement** (contrôle de flux réel), acquittements cumulatifs, purge du
  tampon d'émission sur ACK. Les anciennes fonctions bloquantes restent pour le
  démarrage/le shell hérité.

- **ARP non bloquant** (`arp_lookup`) : sur défaut de cache, émet une requête et
  **abandonne** la trame ; TCP/DNS la rejouent via leur retransmission. Évite
  toute attente active dans la tâche réseau.

- **DNS non bloquant** (`kernel/dns.c`) : `dns_query` lance/relance une requête A
  et renvoie `0=en cours / 1=résolu / -1=échec` ; `dns_tick` gère réémission et
  délai.

## Appels système (ring 3)

| Syscall            | Rôle                                             |
|--------------------|--------------------------------------------------|
| `SYS_net_info`     | état de l'interface (IP, masque, passerelle, DNS)|
| `SYS_dns_resolve`  | résolution DNS non bloquante (0/1/-1)            |
| `SYS_tcp_open`     | ouverture active → id de socket                  |
| `SYS_tcp_state`    | 0=connexion / 1=établi / 2=fermé / -1=erreur     |
| `SYS_tcp_send`     | écrit dans le tampon d'émission                  |
| `SYS_tcp_recv`     | lit le tampon de réception (0=rien, -1=fermé)    |
| `SYS_tcp_close`    | fermeture (FIN)                                  |

## HTTP et navigateur (ring 3)

- `user/lib/http.c` : client **HTTP/1.1** en espace utilisateur (analyse d'URL,
  DNS ou IP, GET, lecture jusqu'à fermeture, décodage **chunked**). L'attente se
  fait par `sys_yield` : le reste du bureau continue de tourner pendant un
  téléchargement.
- `user/web.c` : **navigateur minimal** (processus ring 3) — barre d'adresse
  éditable, rendu HTML simplifié (balises retirées, entités décodées, sauts de
  bloc, retour à la ligne), défilement. Socle du futur moteur de rendu.

## Vérifié (honnêtement)

- En QEMU (carte e1000, réseau utilisateur SLIRP) : DHCP → 10.0.2.15, puis le
  navigateur récupère `http://10.0.2.2:8000/` depuis un vrai serveur HTTP de
  l'hôte. Le serveur journalise `GET / HTTP/1.1 200`, la page s'affiche rendue
  (cf. `docs/ring3-browser.png`). Stress : plusieurs démarrages successifs,
  `GET 200` à chaque fois, **zéro panique**.
- Connexion par **IP** validée de bout en bout. La **résolution DNS** est
  implémentée et exercée ; atteindre un site public dépend d'un accès sortant de
  l'hyperviseur (NAT SLIRP) et n'est pas garanti dans tous les environnements.

## TLS 1.3 / HTTPS (ring 3)

`user/lib/tls.c` ajoute un **client TLS 1.3** au-dessus des sockets, et `http.c`
l'utilise automatiquement pour les URL `https://` (port 443 par défaut).

- **Suite unique** : `TLS_CHACHA20_POLY1305_SHA256`, échange de clés **X25519**.
  Compatible avec la majorité des serveurs modernes (CDN, etc.), qui acceptent
  ChaCha20-Poly1305 + X25519. Les serveurs n'offrant qu'AES-GCM ou des courbes
  NIST échoueront (AES non implémenté ici).
- **Crypto réutilisée** : Monocypher (X25519, ChaCha20-IETF, Poly1305) recompilé
  pour le ring 3, + le SHA-256 du noyau. AEAD RFC 8439 et **HKDF** (HMAC-SHA256,
  `HKDF-Expand-Label`, key schedule complet) écrits dans `tls.c`. Aléa fourni par
  un nouvel appel système `SYS_random` (CSPRNG du noyau).
- **Handshake 1-RTT** : ClientHello (key_share X25519, SNI, signature_algorithms),
  ServerHello, dérivation du secret partagé, déchiffrement AEAD du *flight*
  chiffré (EncryptedExtensions, Certificate, CertificateVerify, Finished),
  **vérification du Finished serveur**, envoi du Finished client, bascule sur les
  clés applicatives. Couche d'enregistrement chiffrée (nonce = iv⊕seq, en-tête en
  données associées).

**Vérifié** : en QEMU, MonOS effectue le handshake complet contre un serveur
**TLS 1.3 réel** (OpenSSL/Python, ChaCha20-Poly1305) et récupère
`https://10.0.2.2:8443/` — `HTTP 200`, page rendue (cf. `docs/ring3-https.png`),
zéro panique.

> ⚠️ **Limite de sécurité importante** : le **certificat du serveur n'est PAS
> vérifié** (ni signature RSA/ECDSA, ni magasin d'autorités, ni vérification du
> nom). La session est donc **chiffrée mais NON authentifiée** : elle protège
> contre l'écoute passive, **pas contre un homme du milieu actif**. À ne pas
> utiliser pour des données sensibles tant que la vérification de chaîne n'est
> pas implémentée.

## Reste à faire (périmètre assumé)

- **Vérification du certificat TLS** : nécessite RSA-PKCS1/PSS et/ou ECDSA P-256
  (absents de Monocypher → arithmétique grands entiers à écrire), un analyseur
  X.509/ASN.1 et un magasin d'autorités racines. C'est la prochaine étape pour
  un HTTPS *authentifié*.
- **AES-GCM** : pour interopérer avec les serveurs qui n'offrent pas ChaCha20.
- **SSH** : le serveur est encore écrit en style **bloquant** et n'a pas été
  porté sur le modèle non bloquant de la tâche réseau ; il reste donc dormant
  pendant que le bureau tourne (à porter comme le client HTTP).
- Réassemblage des segments TCP hors-ordre (on s'appuie sur l'ordre, suffisant
  via SLIRP), et une seule requête DNS en vol à la fois.
