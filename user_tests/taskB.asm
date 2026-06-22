; =============================================================================
;  user_tests/taskB.asm -- Tâche ring 3 de test : imprime "B" puis exit(0)
; =============================================================================
[BITS 64]
org 0x400000
_start:
    mov r12, 8
.loop:
    mov rax, 1
    mov rdi, 1
    lea rsi, [rel msg]
    mov rdx, 2
    syscall
    mov rcx, 0x1200000
.delay:
    dec rcx
    jnz .delay
    dec r12
    jnz .loop
    mov rax, 60
    xor edi, edi
    syscall
.hang:
    jmp .hang
msg: db "B", 10
