/* =============================================================================
 *  linker.ld  --  Script de liaison du noyau de sexOs
 * -----------------------------------------------------------------------------
 *  Le noyau est chargé par le bootloader à l'adresse physique 0x10000.
 *  On lie donc tout le code/données à partir de cette adresse, puis on
 *  extrait un binaire plat avec objcopy.
 *
 *  La section .text commence par la signature (kernel_magic) ; le vrai point
 *  d'entrée _start se trouve juste après (offset +4), ce qui correspond à
 *  l'adresse vers laquelle saute le bootloader (0x10004).
 * ============================================================================= */

ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /* On écarte les sections inutiles produites par les outils. */
    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
    }
}
