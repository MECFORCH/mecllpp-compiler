/* ================================================================
 * FIRMAWORK x86-64  —  Önyükleme Giriş Noktası
 *
 * Zincir:
 *   Multiboot1 yükleme (32-bit Korumalı Mod)
 *     → GDT kurulumu
 *     → PAE + Sayfa tabloları (ilk 4 GB kimlik eşlemesi, 1GB sayfalar)
 *     → EFER.LME (Uzun Mod Enable)
 *     → CR0.PG  (Sayfalama)
 *     → 64-bit kod kesimine uzak atlama
 *     → main(mbi, magic) çağrısı
 *
 * Multiboot sağlama:
 *   flags    = 0x00000003
 *   checksum = -(0x1BADB002 + 0x3) mod 2^32 = 0xE4524FFB
 *
 * ÖNEMLİ: Multiboot başlığı .text.boot bölümünde, _start'tan
 * ÖNCE bulunmalı.  Bootloader bu üç 32-bit kelimeyi tarayarak
 * bulur; ELF ENTRY noktası (e_entry = _start) aracılığıyla
 * gerçek koda atlar.  CPU hiçbir zaman header baytlarını
 * komut olarak çalıştırmaz.
 * ================================================================ */
    /* Senin mevcut başlangıç kodların buraya gelecek */
    /* Örneğin: stack pointer kurma, main'e zıplama vb. */
/* ================================================================
 * .text.boot — 32-bit korumalı mod kodu
 *   (SHF_ALLOC + SHF_EXECINSTR → PT_LOAD segmentine dahil edilir,
 *    dolayısıyla Multiboot tarayıcısı sihirli sayıyı görür.)
 * ================================================================ */
.section .text.boot, "ax"
.code32

/* ================================================================
 * _start — 0x100000 adresindeki ELF giriş noktası
 *
 * İki boot modundan biri için çalışır:
 *
 *   [A] Multiboot / GRUB:
 *       GRUB, ELF e_entry = _start adresini okur ve buraya atlar.
 *       İlk komut: jmp _start_code  →  CPU header'ı atlar, koda gider.
 *       EAX = 0x2BADB002, EBX = Multiboot info işaretçisi.
 *
 *   [B] FIRMAWORK Özel Bootloader (Stage 2):
 *       Stage 2, 32-bit korumalı modda 0x100000'e atlar.
 *       İlk komut: jmp _start_code  →  aynı kurulum kodu çalışır.
 *       EAX = 0xF12AB007 ("FIRMAB00T"), EBX = 0.
 *
 * jmp _start_code: E9 XX XX XX XX  (5 bayt near JMP)
 * Multiboot başlığı sonrasında bir .align 4 → _start_code başlar.
 * ================================================================ */
.global _start
_start:
    jmp  _start_code                /* ← 0x100000: her iki boot modu için ilk komut */

/* ----------------------------------------------------------------
 * Multiboot 1 Başlığı  (dosyanın ilk 8 KB'ında, 4 bayt hizalı)
 * GRUB bu bloğu tarayarak bulur; CPU bu baytları komut olarak
 * çalıştırmaz çünkü _start'taki JMP onları atlatır.
 * ---------------------------------------------------------------- */
.align 4
multiboot_header:
    .long 0x1BADB002            /* sihirli sayı                    */
    .long 0x00000003            /* bayraklar: hizala + bellek haritası */
    .long 0xE4524FFB            /* sağlama: -(magic+flags) mod 2^32 */

/* ================================================================
 * _start_code — Gerçek 32-bit kurulum kodu
 *   Hem Multiboot hem de özel bootloader buraya atlar.
 * ================================================================ */
_start_code:
    cli                             /* kesmeler kapalı */

    /* Multiboot bilgilerini .data değişkenlerine kaydet
     * (henüz sayfalama yok → doğrudan fiziksel adres) */
    movl %eax, mb_saved_magic
    movl %ebx, mb_saved_info

    /* Geçici yığıt */
    movl $_stack_top, %esp

    /* ================================================================
     * Sayfa Tablolarını Kur
     *
     * PML4 → PDPT → 1 GB Devasa Sayfalar
     * İlk 4 GB kimlik eşlemesi (sanal == fiziksel).
     * Bayraklar: P=1 (bit0) | RW=1 (bit1) | PS=1 (bit7) = 0x83
     * Uzun Mod PDPT devasa sayfa işareti: 0x83 | 0x100 = 0x183
     * ================================================================ */

    /* PML4[0] → PDPT (P=1, RW=1) */
    movl $pdpt_table, %eax
    orl  $0x03, %eax
    movl %eax, pml4_table
    movl $0x00000000, pml4_table+4

    /* PDPT[0..3]: dörder 1 GB devasa sayfa → ilk 4 GB */
    movl $0x00000183, pdpt_table+0   /* 0x00000000–0x3FFFFFFF */
    movl $0x00000000, pdpt_table+4
    movl $0x40000183, pdpt_table+8   /* 0x40000000–0x7FFFFFFF */
    movl $0x00000000, pdpt_table+12
    movl $0x80000183, pdpt_table+16  /* 0x80000000–0xBFFFFFFF */
    movl $0x00000000, pdpt_table+20
    movl $0xC0000183, pdpt_table+24  /* 0xC0000000–0xFFFFFFFF */
    movl $0x00000000, pdpt_table+28

    /* ---- CR3 ← PML4 tablosunun fiziksel adresi ---- */
    movl $pml4_table, %eax
    movl %eax, %cr3

    /* ---- CR4.PAE = bit 5 ---- */
    movl %cr4, %eax
    orl  $0x20, %eax
    movl %eax, %cr4

    /* ---- EFER.LME = bit 8  (MSR 0xC0000080) ---- */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $0x00000100, %eax
    wrmsr

    /* ---- GDT yükle ---- */
    lgdt gdt64_ptr

    /* ---- CR0.PG = bit 31 → sayfalama açılır, uzun mod aktif ---- */
    movl %cr0, %eax
    orl  $0x80000000, %eax
    movl %eax, %cr0

    /* ---- 64-bit kod kesimine uzak atlama ---- */
    ljmp $0x08, $_start64

/* ================================================================
 * 64-bit Uzun Mod
 * ================================================================ */
.code64
_start64:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movq $_stack_top, %rsp

    /* .bss bölümünü sıfırla */
    movq $__bss_start, %rdi
    movq $__bss_end,   %rcx
    subq %rdi, %rcx
    xorq %rax, %rax
    test %rcx, %rcx
    jz   .Lbss_done
    rep  stosb
.Lbss_done:

    /* main(void *mbi, uint64_t magic) — System V AMD64 ABI */
    movq mb_saved_info(%rip),  %rdi
    movq mb_saved_magic(%rip), %rsi
    call main

.Lhalt:
    hlt
    jmp .Lhalt

/* ================================================================
 * GDT  (Global Tanımlayıcı Tablosu)
 *
 * 0x00: Null
 * 0x08: 64-bit kod  — P=1 DPL=0 S=1 Type=0xA L=1 G=1 D=0
 * 0x10: 64-bit veri — P=1 DPL=0 S=1 Type=0x2       G=1
 * ================================================================ */
.section .rodata
.align 8
gdt64:
    .quad 0x0000000000000000    /* 0x00: Null                   */
    .quad 0x00AF9A000000FFFF    /* 0x08: 64-bit kod (L=1, D=0)  */
    .quad 0x00CF92000000FFFF    /* 0x10: 64-bit veri            */
gdt64_end:

gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64

/* ================================================================
 * .data: Sayfa tabloları + saklı Multiboot değerleri
 *
 * NOT: Sayfa tabloları .bss'te OLMAMALI; 32-bit kodda
 * doldurulurlar, 64-bit geçişinden önce.  BSS temizleme
 * döngüsü bunları sıfırlamaz.
 * ================================================================ */
.section .data
.align 4096
pml4_table:    .fill 512, 8, 0
pdpt_table:    .fill 512, 8, 0

mb_saved_magic: .quad 0
mb_saved_info:  .quad 0

/* ================================================================
 * .bss: Yığıt
 * ================================================================ */
.section .bss
.align 16
__bss_start:
_stack_bottom: .skip 65536        /* 64 KB yığıt */
_stack_top:
__bss_end:
