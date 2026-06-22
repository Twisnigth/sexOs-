# =============================================================================
#  Makefile  --  Construction de sexOs
# -----------------------------------------------------------------------------
#  Cibles principales :
#    make            : construit l'image disque os.img
#    make run        : construit puis lance sexOs dans QEMU (disquette)
#    make run-hdd    : lance sexOs dans QEMU en tant que disque dur
#    make iso        : génère l'image ISO bootable (via make_iso.sh)
#    make run-iso    : construit l'ISO et la lance dans QEMU (-cdrom)
#    make clean      : supprime les fichiers générés
# =============================================================================

# ---- Outils -----------------------------------------------------------------
ASM      := nasm
LD       := ld
OBJCOPY  := objcopy
QEMU     := qemu-system-i386

# ---- Fichiers ---------------------------------------------------------------
BOOT_BIN   := boot.bin
KERNEL_O   := kernel.o
KERNEL_ELF := kernel.elf
KERNEL_BIN := kernel.bin
OS_IMG     := os.img
OS_ISO     := os.iso

# Doit correspondre à KERNEL_SECTORS dans boot.asm (64 secteurs = 32 Kio).
KERNEL_SECTORS := 64
KERNEL_SIZE    := $(shell echo $$(( $(KERNEL_SECTORS) * 512 )))
# Taille d'une disquette 1.44 Mio (géométrie 18 secteurs / 2 têtes / 80 cyl.).
FLOPPY_SIZE    := 1474560

# ---- Cible par défaut -------------------------------------------------------
all: $(OS_IMG)

# ---- Bootloader (binaire plat de 512 octets) --------------------------------
$(BOOT_BIN): boot.asm
	$(ASM) -f bin $< -o $@

# ---- Noyau : ELF -> binaire plat lié à 0x10000 ------------------------------
$(KERNEL_O): kernel.asm
	$(ASM) -f elf32 $< -o $@

$(KERNEL_ELF): $(KERNEL_O) linker.ld
	$(LD) -m elf_i386 -T linker.ld -o $@ $(KERNEL_O)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	@# Vérifie que le noyau tient dans le nombre de secteurs chargés par le boot.
	@size=$$(stat -c%s $@); \
	if [ $$size -gt $(KERNEL_SIZE) ]; then \
		echo "ERREUR: le noyau ($$size o) depasse $(KERNEL_SIZE) o."; \
		echo "        Augmentez KERNEL_SECTORS dans boot.asm et le Makefile."; \
		exit 1; \
	fi
	@# Complète le noyau jusqu'à KERNEL_SECTORS secteurs pleins.
	@truncate -s $(KERNEL_SIZE) $@

# ---- Image disque : bootloader + noyau, complétée à 1.44 Mio ----------------
$(OS_IMG): $(BOOT_BIN) $(KERNEL_BIN)
	cat $(BOOT_BIN) $(KERNEL_BIN) > $@
	truncate -s $(FLOPPY_SIZE) $@
	@echo "Image disque construite : $@"

# ---- Image ISO bootable -----------------------------------------------------
iso: $(OS_IMG)
	./make_iso.sh

$(OS_ISO): $(OS_IMG)
	./make_iso.sh

# ---- Exécution dans QEMU ----------------------------------------------------
run: $(OS_IMG)
	$(QEMU) -fda $(OS_IMG)

run-hdd: $(OS_IMG)
	$(QEMU) -drive format=raw,file=$(OS_IMG)

run-iso: $(OS_ISO)
	$(QEMU) -cdrom $(OS_ISO)

# ---- Nettoyage --------------------------------------------------------------
clean:
	rm -f $(BOOT_BIN) $(KERNEL_O) $(KERNEL_ELF) $(KERNEL_BIN) $(OS_IMG) $(OS_ISO)
	@echo "Nettoyage termine."

.PHONY: all iso run run-hdd run-iso clean
