; =============================================================================
;  kernel/usermode.asm -- Passage en ring 3 et point d'entrée des appels système
; =============================================================================
[BITS 64]

global enter_user
global syscall_entry
global user_exit
global kernel_rsp

extern syscall_dispatch

%define USER_CS 0x1b        ; sélecteur code utilisateur (0x18 | RPL 3)
%define USER_SS 0x23        ; sélecteur données utilisateur (0x20 | RPL 3)

section .data
; Contexte noyau sauvegardé par enter_user, restauré par user_exit (longjmp).
saved_rsp:  dq 0
saved_rbx:  dq 0
saved_rbp:  dq 0
saved_r12:  dq 0
saved_r13:  dq 0
saved_r14:  dq 0
saved_r15:  dq 0
kernel_rsp: dq 0            ; pile noyau pour l'entrée syscall
user_rsp:   dq 0            ; pile utilisateur sauvegardée pendant un syscall
ret_rip:    dq 0
ret_rflags: dq 0
retval:     dq 0

section .text
; void enter_user(uint64_t entry /*rdi*/, uint64_t user_stack /*rsi*/)
;  Bascule en ring 3. Revient (comme un appel normal) quand l'utilisateur
;  effectue l'appel système exit.
enter_user:
    ; Sauvegarde du contexte noyau (registres callee-saved + pile) pour pouvoir
    ; y revenir depuis user_exit sans corrompre l'appelant.
    mov [saved_rbx], rbx
    mov [saved_rbp], rbp
    mov [saved_r12], r12
    mov [saved_r13], r13
    mov [saved_r14], r14
    mov [saved_r15], r15
    mov [saved_rsp], rsp
    ; segments de données utilisateur
    mov ax, USER_SS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    ; cadre iretq : SS, RSP, RFLAGS, CS, RIP
    push USER_SS
    push rsi                    ; pile utilisateur
    push 0x202                  ; RFLAGS (IF=1)
    push USER_CS
    push rdi                    ; point d'entrée
    iretq

; Point d'entrée de l'instruction syscall (ring 0).
;  Construit un registers_t IDENTIQUE à celui des interruptions (cf. isr.asm) sur
;  la pile noyau de la tâche, puis appelle syscall_enter(registers_t*) qui renvoie
;  le contexte à reprendre (la MÊME tâche, ou une AUTRE si l'appel bloque/cède).
extern syscall_enter
syscall_entry:
    mov [user_rsp], rsp         ; sauve la pile utilisateur (IF=0 : atomique)
    mov rsp, [kernel_rsp]       ; pile noyau de la tâche courante
    ; Cadre iretq (ss, rsp, rflags, cs, rip) + err/int factices.
    push USER_SS
    push qword [user_rsp]
    push r11                    ; RFLAGS sauvegardé par syscall
    push USER_CS
    push rcx                    ; RIP de retour sauvegardé par syscall
    push qword 0                ; err_code
    push qword 0                ; int_no (0 = appel système)
    ; Registres généraux (même ordre que isr_common).
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
    cld
    mov rdi, rsp                ; registers_t* courant
    call syscall_enter         ; -> registers_t* à reprendre (rax)
    mov rsp, rax
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
    add rsp, 16                 ; saute int_no + err_code
    iretq

; void user_exit(void) -- restaure le contexte noyau et retourne dans enter_user.
user_exit:
    mov rbx, [saved_rbx]
    mov rbp, [saved_rbp]
    mov r12, [saved_r12]
    mov r13, [saved_r13]
    mov r14, [saved_r14]
    mov r15, [saved_r15]
    mov rsp, [saved_rsp]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
