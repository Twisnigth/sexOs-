#!/usr/bin/env python3
# =============================================================================
#  repo/mkrepo.py -- Genere un depot de paquets MonOS (format MONPAC1)
# -----------------------------------------------------------------------------
#  Produit, dans le dossier courant :
#    - un fichier <nom>-<version>.pkg par paquet (format MONPAC1)
#    - un fichier repo.db (index : nom version deps sha256)
#
#  Pour ajouter un paquet : ajoutez une entree dans la liste PACKAGES ci-dessous,
#  puis relancez : python3 mkrepo.py
#  Ensuite, servez le dossier : python3 -m http.server 8000
# =============================================================================
import hashlib, os

# --- Definition des paquets --------------------------------------------------
#  name    : nom du paquet (ce que vous tapez dans `pacman -S <name>`)
#  version : version (texte libre)
#  deps    : liste de noms de paquets requis (installes automatiquement)
#  files   : { chemin_dans_MonOS : contenu_texte }
PACKAGES = [
    {
        "name": "hello", "version": "1.0", "deps": [],
        "files": {
            "/usr/share/hello/message.txt":
                "Bonjour ! Le paquet 'hello' a ete installe par pacman dans MonOS.\n",
        },
    },
    {
        "name": "cowsay", "version": "1.0", "deps": ["hello"],
        "files": {
            "/usr/share/cowsay/cow.txt":
                "  ___________\n"
                " < MonOS ! >\n"
                "  -----------\n"
                "        \\   ^__^\n"
                "         \\  (oo)\\___\n"
                "            (__)\\   )\n"
                "                ||--w|\n",
        },
    },
]

# --- Construction d'un paquet MONPAC1 ---------------------------------------
#  Format :  "MONPAC1\n"  puis, pour chaque fichier :
#            "FILE <chemin> <taille>\n" <octets> "\n"
def build_pkg(pkg):
    out = b"MONPAC1\n"
    for path, content in pkg["files"].items():
        data = content.encode("utf-8")
        out += b"FILE %s %d\n" % (path.encode(), len(data))
        out += data + b"\n"
    return out

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    db_lines = []
    for pkg in PACKAGES:
        blob = build_pkg(pkg)
        fname = "%s-%s.pkg" % (pkg["name"], pkg["version"])
        with open(os.path.join(here, fname), "wb") as f:
            f.write(blob)
        sha = hashlib.sha256(blob).hexdigest()
        deps = ",".join(pkg["deps"]) if pkg["deps"] else "-"
        db_lines.append("%s %s %s %s" % (pkg["name"], pkg["version"], deps, sha))
        print("paquet : %-20s sha256=%s" % (fname, sha))
    with open(os.path.join(here, "repo.db"), "w") as f:
        f.write("\n".join(db_lines) + "\n")
    print("repo.db ecrit (%d paquets)." % len(PACKAGES))

if __name__ == "__main__":
    main()
