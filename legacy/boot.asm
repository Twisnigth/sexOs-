; =============================================================================
;  boot.asm  --  Bootloader (secteur d'amorçage 512 octets) pour MonOS
; -----------------------------------------------------------------------------
;  Rôle :
;    1. Démarre en mode réel 16 bits (le BIOS nous charge à 0x7C00).
;    2. Charge le noyau depuis le disque via INT 0x13 (cas disquette / disque dur)
;       OU détecte que le noyau est déjà présent en mémoire (cas démarrage ISO
;       El Torito « no-emulation », où le BIOS a chargé toute l'image à 0x7C00).
;    3. Active la ligne A20.
;    4. Installe une GDT plate (segments 4 Gio).
;    5. Bascule en mode protégé 32 bits.
;    6. Saute vers le point d'entrée du noyau.
;
;  Assemblé en binaire plat : nasm -f bin boot.asm -o boot.bin
; =============================================================================

[BITS 16]
[ORG 0x7C00]

; ---- Constantes -------------------------------------------------------------
KERNEL_MAGIC   equ 0xC0DEBED5      ; signature placée aux 4 premiers octets du noyau
KERNEL_LOAD    equ 0x10000         ; adresse physique finale du noyau
KERNEL_ENTRY   equ KERNEL_LOAD + 4 ; on saute après la signature (4 octets)
KERNEL_SECTORS equ 64              ; nombre de secteurs (512 o) à charger = 32 Kio
CD_TMP         equ 0x7E00          ; emplacement où l'ISO charge le noyau (juste après nous)

; Géométrie disquette 1.44 Mio utilisée pour la conversion LBA -> CHS
SPT            equ 18              ; secteurs par piste
HEADS          equ 2              ; nombre de têtes

; =============================================================================
;  Point d'entrée (mode réel 16 bits)
; =============================================================================
start:
    cli                            ; pas d'interruptions pendant l'init
    xor ax, ax
    mov ds, ax                     ; DS = ES = SS = 0
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00                 ; pile juste sous le code de boot
    mov [boot_drive], dl           ; le BIOS nous donne le n° de disque dans DL
    sti

    mov si, msg_boot               ; petit message d'amorçage
    call print16

    ; --- Le noyau est-il déjà en mémoire à 0x7E00 ? (démarrage depuis l'ISO) ---
    mov eax, [CD_TMP]
    cmp eax, KERNEL_MAGIC
    je  .from_cd

    ; --- Sinon : chargement classique depuis le disque via INT 0x13 ----------
    mov byte [from_cd], 0
    call load_kernel_disk
    jmp  .loaded

.from_cd:
    mov byte [from_cd], 1          ; le noyau sera recopié depuis 0x7E00 en mode protégé

.loaded:
    call enable_a20                ; activation de la ligne A20

    cli
    lgdt [gdt_descriptor]          ; chargement de la GDT

    mov eax, cr0                   ; bit PE (Protection Enable) de CR0
    or  al, 1
    mov cr0, eax

    jmp CODE_SEG:protected_mode    ; saut « far » -> vide le pipeline, charge CS

; =============================================================================
;  Affichage d'une chaîne en mode réel (BIOS téléscripteur, INT 0x10/0x0E)
;  Entrée : SI = pointeur sur chaîne terminée par 0
; =============================================================================
print16:
    pusha
.next:
    lodsb
    or  al, al
    jz  .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .next
.done:
    popa
    ret

; =============================================================================
;  Chargement du noyau depuis le disque, secteur par secteur (INT 0x13, AH=2)
;  Destination : ES:BX = 0x1000:0000 = adresse physique 0x10000
;  On lit KERNEL_SECTORS secteurs à partir du LBA 1 (le secteur 0 = ce boot).
; =============================================================================
load_kernel_disk:
    mov ax, 0x1000
    mov es, ax                     ; ES = 0x1000  -> ES:BX pointe sur 0x10000
    xor bx, bx
    mov word [lba], 1              ; on commence au LBA 1 (secteur logique 2)
    mov cx, KERNEL_SECTORS         ; compteur de secteurs restants
.next_sector:
    push cx

    ; --- Conversion LBA -> CHS -----------------------------------------------
    mov ax, [lba]
    xor dx, dx
    mov cx, SPT
    div cx                         ; AX = LBA / SPT ; DX = LBA % SPT
    mov [sect], dl
    inc byte [sect]                ; secteur CHS = (LBA % SPT) + 1  (base 1)
    xor dx, dx
    mov cx, HEADS
    div cx                         ; AX = cylindre ; DX = tête
    mov [cyl], al
    mov [head], dl

    ; --- Lecture d'un secteur -------------------------------------------------
    mov ah, 0x02                   ; fonction « lire des secteurs »
    mov al, 1                      ; un seul secteur à la fois (robuste)
    mov ch, [cyl]
    mov cl, [sect]
    mov dh, [head]
    mov dl, [boot_drive]
    int 0x13
    jc  .disk_error                ; CF=1 -> erreur

    add bx, 512                    ; secteur suivant en mémoire
    inc word [lba]
    pop cx
    loop .next_sector
    ret

.disk_error:
    ; réinitialisation du contrôleur disque puis nouvelle tentative
    xor ah, ah
    mov dl, [boot_drive]
    int 0x13
    pop cx
    jmp .next_sector

; =============================================================================
;  Activation de la ligne A20 par la méthode rapide (port système 0x92)
; =============================================================================
enable_a20:
    in  al, 0x92
    or  al, 00000010b
    out 0x92, al
    ret

; =============================================================================
;  Table des descripteurs globaux (GDT) : modèle plat 0..4 Gio
; =============================================================================
gdt_start:
    dq 0x0000000000000000          ; descripteur nul obligatoire

gdt_code:                          ; segment de code : base=0, limite=4Gio
    dw 0xFFFF                       ; limite 0:15
    dw 0x0000                       ; base 0:15
    db 0x00                         ; base 16:23
    db 10011010b                    ; présent, anneau 0, exécutable, lecture
    db 11001111b                    ; granularité 4K, 32 bits, limite 16:19
    db 0x00                         ; base 24:31

gdt_data:                          ; segment de données : base=0, limite=4Gio
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b                    ; présent, anneau 0, données, écriture
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1      ; taille de la GDT - 1
    dd gdt_start                    ; adresse de la GDT

CODE_SEG equ gdt_code - gdt_start   ; sélecteur 0x08
DATA_SEG equ gdt_data - gdt_start   ; sélecteur 0x10

; =============================================================================
;  Code 32 bits (mode protégé)
; =============================================================================
[BITS 32]
protected_mode:
    mov ax, DATA_SEG               ; recharge tous les segments de données
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000               ; pile noyau confortable

    ; --- Cas ISO : recopier le noyau de 0x7E00 vers 0x10000 ------------------
    cmp byte [from_cd], 1
    jne .jump_kernel
    mov esi, CD_TMP
    mov edi, KERNEL_LOAD
    mov ecx, (KERNEL_SECTORS * 512) / 4
    rep movsd

.jump_kernel:
    jmp KERNEL_ENTRY               ; saut vers le noyau (après sa signature)

; =============================================================================
;  Données du bootloader
; =============================================================================
boot_drive db 0
from_cd    db 0
lba        dw 0
cyl        db 0
head       db 0
sect       db 0
msg_boot   db "MonOS : amorcage...", 13, 10, 0

; --- Remplissage jusqu'à 510 octets + signature de boot 0xAA55 ---------------
times 510 - ($ - $$) db 0
dw 0xAA55
