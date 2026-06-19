/* Script de lien des programmes ring 3 sexOs : chargés à 0x400000. */
ENTRY(_start)
SECTIONS {
    . = 0x400000;
    .text   : { *(.text*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
}
