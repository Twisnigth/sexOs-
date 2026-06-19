; =============================================================================
;  kernel/switch.asm -- Primitives de commutation de contexte (Phase 1)
; -----------------------------------------------------------------------------
;  Un contexte de tâche est un registers_t (cf. idt.h) posé sur la pile noyau de
;  la tâche. Le restaurer consiste à dépiler tous les registres puis iretq.
; =============================================================================
[BITS 64]

global sched_resume
global sched_save_and_run
global sched_return

section .data
; Contexte noyau de l'appelant de sched_save_and_run (style setjmp/longjmp).
sched_saved_rbx: dq 0
sched_saved_rbp: dq 0
sched_saved_r12: dq 0
sched_saved_r13: dq 0
sched_saved_r14: dq 0
sched_saved_r15: dq 0
sched_saved_rsp: dq 0

section .text

; Dépile un registers_t (pointé par rdi) et reprend la tâche. Ne revient pas.
%macro RESTORE_AND_IRET 0
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
    add rsp, 16          ; saute int_no + err_code
    iretq
%endmacro

; void sched_resume(registers_t *ctx)  -- rdi = ctx
sched_resume:
    mov rsp, rdi
    RESTORE_AND_IRET

; void sched_save_and_run(registers_t *first_ctx)  -- rdi = first_ctx
;  Sauvegarde le contexte noyau de l'appelant puis bascule sur la 1re tâche.
;  Revient à l'appelant (comme un appel normal) via sched_return.
sched_save_and_run:
    mov [sched_saved_rbx], rbx
    mov [sched_saved_rbp], rbp
    mov [sched_saved_r12], r12
    mov [sched_saved_r13], r13
    mov [sched_saved_r14], r14
    mov [sched_saved_r15], r15
    mov [sched_saved_rsp], rsp
    mov rsp, rdi
    RESTORE_AND_IRET

; void sched_return(void) -- restaure le contexte noyau sauvegardé et revient
;  (sortie de sched_save_and_run / sched_run_until_idle).
sched_return:
    mov rbx, [sched_saved_rbx]
    mov rbp, [sched_saved_rbp]
    mov r12, [sched_saved_r12]
    mov r13, [sched_saved_r13]
    mov r14, [sched_saved_r14]
    mov r15, [sched_saved_r15]
    mov rsp, [sched_saved_rsp]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
