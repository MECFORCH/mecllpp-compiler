/* ================================================================
 * FIRMAWORK x86-64 Linker Script
 *
 * Yükleme adresi : 0x100000 (1 MB) — Multiboot standardı
 *
 * Bellek haritası:
 *   0x00100000  .text / .rodata / .data  (x86-64 firmware kodu)
 *   ...         .bss
 *   ...+4K      .arch_rv32  (RV32 blobu — .bss'ten hemen sonra, boşluksuz)
 *   ...+4K      .arch_a64   (A64 blobu  — rv32'den hemen sonra, boşluksuz)
 *
 * Blokları .bss'ten hemen sonra (boşluksuz) yerleştirerek 2x1MB
 * gap ve şişkin firmware.bin sorununu ortadan kaldırıyoruz.
 * rv32_blob_start/end ve a64_blob_start/end sembolleri linker
 * tarafından atanır; main.c bunları extern olarak kullanır.
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

    /* ----------------------------------------------------------------
     * Mimari blob bölümleri — .bss'ten hemen sonra, boşluksuz.
     *
     * Sabit adres (0x200000 / 0x300000) kullanmadığımız için
     * firmware.bin içinde 2x1MB dolgu oluşmaz.
     * Semboller (rv32_blob_start/end, a64_blob_start/end)
     * blob_shims.s içindeki etiketlerden gelir; doğrudan
     *   extern char rv32_blob_start[];
     * ile main.c'den erişilir.
     * ---------------------------------------------------------------- */
    .arch_rv32 ALIGN(4096) : {
        KEEP(*(.arch_rv32))
    } > RAM

    .arch_a64 ALIGN(4096) : {
        KEEP(*(.arch_a64))
    } > RAM

    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
