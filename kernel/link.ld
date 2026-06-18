/* =============================================================================
 *  kernel/link.ld -- Script de liaison du noyau MonOS (x86_64, protocole Limine)
 * -----------------------------------------------------------------------------
 *  Le noyau est un ELF64 chargé par Limine dans la moitié haute de l'espace
 *  d'adressage virtuel (higher-half), à partir de 0xffffffff80000000.
 *  Limine met en place le long mode, une pagination initiale et mappe le noyau
 *  à cette adresse ; on n'a donc qu'à respecter cette convention.
 *
 *  La section .limine_requests regroupe les requêtes adressées au bootloader
 *  (encadrées par des marqueurs de début/fin) afin que Limine les retrouve.
 * ============================================================================= */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS
{
    text    PT_LOAD;
    rodata  PT_LOAD;
    data    PT_LOAD;
}

SECTIONS
{
    /* Moitié haute : -2 Gio. */
    . = 0xffffffff80000000;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    /* Requêtes Limine : encadrées par leurs marqueurs. */
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /* Sections inutiles pour un noyau autonome. */
    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
