#!/usr/bin/env bash
# =============================================================================
#  make_iso.sh  --  Génère une image ISO bootable à partir de os.img
# -----------------------------------------------------------------------------
#  Méthode : El Torito en mode « no-emulation ».
#
#  Le bootloader (boot.asm) est conçu pour ce mode : on demande au BIOS de
#  charger en mémoire (à 0x7C00) non seulement le secteur de boot mais aussi
#  le noyau qui le suit, grâce à l'option -boot-load-size. Le bootloader
#  détecte alors que le noyau est déjà présent en mémoire (signature à 0x7E00)
#  et le recopie à son adresse finale, sans avoir besoin d'INT 0x13 sur le CD.
#
#  Résultat : qemu-system-i386 -cdrom os.iso  démarre jusqu'au shell.
# =============================================================================
set -euo pipefail

OS_IMG="os.img"
OS_ISO="os.iso"
ISO_DIR="iso_root"

# Nombre de secteurs virtuels (512 o) à charger = secteur de boot + noyau.
# Doit couvrir boot.bin (1 secteur) + KERNEL_SECTORS (64) = 65 secteurs.
BOOT_LOAD_SIZE=65

# ---- Vérifications ----------------------------------------------------------
if [ ! -f "$OS_IMG" ]; then
    echo "==> $OS_IMG introuvable, construction prealable..."
    if [ -f Makefile ]; then make "$OS_IMG"; else ./build.sh; fi
fi

# ---- Préparation de l'arborescence ISO --------------------------------------
rm -rf "$ISO_DIR"
mkdir -p "$ISO_DIR/boot"
cp "$OS_IMG" "$ISO_DIR/boot/os.img"

# ---- Création de l'ISO ------------------------------------------------------
if command -v xorriso >/dev/null 2>&1; then
    echo "==> Generation de l'ISO avec xorriso (El Torito, no-emulation)"
    xorriso -as mkisofs \
        -o "$OS_ISO" \
        -V "MONOS" \
        -b boot/os.img \
        -no-emul-boot \
        -boot-load-size "$BOOT_LOAD_SIZE" \
        "$ISO_DIR"
elif command -v genisoimage >/dev/null 2>&1; then
    echo "==> Generation de l'ISO avec genisoimage (El Torito, no-emulation)"
    genisoimage \
        -o "$OS_ISO" \
        -V "MONOS" \
        -b boot/os.img \
        -no-emul-boot \
        -boot-load-size "$BOOT_LOAD_SIZE" \
        "$ISO_DIR"
else
    echo "ERREUR: ni xorriso ni genisoimage ne sont installes."
    echo "        Installez par ex. : sudo apt-get install xorriso"
    exit 1
fi

echo "==> ISO creee : $OS_ISO ($(stat -c%s "$OS_ISO") octets)"
echo "    Lancez : qemu-system-i386 -cdrom $OS_ISO"
