/* ================================================================
 * FIRMAWORK x86-64 Linker Script
 *
 * Yükleme adresi : 0x100000 (1 MB) — Multiboot standardı
 *
 * Bellek haritası:
 *   0x00100000  x86-64 firmware kodu  (.text .rodata .data .bss)
 *   0x00200000  RISC-V 32 mikro-kernel blobu  (.arch_rv32)
 *   0x00300000  AArch64 mikro-kernel blobu    (.arch_a64)
 *
 * NOT: Ayrı bir .multiboot bölümü yok.  Multiboot başlığı
 * .text.boot'un en başında yer alır ve PT_LOAD segmentine
 * dahil edildiği için QEMU tarayıcısı tarafından bulunur.
 * ================================================================ */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x100000, LENGTH = 16M
}

SECTIONS
{
    . = 0x100000;

    .text ALIGN(4) : {
        *(.text.boot)
        *(.text*)
    } > RAM

    .rodata ALIGN(4096) : {
        *(.rodata*)
    } > RAM

    .data ALIGN(4096) : {
        *(.data*)
    } > RAM

    .bss ALIGN(4096) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM

    _end = .;

    /* .arch_rv32 / .arch_a64 ELF'e gömülmüyor.
     * fw_rv32.bin ve fw_a64.bin ayrı dosya olarak QEMU -device loader
     * ile 0x200000 ve 0x300000'e yüklenir (firmware.bin şişmez). */

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
