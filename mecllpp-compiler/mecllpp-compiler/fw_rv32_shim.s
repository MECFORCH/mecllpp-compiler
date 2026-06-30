# ================================================================
# fw_rv32_shim.s — FIRMAWORK RISC-V 32 Mikro-Kernel Blobu
#
# Yükleme adresi : 0x80000000  (QEMU virt RV32 RAM)
# UART           : NS16550A @ 0x10000000  (QEMU virt varsayılanı)
# Baud            : 115200 (DLL=1, DLM=0)
#
# Derleme (calistir.sh içinden otomatik):
#   clang --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
#         -nostdlib -ffreestanding -fno-stack-protector           \
#         fw_rv32_shim.s -o fw_rv32_shim.elf                     \
#         -Wl,-Ttext=0x80000000
#   llvm-objcopy -O binary fw_rv32_shim.elf fw_rv32.bin
# ================================================================

.section .text.start, "ax", @progbits
.global _start
_start:
    # Yığın: QEMU virt RV32 RAM 0x80000000-0x88000000 → tepesi
    lui   sp, 0x80800           # sp = 0x80800000

    # ---- NS16550A başlatma ----
    lui   t0, 0x10000           # t0 = 0x10000000 (UART tabanı)
    li    t1, 0x83              # LCR: DLAB=1, 8N1
    sb    t1, 12(t0)
    li    t1, 1                 # DLL = 1 (115200 @ 1.8432 MHz)
    sb    t1, 0(t0)
    sb    zero, 4(t0)           # DLM = 0
    li    t1, 0x03              # LCR: 8 bit, parite yok, 1 stop, DLAB=0
    sb    t1, 12(t0)
    li    t1, 0xC7              # FCR: FIFO etkin, 14-bayt eşiği, temizle
    sb    t1, 8(t0)

    # ---- Karşılama mesajı ----
    lui   a0, %hi(msg_init)
    addi  a0, a0, %lo(msg_init)
    call  rv_puts

    # ---- Sonsuz echo döngüsü ----
echo_loop:
    call  rv_getc               # a0 = okunan karakter
    call  rv_putc               # a0'ı geri yaz
    j     echo_loop

# ----------------------------------------------------------------
# rv_getc() → a0 : NS16550 RBR'dan karakter oku (LSR bit0 bekle)
# ----------------------------------------------------------------
rv_getc:
    lui   t0, 0x10000
1:  lbu   t1, 20(t0)           # LSR @ +0x14 (byte offset 20)
    andi  t1, t1, 1             # bit0 = Data Ready
    beqz  t1, 1b
    lbu   a0, 0(t0)             # RBR @ +0x00
    ret

# ----------------------------------------------------------------
# rv_putc(a0) : NS16550 THR'a karakter yaz (LSR bit5 bekle)
# ----------------------------------------------------------------
rv_putc:
    lui   t0, 0x10000
1:  lbu   t1, 20(t0)           # LSR
    andi  t1, t1, 0x20          # bit5 = TX Empty
    beqz  t1, 1b
    sb    a0, 0(t0)             # THR @ +0x00
    ret

# ----------------------------------------------------------------
# rv_puts(a0) : null-sonlandırmalı dizeyi yaz
# ----------------------------------------------------------------
rv_puts:
    addi  sp, sp, -16
    sw    ra, 12(sp)
    sw    s0, 8(sp)
    mv    s0, a0
.puts_next:
    lbu   a0, 0(s0)
    beqz  a0, .puts_done
    call  rv_putc
    addi  s0, s0, 1
    j     .puts_next
.puts_done:
    lw    ra, 12(sp)
    lw    s0, 8(sp)
    addi  sp, sp, 16
    ret

.section .rodata
msg_init:
    .asciz "\r\n[FIRMAWORK] RISC-V 32 blogu aktif\r\nNS16550 @ 0x10000000 | 115200-8N1\r\nEcho: her karakter yankilanir | Cikis: Ctrl+A X\r\n> "
