; =============================================================================
;  kernel/cpu.asm -- Routines bas niveau : chargement GDT/IDT/TSS
; =============================================================================
[BITS 64]

global gdt_flush
global idt_flush
global tss_flush

; void gdt_flush(uint64_t gdtr_ptr)  -- RDI = adresse du GDTR
gdt_flush:
    lgdt [rdi]
    ; Recharge les segments de données (sélecteur 0x10 = données noyau).
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ; Recharge CS via un "far return" (sélecteur 0x08 = code noyau).
    pop rax                 ; adresse de retour
    push qword 0x08         ; CS
    push rax                ; RIP
    retfq

; void idt_flush(uint64_t idtr_ptr)  -- RDI = adresse de l'IDTR
idt_flush:
    lidt [rdi]
    ret

; void tss_flush(uint16_t selector)  -- RDI = sélecteur du TSS
tss_flush:
    mov ax, di
    ltr ax
    ret

; Marque la pile comme non exécutable (silence l avertissement de l éditeur de liens).
section .note.GNU-stack noalloc noexec nowrite progbits
