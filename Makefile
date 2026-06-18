# =============================================================================
#  Makefile -- Construction de MonOS v2 (noyau 64 bits + image Limine)
# -----------------------------------------------------------------------------
#  Cibles :
#    make            : compile le noyau (build/kernel.elf)
#    make iso        : construit l'image hybride bootable build/monos.iso
#    make run        : lance l'ISO dans QEMU avec firmware UEFI (OVMF)
#    make run-bios   : lance l'ISO dans QEMU en BIOS legacy (SeaBIOS)
#    make clean      : supprime les fichiers générés
#
#  Le noyau est compilé en C autonome (freestanding) avec le gcc de l'hôte,
#  ce qui évite un cross-compilateur. Limine (dans third_party/) fournit le
#  démarrage UEFI+BIOS, le long mode, la pagination initiale et le framebuffer.
# =============================================================================

# ---- Outils -----------------------------------------------------------------
CC       := gcc
LD       := ld
ASM      := nasm
QEMU     := qemu-system-x86_64

LIMINE_DIR := third_party/limine
LIMINE     := $(LIMINE_DIR)/limine

# ---- Répertoires et fichiers ------------------------------------------------
KDIR    := kernel
BUILD   := build
OBJDIR  := $(BUILD)/obj
ISODIR  := $(BUILD)/iso
KERNEL  := $(BUILD)/kernel.elf
ISO     := $(BUILD)/monos.iso

# Firmware UEFI pour QEMU.
OVMF_CODE := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS_SRC := /usr/share/OVMF/OVMF_VARS_4M.fd
OVMF_VARS := $(BUILD)/OVMF_VARS.fd

# ---- Sources ----------------------------------------------------------------
CSRC := $(wildcard $(KDIR)/*.c)
ASRC := $(wildcard $(KDIR)/*.asm)
MCDIR := third_party/monocypher
MCSRC := $(MCDIR)/monocypher.c $(MCDIR)/monocypher-ed25519.c
OBJ  := $(patsubst $(KDIR)/%.c,$(OBJDIR)/%.o,$(CSRC)) \
        $(patsubst $(KDIR)/%.asm,$(OBJDIR)/%_asm.o,$(ASRC)) \
        $(patsubst $(MCDIR)/%.c,$(OBJDIR)/mc_%.o,$(MCSRC))

# ---- Drapeaux de compilation (noyau autonome x86_64) ------------------------
CFLAGS := -Wall -Wextra -std=c11 -ffreestanding -fno-stack-protector \
          -fno-stack-clash-protection -fno-pic -fno-pie -m64 -march=x86-64 \
          -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
          -mcmodel=kernel -O2 -g -I$(KDIR) -I$(MCDIR)

LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
           --build-id=none -T $(KDIR)/link.ld

# ---- Cible par défaut -------------------------------------------------------
all: $(KERNEL)

$(OBJDIR)/%.o: $(KDIR)/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%_asm.o: $(KDIR)/%.asm
	@mkdir -p $(OBJDIR)
	$(ASM) -f elf64 $< -o $@

$(OBJDIR)/mc_%.o: $(MCDIR)/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJ) $(KDIR)/link.ld
	@mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@echo "Noyau construit : $@"

# ---- Outil hôte Limine (compilé depuis les sources vendues si besoin) -------
$(LIMINE):
	$(MAKE) -C $(LIMINE_DIR)

# ---- Image ISO hybride (UEFI + BIOS, dd-able sur USB) -----------------------
iso: $(ISO)

$(ISO): $(KERNEL) boot/limine.conf $(LIMINE)
	rm -rf $(ISODIR)
	mkdir -p $(ISODIR)/boot/limine $(ISODIR)/EFI/BOOT
	cp $(KERNEL) $(ISODIR)/boot/kernel.elf
	cp boot/limine.conf $(ISODIR)/boot/limine/
	cp $(LIMINE_DIR)/limine-bios.sys $(ISODIR)/boot/limine/
	cp $(LIMINE_DIR)/limine-bios-cd.bin $(ISODIR)/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin $(ISODIR)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI  $(ISODIR)/EFI/BOOT/
	cp $(LIMINE_DIR)/BOOTIA32.EFI $(ISODIR)/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
	    -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(ISODIR) -o $(ISO)
	$(LIMINE) bios-install $(ISO)
	@echo "Image construite : $(ISO)"

# ---- Exécution dans QEMU ----------------------------------------------------
$(OVMF_VARS):
	@mkdir -p $(BUILD)
	cp $(OVMF_VARS_SRC) $(OVMF_VARS)

run: $(ISO) $(OVMF_VARS)
	$(QEMU) -M q35 -m 512M \
	    -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	    -cdrom $(ISO) -serial stdio

run-bios: $(ISO)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -serial stdio

# Avec réseau (user-mode) : carte e1000 + redirection du port 22 (SSH) vers 2222.
run-net: $(ISO) $(OVMF_VARS)
	$(QEMU) -M q35 -m 512M -cpu qemu64,+rdrand \
	    -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	    -netdev user,id=n0,hostfwd=tcp::2222-:22 \
	    -device e1000,netdev=n0 \
	    -cdrom $(ISO) -serial stdio

# ---- Nettoyage --------------------------------------------------------------
clean:
	rm -rf $(BUILD)
	@echo "Nettoyage termine."

.PHONY: all iso run run-bios run-net clean
