; =============================================================================
;  user/lib/crt0.asm -- Amorce des programmes ring 3 sexOs
;  Appelle main(), puis exit(code de retour).
; =============================================================================
[BITS 64]
global _start
extern main

_start:
    xor ebp, ebp
    call main
    mov edi, eax            ; code de retour de main
    mov eax, 60             ; SYS_exit (ABI Linux)
    syscall
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
