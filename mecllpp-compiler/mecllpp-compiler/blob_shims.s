/* ================================================================
 * blob_shims.s — FIRMAWORK Mimari Blobu Gömücü (x86-64 hedef)
 *
 * fw_rv32.bin ve fw_a64.bin ham ikilileri .incbin ile gömülür.
 * Linker script bu bölümleri sabit adreslere yerleştirir:
 *   .arch_rv32 → 0x00200000
 *   .arch_a64  → 0x00300000
 *
 * main.c bu sembolleri extern olarak kullanır:
 *   rv32_blob_start / rv32_blob_end
 *   a64_blob_start  / a64_blob_end
 * ================================================================ */

.section .arch_rv32, "a", @progbits
.global rv32_blob_start
.global rv32_blob_end
rv32_blob_start:
    .incbin "fw_rv32.bin"
rv32_blob_end:

.section .arch_a64, "a", @progbits
.global a64_blob_start
.global a64_blob_end
a64_blob_start:
    .incbin "fw_a64.bin"
a64_blob_end:
