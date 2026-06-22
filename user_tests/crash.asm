; =============================================================================
;  user_tests/crash.asm -- Tâche ring 3 fautive : exécute une instruction
;  privilégiée (cli). En ring 3 -> #GP. Prouve la séparation des privilèges :
;  le noyau doit TUER cette tâche sans planter (les autres continuent).
; =============================================================================
[BITS 64]
org 0x400000
_start:
    mov rax, 1
    mov rdi, 1
    lea rsi, [rel msg]
    mov rdx, 6
    syscall
    cli                         ; instruction privilégiée -> #GP depuis le ring 3
.hang:
    jmp .hang                   ; jamais atteint (la tâche est tuée)
msg: db "C cli", 10
