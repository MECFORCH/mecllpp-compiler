/* ================================================================
 * FIRMAWORK x86-64 Linker Script
 *
 * Yükleme adresi : 0x100000 (1 MB) — Multiboot standardı
 *
 * Bellek haritası:
 *   0x00100000  x86-64 firmware kodu  (.text .rodata .data .bss)
 *   0x00200000  RISC-V 32 mikro-kernel blobu  (.arch_rv32  NOLOAD)
 *   0x00300000  AArch64 mikro-kernel blobu    (.arch_a64   NOLOAD)
 *
 * NOLOAD nedir?
 *   Bu bölümler VMA'ya (sanal adres) yerleştirilir ama PT_LOAD
 *   segment'i oluşturulmaz — dosyaya YAZILMAZ.  Stub (.incbin 40B)
 *   veya gerçek blob fark etmeksizin firmware.elf/bin şişmez.
 *   Blob'lar QEMU tarafından ayrı olarak yüklenir:
 *     -device loader,file=fw_rv32.bin,addr=0x200000,force-raw=on
 *     -device loader,file=fw_a64.bin ,addr=0x300000,force-raw=on
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

    /* ----------------------------------------------------------------
     * Mimari blob rezervasyonları — NOLOAD
     *
     * VMA tahsis edilir, dosyaya yazılmaz (PT_LOAD yok).
     * Stub (.incbin 40 B) veya gerçek blob fark etmeksizin
     * firmware.bin / firmware-32.elf şişmez; 1MB+1MB gap oluşmaz.
     *
     * rv32_blob_start/end  ve  a64_blob_start/end  sembolleri
     * blob_shims.s'deki etiketlerden gelir — main.c bunları extern
     * kullanarak boyut ve adres bilgisi alır.
     * ---------------------------------------------------------------- */
    .arch_rv32 0x200000 (NOLOAD) : {
        *(.arch_rv32)
    } > RAM

    .arch_a64 0x300000 (NOLOAD) : {
        *(.arch_a64)
    } > RAM

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
