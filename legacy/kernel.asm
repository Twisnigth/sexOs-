; =============================================================================
;  kernel.asm  --  Noyau de sexOs (mode protégé 32 bits)
; -----------------------------------------------------------------------------
;  Contient :
;    - Un pilote écran VGA mode texte (0xB8000) : caractère, chaîne, effacement,
;      retour à la ligne, défilement, curseur matériel, couleurs.
;    - Un pilote clavier PS/2 : lecture des scancodes (port 0x60), conversion
;      scancode -> ASCII (disposition AZERTY), gestion de Maj, Retour arrière, Entrée.
;    - Un shell interactif avec les commandes : help, clear, echo, about, reboot,
;      fortune, art.
;
;  Les 4 premiers octets sont une signature (KERNEL_MAGIC) qui permet au
;  bootloader de détecter le noyau en mémoire lors d'un démarrage depuis l'ISO.
;  Le vrai point d'entrée (_start) se trouve donc juste après (offset +4).
;
;  Assemblé en ELF puis lié à l'adresse 0x10000 (voir linker.ld).
; =============================================================================

[BITS 32]

; ---- Constantes -------------------------------------------------------------
VGA_MEM      equ 0xB8000           ; mémoire vidéo texte
VGA_WIDTH    equ 80
VGA_HEIGHT   equ 25
INPUT_MAX    equ 128               ; taille maximale d'une ligne saisie

section .text
global _start

kernel_magic:
    dd 0xC0DEBED5                  ; signature (doit rester les 4 premiers octets)

; =============================================================================
;  Point d'entrée du noyau
; =============================================================================
_start:
    mov esp, 0x90000               ; pile (par sécurité, déjà réglée par le boot)

    mov byte [current_color], 0x07 ; gris clair sur noir
    call clear_screen

    ; --- Bannière de bienvenue ----------------------------------------------
    mov byte [current_color], 0x0A ; vert clair
    mov esi, banner
    call print

    mov byte [current_color], 0x0F ; blanc
    mov esi, welcome
    call print

    mov byte [current_color], 0x07

; --- Boucle principale du shell ---------------------------------------------
shell_loop:
    mov byte [current_color], 0x0E ; jaune pour l'invite
    mov esi, prompt
    call print
    mov byte [current_color], 0x07

    call read_line                 ; lit une ligne dans input_buffer
    call handle_command            ; interprète la commande
    jmp shell_loop

; =============================================================================
;  PILOTE VGA
; =============================================================================

; ---- clear_screen : efface tout l'écran et replace le curseur en (0,0) ------
clear_screen:
    push eax
    push ecx
    push edi
    mov edi, VGA_MEM
    mov ecx, VGA_WIDTH * VGA_HEIGHT
    mov ah, [current_color]
    mov al, ' '
    rep stosw                      ; remplit l'écran d'espaces colorés
    mov byte [cursor_x], 0
    mov byte [cursor_y], 0
    call update_cursor
    pop edi
    pop ecx
    pop eax
    ret

; ---- putchar : affiche le caractère AL en gérant \n, \b, \r et défilement ---
putchar:
    push eax
    push ebx
    push ecx
    push edx
    push edi
    mov dl, al                     ; DL = caractère à traiter

    cmp dl, 0x0A                    ; '\n'
    je  .newline
    cmp dl, 0x08                    ; retour arrière
    je  .backspace
    cmp dl, 0x0D                    ; retour chariot
    je  .carriage

    ; --- caractère imprimable : écriture en mémoire vidéo --------------------
    movzx eax, byte [cursor_y]
    imul eax, VGA_WIDTH
    movzx ebx, byte [cursor_x]
    add eax, ebx
    shl eax, 1                      ; *2 (caractère + attribut)
    mov edi, VGA_MEM
    add edi, eax
    mov al, dl
    mov ah, [current_color]
    mov [edi], ax

    inc byte [cursor_x]
    cmp byte [cursor_x], VGA_WIDTH
    jb  .done
    mov byte [cursor_x], 0          ; passage à la ligne automatique
    inc byte [cursor_y]
    jmp .check_scroll

.newline:
    mov byte [cursor_x], 0
    inc byte [cursor_y]
    jmp .check_scroll

.carriage:
    mov byte [cursor_x], 0
    jmp .done

.backspace:
    cmp byte [cursor_x], 0
    jne .bs_dec
    cmp byte [cursor_y], 0
    je  .done                       ; déjà en haut à gauche : rien à faire
    dec byte [cursor_y]
    mov byte [cursor_x], VGA_WIDTH - 1
    jmp .bs_clear
.bs_dec:
    dec byte [cursor_x]
.bs_clear:
    movzx eax, byte [cursor_y]
    imul eax, VGA_WIDTH
    movzx ebx, byte [cursor_x]
    add eax, ebx
    shl eax, 1
    mov edi, VGA_MEM
    add edi, eax
    mov al, ' '
    mov ah, [current_color]
    mov [edi], ax                   ; efface le caractère sous le curseur
    jmp .done

.check_scroll:
    cmp byte [cursor_y], VGA_HEIGHT
    jb  .done
    call scroll
    mov byte [cursor_y], VGA_HEIGHT - 1

.done:
    call update_cursor
    pop edi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; ---- scroll : décale tout l'écran d'une ligne vers le haut -------------------
scroll:
    push esi
    push edi
    push ecx
    push eax
    mov edi, VGA_MEM                       ; destination : ligne 0
    mov esi, VGA_MEM + VGA_WIDTH * 2       ; source : ligne 1
    mov ecx, VGA_WIDTH * (VGA_HEIGHT - 1)
    rep movsw                              ; remonte les lignes 1..24 vers 0..23
    ; efface la dernière ligne
    mov edi, VGA_MEM + VGA_WIDTH * (VGA_HEIGHT - 1) * 2
    mov ecx, VGA_WIDTH
    mov ah, [current_color]
    mov al, ' '
    rep stosw
    pop eax
    pop ecx
    pop edi
    pop esi
    ret

; ---- print : affiche la chaîne pointée par ESI (terminée par 0) -------------
print:
    push eax
    push esi
.next:
    lodsb
    test al, al
    jz  .done
    call putchar
    jmp .next
.done:
    pop esi
    pop eax
    ret

; ---- newline : passe simplement à la ligne ----------------------------------
newline:
    push eax
    mov al, 0x0A
    call putchar
    pop eax
    ret

; ---- update_cursor : positionne le curseur matériel via le contrôleur CRT ---
update_cursor:
    push eax
    push ebx
    push edx
    movzx eax, byte [cursor_y]
    imul eax, VGA_WIDTH
    movzx ebx, byte [cursor_x]
    add eax, ebx                    ; position linéaire du curseur
    mov ebx, eax

    mov dx, 0x3D4                   ; registre d'index CRTC
    mov al, 0x0F                    ; registre « position basse du curseur »
    out dx, al
    mov dx, 0x3D5
    mov al, bl
    out dx, al

    mov dx, 0x3D4
    mov al, 0x0E                    ; registre « position haute du curseur »
    out dx, al
    mov dx, 0x3D5
    mov al, bh
    out dx, al

    pop edx
    pop ebx
    pop eax
    ret

; =============================================================================
;  PILOTE CLAVIER PS/2
; =============================================================================

; ---- getchar : attend une touche et renvoie son code ASCII dans AL ----------
;      Renvoie : caractère imprimable, ou 0x0A (Entrée), ou 0x08 (Retour arrière).
;      Gère l'état des touches Maj (Shift) en interne.
getchar:
.wait:
    in  al, 0x64                    ; registre d'état du contrôleur
    test al, 1                      ; bit 0 : buffer de sortie plein ?
    jz  .wait
    in  al, 0x60                    ; lecture du scancode

    test al, 0x80                   ; bit 7 mis = relâchement de touche
    jnz .release

    ; --- appui de touche -----------------------------------------------------
    cmp al, 0x2A                    ; Maj gauche
    je  .shift_down
    cmp al, 0x36                    ; Maj droite
    je  .shift_down
    cmp al, 0x39                    ; au-delà de l'espace : hors table
    ja  .wait

    movzx ebx, al
    cmp byte [shift_state], 0
    jne .shifted
    mov al, [keymap + ebx]
    jmp .check
.shifted:
    mov al, [keymap_shift + ebx]
.check:
    test al, al                     ; 0 = touche ignorée -> on continue
    jz  .wait
    ret

.shift_down:
    mov byte [shift_state], 1
    jmp .wait

.release:
    and al, 0x7F                    ; on retire le bit 7
    cmp al, 0x2A
    je  .shift_up
    cmp al, 0x36
    je  .shift_up
    jmp .wait
.shift_up:
    mov byte [shift_state], 0
    jmp .wait

; ---- read_line : lit une ligne complète dans input_buffer -------------------
;      Écho à l'écran, gestion du retour arrière, terminée par Entrée.
read_line:
    push eax
    push ecx
    push edi
    mov edi, input_buffer
    xor ecx, ecx                    ; longueur courante
.loop:
    call getchar
    cmp al, 0x0A                    ; Entrée -> fin de ligne
    je  .done
    cmp al, 0x08                    ; Retour arrière
    je  .backspace
    cmp ecx, INPUT_MAX - 1          ; tampon plein ?
    jae .loop
    mov [edi], al
    inc edi
    inc ecx
    call putchar                    ; écho du caractère saisi
    jmp .loop
.backspace:
    test ecx, ecx
    jz  .loop                       ; rien à effacer
    dec edi
    dec ecx
    mov al, 0x08
    call putchar                    ; efface visuellement le caractère
    jmp .loop
.done:
    mov byte [edi], 0               ; termine la chaîne
    call newline
    pop edi
    pop ecx
    pop eax
    ret

; =============================================================================
;  SHELL : interprétation des commandes
; =============================================================================

; ---- streq : compare les chaînes [ESI] et [EDI] (terminées par 0) -----------
;      Sortie : ZF=1 si égales, ZF=0 sinon.
streq:
    push esi
    push edi
.cmp:
    mov al, [esi]
    mov bl, [edi]
    cmp al, bl
    jne .neq
    test al, al
    jz  .eq
    inc esi
    inc edi
    jmp .cmp
.eq:
    pop edi
    pop esi
    xor eax, eax                    ; ZF=1
    ret
.neq:
    pop edi
    pop esi
    or  eax, 1                      ; ZF=0
    ret

; ---- handle_command : reconnaît la commande dans input_buffer ---------------
handle_command:
    mov al, [input_buffer]
    test al, al
    jz  .ret                        ; ligne vide

    mov esi, input_buffer
    mov edi, cmd_help
    call streq
    jz  cmd_do_help

    mov esi, input_buffer
    mov edi, cmd_clear
    call streq
    jz  cmd_do_clear

    mov esi, input_buffer
    mov edi, cmd_about
    call streq
    jz  cmd_do_about

    mov esi, input_buffer
    mov edi, cmd_reboot
    call streq
    jz  cmd_do_reboot

    mov esi, input_buffer
    mov edi, cmd_fortune
    call streq
    jz  cmd_do_fortune

    mov esi, input_buffer
    mov edi, cmd_art
    call streq
    jz  cmd_do_art

    call cmd_try_echo               ; « echo ... » ? (CF=1 si traité)
    jc  .ret

    ; --- commande inconnue ---------------------------------------------------
    mov esi, msg_unknown
    call print
    mov esi, input_buffer
    call print
    mov esi, msg_unknown_end
    call print
.ret:
    ret

; ---- help -------------------------------------------------------------------
cmd_do_help:
    mov esi, txt_help
    call print
    ret

; ---- clear ------------------------------------------------------------------
cmd_do_clear:
    call clear_screen
    ret

; ---- about ------------------------------------------------------------------
cmd_do_about:
    mov esi, txt_about
    call print
    ret

; ---- reboot : redémarre la machine via le contrôleur clavier 8042 -----------
cmd_do_reboot:
    mov esi, txt_reboot
    call print
    mov al, 0xFE                    ; impulsion de reset sur la ligne CPU
    out 0x64, al
    hlt                             ; au cas où le reset traînerait
    ret

; ---- fortune : affiche une petite citation ----------------------------------
cmd_do_fortune:
    mov esi, txt_fortune
    call print
    ret

; ---- art : un peu d'art ASCII -----------------------------------------------
cmd_do_art:
    mov byte [current_color], 0x0D ; magenta clair
    mov esi, txt_art
    call print
    mov byte [current_color], 0x07
    ret

; ---- echo : si la ligne commence par « echo », affiche le reste -------------
;      Sortie : CF=1 si la commande a été reconnue et traitée.
cmd_try_echo:
    mov esi, input_buffer
    mov edi, cmd_echo               ; "echo"
    mov ecx, 4
.cmp4:
    mov al, [esi]
    mov bl, [edi]
    cmp al, bl
    jne .no
    inc esi
    inc edi
    loop .cmp4
    ; ESI pointe maintenant sur input_buffer+4
    mov al, [esi]
    test al, al
    jz  .empty                      ; « echo » seul -> ligne vide
    cmp al, ' '
    jne .no                         ; « echoXXX » -> pas la commande echo
    inc esi                         ; on saute l'espace
    call print                      ; affiche l'argument
    call newline
    stc
    ret
.empty:
    call newline
    stc
    ret
.no:
    clc
    ret

; =============================================================================
;  DONNÉES EN LECTURE SEULE
; =============================================================================
section .rodata

banner:
    db 10
    db "  __  __             ___  ____  ", 10
    db " |  \/  | ___  _ __ / _ \/ ___| ", 10
    db " | |\/| |/ _ \| '_ \ | | \___ \ ", 10
    db " | |  | | (_) | | | | |_| |___) |", 10
    db " |_|  |_|\___/|_| |_|\___/|____/ ", 10
    db 10, 0

welcome:
    db "Bienvenue dans sexOs v0.1 !", 10
    db "Systeme x86 32 bits ecrit en assembleur.", 10
    db "Tapez 'help' pour la liste des commandes.", 10, 10, 0

prompt:
    db "sexOs> ", 0

txt_help:
    db "Commandes disponibles :", 10
    db "  help    - affiche cette aide", 10
    db "  clear   - efface l'ecran", 10
    db "  echo X  - affiche le texte X", 10
    db "  about   - informations sur le systeme", 10
    db "  reboot  - redemarre la machine", 10
    db "  fortune - une citation au hasard", 10
    db "  art     - un peu d'art ASCII", 10, 0

txt_about:
    db 10
    db "  sexOs version 0.1", 10
    db "  -----------------", 10
    db "  Un mini systeme d'exploitation x86 32 bits.", 10
    db "  Bootloader maison + noyau, 100% assembleur (NASM).", 10
    db "  Pilotes : VGA texte + clavier PS/2 (AZERTY).", 10, 10, 0

txt_reboot:
    db "Redemarrage en cours...", 10, 0

txt_fortune:
    db 10
    db "  ", 34, "Il n'y a que deux sortes de langages :", 10
    db "   ceux dont les gens se plaignent et ceux", 10
    db "   que personne n'utilise.", 34, " - B. Stroustrup", 10, 10, 0

txt_art:
    db 10
    db "      .--.", 10
    db "     |o_o |    sexOs", 10
    db "     |:_/ |   ronronne", 10
    db "    //   \ \  dans QEMU", 10
    db "   (|     | )", 10
    db "  /'\_   _/`\", 10
    db "  \___)=(___/", 10, 10, 0

msg_unknown:
    db "Commande inconnue : '", 0
msg_unknown_end:
    db "'. Tapez 'help'.", 10, 0

; --- Noms des commandes ------------------------------------------------------
cmd_help    db "help", 0
cmd_clear   db "clear", 0
cmd_echo    db "echo", 0
cmd_about   db "about", 0
cmd_reboot  db "reboot", 0
cmd_fortune db "fortune", 0
cmd_art     db "art", 0

; =============================================================================
;  TABLES DE CONVERSION SCANCODE -> ASCII (jeu de scancodes 1, AZERTY)
;  Index = scancode (0x00 a 0x39). 0 = touche non gérée.
; =============================================================================
keymap:
    db 0,    0                      ; 00, 01 (Echap)
    db "1234567890"                 ; 02-0B : rangée des chiffres
    db "-", "="                     ; 0C, 0D
    db 0x08, 0                       ; 0E (Retour arr.), 0F (Tab)
    db "azertyuiop"                 ; 10-19 (AZERTY)
    db 0, 0                          ; 1A, 1B
    db 0x0A, 0                       ; 1C (Entree), 1D (Ctrl)
    db "qsdfghjklm"                 ; 1E-27 (AZERTY)
    db 0, 0                          ; 28, 29
    db 0                             ; 2A (Maj gauche)
    db 0                             ; 2B
    db "wxcvbn"                      ; 2C-31 (AZERTY)
    db ",", ";", ":", "!"           ; 32-35
    db 0                             ; 36 (Maj droite)
    db "*"                           ; 37 (pave num.)
    db 0                             ; 38 (Alt)
    db " "                           ; 39 (Espace)

keymap_shift:
    db 0,    0                      ; 00, 01
    db "1234567890"                 ; 02-0B
    db "_", "+"                     ; 0C, 0D
    db 0x08, 0                       ; 0E, 0F
    db "AZERTYUIOP"                 ; 10-19
    db 0, 0                          ; 1A, 1B
    db 0x0A, 0                       ; 1C, 1D
    db "QSDFGHJKLM"                 ; 1E-27
    db 0, 0                          ; 28, 29
    db 0                             ; 2A
    db 0                             ; 2B
    db "WXCVBN"                      ; 2C-31
    db "?", ".", "/", "*"           ; 32-35
    db 0                             ; 36
    db "*"                           ; 37
    db 0                             ; 38
    db " "                           ; 39

; =============================================================================
;  DONNÉES NON INITIALISÉES (réservées en RAM)
; =============================================================================
section .bss
cursor_x      resb 1
cursor_y      resb 1
current_color resb 1
shift_state   resb 1
input_buffer  resb INPUT_MAX
