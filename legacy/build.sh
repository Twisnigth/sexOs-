#!/usr/bin/env bash
# =============================================================================
#  build.sh  --  Construit sexOs sans dépendre de make
# -----------------------------------------------------------------------------
#  Assemble le bootloader et le noyau, puis fusionne le tout dans une image
#  disque bootable (os.img) complétée à la taille d'une disquette 1.44 Mio.
# =============================================================================
set -euo pipefail

# ---- Paramètres (doivent correspondre à boot.asm) ---------------------------
KERNEL_SECTORS=64
KERNEL_SIZE=$(( KERNEL_SECTORS * 512 ))   # 32 Kio
FLOPPY_SIZE=1474560                        # 1.44 Mio

BOOT_BIN="boot.bin"
KERNEL_O="kernel.o"
KERNEL_ELF="kernel.elf"
KERNEL_BIN="kernel.bin"
OS_IMG="os.img"

echo "==> Assemblage du bootloader (boot.asm)"
nasm -f bin boot.asm -o "$BOOT_BIN"

echo "==> Assemblage du noyau (kernel.asm)"
nasm -f elf32 kernel.asm -o "$KERNEL_O"

echo "==> Liaison du noyau a 0x10000 (linker.ld)"
ld -m elf_i386 -T linker.ld -o "$KERNEL_ELF" "$KERNEL_O"

echo "==> Extraction du binaire plat (objcopy)"
objcopy -O binary "$KERNEL_ELF" "$KERNEL_BIN"

# Vérification de taille
size=$(stat -c%s "$KERNEL_BIN")
if [ "$size" -gt "$KERNEL_SIZE" ]; then
    echo "ERREUR: le noyau ($size o) depasse $KERNEL_SIZE o."
    echo "        Augmentez KERNEL_SECTORS dans boot.asm et build.sh."
    exit 1
fi
echo "    Noyau : $size octets (limite $KERNEL_SIZE)."

# Complète le noyau à un nombre entier de secteurs
truncate -s "$KERNEL_SIZE" "$KERNEL_BIN"

echo "==> Fusion bootloader + noyau -> $OS_IMG"
cat "$BOOT_BIN" "$KERNEL_BIN" > "$OS_IMG"
truncate -s "$FLOPPY_SIZE" "$OS_IMG"

echo "==> Termine : $OS_IMG ($(stat -c%s "$OS_IMG") octets)"
echo "    Lancez : qemu-system-i386 -fda $OS_IMG"
