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
#  Le bureau (compositeur, WM, applications) est désormais compilé en RING 3
#  (cf. user/) : on l'exclut du noyau.
DESKTOP_SRC := $(KDIR)/desktop.c $(KDIR)/wm.c $(KDIR)/app_terminal.c \
               $(KDIR)/app_files.c $(KDIR)/app_settings.c $(KDIR)/app_editor.c \
               $(KDIR)/app_about.c
CSRC := $(filter-out $(DESKTOP_SRC), $(wildcard $(KDIR)/*.c))
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

# ---- Programmes de test ring 3 (Phase 1) : binaires plats embarqués ---------
UTEST_BINS := $(OBJDIR)/taskA.bin $(OBJDIR)/taskB.bin $(OBJDIR)/crash.bin

$(OBJDIR)/%.bin: user_tests/%.asm
	@mkdir -p $(OBJDIR)
	$(ASM) -f bin $< -o $@

# ---- Runtime + programmes userspace en C (ring 3, ELF) ----------------------
UCFLAGS  := -Wall -ffreestanding -fno-stack-protector -fno-pic -fno-pie -m64 \
            -march=x86-64 -mno-sse -mno-mmx -mno-80387 -mno-red-zone -O2 \
            -I$(KDIR) -Iuser/lib -I$(MCDIR)
ULDFLAGS := -m elf_x86_64 -nostdlib -static --build-id=none -T user/user.ld

$(OBJDIR)/u_crt0.o: user/lib/crt0.asm
	@mkdir -p $(OBJDIR)
	$(ASM) -f elf64 $< -o $@
$(OBJDIR)/u_%.o: user/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(UCFLAGS) -c $< -o $@
$(OBJDIR)/u_%.o: user/lib/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(UCFLAGS) -c $< -o $@
# Modules du noyau recompilés pour l'espace utilisateur (gfx, klib, vfs, users,
# et le bureau lui-même) : préfixe uk_.
$(OBJDIR)/uk_%.o: $(KDIR)/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(UCFLAGS) -c $< -o $@

$(OBJDIR)/gfxdemo.elf: $(OBJDIR)/u_crt0.o $(OBJDIR)/u_gfxdemo.o user/user.ld
	$(LD) $(ULDFLAGS) -o $@ $(OBJDIR)/u_crt0.o $(OBJDIR)/u_gfxdemo.o
$(OBJDIR)/wmserver.elf: $(OBJDIR)/u_crt0.o $(OBJDIR)/u_wmserver.o $(OBJDIR)/uk_gfx.o user/user.ld
	$(LD) $(ULDFLAGS) -o $@ $(OBJDIR)/u_crt0.o $(OBJDIR)/u_wmserver.o $(OBJDIR)/uk_gfx.o

# Le BUREAU complet en ring 3 : runtime + modules portables + desktop/wm/apps.
DESKTOP_OBJS := $(OBJDIR)/u_crt0.o $(OBJDIR)/u_libos.o $(OBJDIR)/u_desktop_main.o \
                $(OBJDIR)/uk_gfx.o $(OBJDIR)/uk_klib.o $(OBJDIR)/uk_vfs.o \
                $(OBJDIR)/uk_users.o $(OBJDIR)/uk_desktop.o $(OBJDIR)/uk_wm.o \
                $(OBJDIR)/uk_app_terminal.o $(OBJDIR)/uk_app_files.o \
                $(OBJDIR)/uk_app_settings.o $(OBJDIR)/uk_app_editor.o \
                $(OBJDIR)/uk_app_about.o
$(OBJDIR)/desktop.elf: $(DESKTOP_OBJS) user/user.ld
	$(LD) $(ULDFLAGS) -o $@ $(DESKTOP_OBJS)

# --- Compositeur (serveur) + applications, chacun un PROCESSUS séparé ---------
$(OBJDIR)/compositor.elf: $(OBJDIR)/u_crt0.o $(OBJDIR)/u_compositor.o \
                          $(OBJDIR)/u_urt.o $(OBJDIR)/uk_gfx.o user/user.ld
	$(LD) $(ULDFLAGS) -o $@ $(OBJDIR)/u_crt0.o $(OBJDIR)/u_compositor.o \
	      $(OBJDIR)/u_urt.o $(OBJDIR)/uk_gfx.o
# Modèle commun aux applications clientes (libwin + urt + gfx).
APPLIBS := $(OBJDIR)/u_crt0.o $(OBJDIR)/u_libwin.o $(OBJDIR)/u_urt.o $(OBJDIR)/uk_gfx.o
$(OBJDIR)/app_clock.elf: $(OBJDIR)/u_app_clock.o $(APPLIBS) user/user.ld
	$(LD) $(ULDFLAGS) -o $@ $(OBJDIR)/u_app_clock.o $(APPLIBS)
$(OBJDIR)/app_hello.elf: $(OBJDIR)/u_app_hello.o $(APPLIBS) user/user.ld
	$(LD) $(ULDFLAGS) -o $@ $(OBJDIR)/u_app_hello.o $(APPLIBS)
$(OBJDIR)/app_crash.elf: $(OBJDIR)/u_app_crash.o $(APPLIBS) user/user.ld
	$(LD) $(ULDFLAGS) -o $@ $(OBJDIR)/u_app_crash.o $(APPLIBS)
# Applications complètes (terminal, explorateur) : chacune un PROCESSUS séparé.
$(OBJDIR)/term.elf: $(OBJDIR)/u_term.o $(APPLIBS) user/user.ld
	$(LD) $(ULDFLAGS) -o $@ $(OBJDIR)/u_term.o $(APPLIBS)
$(OBJDIR)/files.elf: $(OBJDIR)/u_files.o $(APPLIBS) user/user.ld
	$(LD) $(ULDFLAGS) -o $@ $(OBJDIR)/u_files.o $(APPLIBS)
$(OBJDIR)/monitor.elf: $(OBJDIR)/u_monitor.o $(APPLIBS) user/user.ld
	$(LD) $(ULDFLAGS) -o $@ $(OBJDIR)/u_monitor.o $(APPLIBS)
# Monocypher recompilé pour l'espace utilisateur (X25519, ChaCha20, Poly1305).
$(OBJDIR)/u_monocypher.o: $(MCDIR)/monocypher.c
	@mkdir -p $(OBJDIR)
	$(CC) $(UCFLAGS) -c $< -o $@
# Navigateur web : compositeur + pile réseau + TLS 1.3 (HTTP/HTTPS en ring 3).
WEB_OBJS := $(OBJDIR)/u_web.o $(OBJDIR)/u_http.o $(OBJDIR)/u_tls.o \
            $(OBJDIR)/u_bigint.o $(OBJDIR)/u_rsa.o $(OBJDIR)/u_ecdsa.o $(OBJDIR)/u_x509.o $(OBJDIR)/u_castore.o \
            $(OBJDIR)/u_monocypher.o $(OBJDIR)/uk_sha256.o
$(OBJDIR)/web.elf: $(WEB_OBJS) $(APPLIBS) user/user.ld
	$(LD) $(ULDFLAGS) -o $@ $(WEB_OBJS) $(APPLIBS)

# user_blobs.asm incbin les binaires : dépendance explicite (prioritaire sur le
# motif générique ci-dessus).
$(OBJDIR)/user_blobs_asm.o: $(KDIR)/user_blobs.asm $(UTEST_BINS) \
                            $(OBJDIR)/gfxdemo.elf $(OBJDIR)/wmserver.elf $(OBJDIR)/desktop.elf \
                            $(OBJDIR)/compositor.elf $(OBJDIR)/app_clock.elf \
                            $(OBJDIR)/app_hello.elf $(OBJDIR)/app_crash.elf \
                            $(OBJDIR)/term.elf $(OBJDIR)/files.elf \
                            $(OBJDIR)/monitor.elf $(OBJDIR)/web.elf
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
	@# Module : binaire Linux statique busybox (musl, construit dans user/).
	@if [ -f user/busybox ]; then cp user/busybox $(ISODIR)/boot/busybox; \
	 else echo "(user/busybox absent : module non inclus)"; fi
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
