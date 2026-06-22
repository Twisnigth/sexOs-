; =============================================================================
;  user_tests/taskA.asm -- Tâche ring 3 de test : imprime "A" puis exit(0)
;  Binaire « plat » chargé à 0x400000. N'utilise que les appels write/exit.
; =============================================================================
[BITS 64]
org 0x400000
_start:
    mov r12, 8                  ; nombre d'itérations
.loop:
    mov rax, 1                  ; write
    mov rdi, 1                  ; fd = stdout
    lea rsi, [rel msg]
    mov rdx, 2                  ; "A\n"
    syscall
    mov rcx, 0x1200000          ; petite attente active -> laisse le minuteur préempter
.delay:
    dec rcx
    jnz .delay
    dec r12
    jnz .loop
    mov rax, 60                 ; exit
    xor edi, edi               ; code 0
    syscall
.hang:
    jmp .hang
msg: db "A", 10
