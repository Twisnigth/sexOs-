; =============================================================================
;  kernel/isr.asm -- Stubs des interruptions (exceptions 0-31 + IRQ 32-47)
; -----------------------------------------------------------------------------
;  Chaque stub empile un éventuel code d'erreur factice puis le numéro de
;  vecteur, et saute vers une routine commune qui sauvegarde tous les registres
;  et appelle le répartiteur C (isr_dispatch).
; =============================================================================
[BITS 64]

extern isr_dispatch

; Stub pour les vecteurs SANS code d'erreur : on empile un 0 factice.
%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

; Stub pour les vecteurs AVEC code d'erreur (déjà empilé par le CPU).
%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1
    jmp isr_common
%endmacro

; Exceptions 0-31 (selon qu'elles empilent ou non un code d'erreur).
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

; IRQ 0-15 -> vecteurs 32-47 (pas de code d'erreur).
%macro IRQ 2
global irq%1
irq%1:
    push qword 0
    push qword %2
    jmp isr_common
%endmacro

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; ---- Routine commune --------------------------------------------------------
isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp            ; 1er argument = pointeur sur les registres sauvegardés
    cld
    call isr_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16             ; retire int_no et err_code
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
