; =============================================================================
;  kernel/user_blobs.asm -- Embarque les binaires de test ring 3 (Phase 1)
;  Les .bin sont produits par nasm -f bin depuis user_tests/ (cf. Makefile).
; =============================================================================
[BITS 64]
section .rodata

global utest_a_start
global utest_a_end
global utest_b_start
global utest_b_end
global utest_crash_start
global utest_crash_end
global ugfx_start
global ugfx_end
global uwm_start
global uwm_end
global udesk_start
global udesk_end
global ucomp_start
global ucomp_end
global uclock_start
global uclock_end
global uhello_start
global uhello_end
global ucrash_start
global ucrash_end
global uterm_start
global uterm_end
global ufiles_start
global ufiles_end

utest_a_start:      incbin "build/obj/taskA.bin"
utest_a_end:
utest_b_start:      incbin "build/obj/taskB.bin"
utest_b_end:
utest_crash_start:  incbin "build/obj/crash.bin"
utest_crash_end:
ugfx_start:         incbin "build/obj/gfxdemo.elf"
ugfx_end:
uwm_start:          incbin "build/obj/wmserver.elf"
uwm_end:
udesk_start:        incbin "build/obj/desktop.elf"
udesk_end:
ucomp_start:        incbin "build/obj/compositor.elf"
ucomp_end:
uclock_start:       incbin "build/obj/app_clock.elf"
uclock_end:
uhello_start:       incbin "build/obj/app_hello.elf"
uhello_end:
ucrash_start:       incbin "build/obj/app_crash.elf"
ucrash_end:
uterm_start:        incbin "build/obj/term.elf"
uterm_end:
ufiles_start:       incbin "build/obj/files.elf"
ufiles_end:

section .note.GNU-stack noalloc noexec nowrite progbits
