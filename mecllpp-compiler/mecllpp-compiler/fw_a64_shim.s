// ================================================================
// fw_a64_shim.s — FIRMAWORK AArch64 Mikro-Kernel Blobu
//
// Yükleme adresi : 0x40000000  (QEMU virt AArch64 RAM)
// UART           : PL011 @ 0x09000000  (QEMU virt varsayılanı)
//   UARTDR  = +0x00   (Veri: RX/TX)
//   UARTFR  = +0x18   (Bayraklar: bit4=RXFE, bit5=TXFF)
//   UARTCR  = +0x30   (Kontrol: bit0=UARTEN, bit8=TXE, bit9=RXE)
//
// Derleme (calistir.sh içinden otomatik):
//   clang --target=aarch64-elf -nostdlib -ffreestanding     \
//         -fno-stack-protector fw_a64_shim.s -o fw_a64.elf  \
//         -Wl,-Ttext=0x40000000
//   llvm-objcopy -O binary fw_a64.elf fw_a64.bin
// ================================================================

.section .text.start, "ax"
.global _start
_start:
    // Yığın kur: 0x40000000 altında güvenli bölge
    movz x0, #0x4005, lsl #16  // x0 = 0x40050000
    mov  sp, x0

    // PL011 başlat: UARTCR = UARTEN(bit0)|TXE(bit8)|RXE(bit9) = 0x301
    movz x1, #0x0900, lsl #16  // x1 = 0x09000000 (PL011 tabanı)
    mov  w2, #0x301
    str  w2, [x1, #0x30]       // UARTCR

    // Karşılama mesajı
    adr  x0, msg_init
    bl   a64_puts

    // Sonsuz echo döngüsü
echo_loop:
    bl   a64_getc
    bl   a64_putc
    b    echo_loop

// ----------------------------------------------------------------
// w0 = a64_getc() : PL011 UARTDR'dan karakter oku (RXFE=0 bekle)
// ----------------------------------------------------------------
a64_getc:
    movz x1, #0x0900, lsl #16
1:  ldr  w2, [x1, #0x18]       // UARTFR
    tst  w2, #0x10              // RXFE (RX FIFO boş)
    b.ne 1b
    ldr  w0, [x1, #0]           // UARTDR
    ret

// ----------------------------------------------------------------
// a64_putc(w0) : PL011 UARTDR'a karakter yaz (TXFF=0 bekle)
// ----------------------------------------------------------------
a64_putc:
    movz x1, #0x0900, lsl #16
1:  ldr  w2, [x1, #0x18]       // UARTFR
    tst  w2, #0x20              // TXFF (TX FIFO dolu)
    b.ne 1b
    str  w0, [x1, #0]           // UARTDR
    ret

// ----------------------------------------------------------------
// a64_puts(x0) : null-sonlandırmalı dizeyi yaz
// ----------------------------------------------------------------
a64_puts:
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    str  x19, [sp, #16]
    mov  x19, x0
.puts_loop:
    ldrb w0, [x19], #1
    cbz  w0, .puts_done
    bl   a64_putc
    b    .puts_loop
.puts_done:
    ldr  x19, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret

.section .rodata
msg_init:
    .asciz "\r\n[FIRMAWORK] AArch64 blogu aktif\r\nPL011 @ 0x09000000 | 115200-8N1\r\nEcho: her karakter yankilanir | Cikis: Ctrl+A X\r\n> "
