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

utest_a_start:      incbin "build/obj/taskA.bin"
utest_a_end:
utest_b_start:      incbin "build/obj/taskB.bin"
utest_b_end:
utest_crash_start:  incbin "build/obj/crash.bin"
utest_crash_end:
ugfx_start:         incbin "build/obj/gfxdemo.elf"
ugfx_end:

section .note.GNU-stack noalloc noexec nowrite progbits
