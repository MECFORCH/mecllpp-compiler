#!/bin/sh
# ================================================================
# calistir.sh — FIRMAWORK Build + QEMU Pipeline  v11
#               Çapraz Platform (x86-64 / RISC-V 32 / AArch64)
#
# Kullanım:
#   sh calistir.sh [seçenekler]
#
# Seçenekler:
#   --arch=x86_64    x86-64 Long Mode (varsayılan)
#   --arch=riscv32   RISC-V RV32I, QEMU virt makinesi
#   --arch=aarch64   AArch64 ARMv8-A, QEMU virt makinesi
#
#   --boot=multiboot x86-64: QEMU -kernel ile GRUB (varsayılan)
#   --boot=custom    x86-64: kendi stage1/stage2 bootloader zinciri
#
#   --vga            x86-64: GTK VGA penceresi aç (multiboot modunda)
#
# Ortam değişkenleri:
#   FIAWO=dosya.fiawo    → Segment 0 @ 0x80200000
#   FIAWO1=dosya.fiawo   → Segment 1 @ 0x80300000 (sadece x86-64)
#
# Örnekler:
#   sh calistir.sh
#   sh calistir.sh --arch=riscv32 FIAWO=kod.fiawo
#   sh calistir.sh --arch=aarch64 FIAWO=kod.fiawo
#   BOOT_MODE=custom sh calistir.sh
#   VGA_MODE=1 sh calistir.sh
#
# Çıkış: Ctrl+A, ardından X (tüm modlarda)
# ================================================================
set -e

# ----------------------------------------------------------------
# Argüman ayrıştırma
# ----------------------------------------------------------------
ARCH="${ARCH:-x86_64}"
BOOT_MODE="${BOOT_MODE:-multiboot}"
VGA_MODE="${VGA_MODE:-0}"

for ARG in "$@"; do
    case "$ARG" in
        --arch=x86_64)  ARCH=x86_64   ;;
        --arch=riscv32) ARCH=riscv32  ;;
        --arch=aarch64) ARCH=aarch64  ;;
        --boot=custom)  BOOT_MODE=custom ;;
        --boot=multiboot) BOOT_MODE=multiboot ;;
        --vga)          VGA_MODE=1 ;;
        *) echo "[uyarı] Bilinmeyen seçenek yoksayıldı: $ARG" ;;
    esac
done

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "  FIRMAWORK Build Pipeline  v11  —  Çapraz Platform"
echo "  Hedef Mimari : $ARCH"
case "$ARCH" in
    x86_64)  echo "  Boot Modu    : $BOOT_MODE" ;;
    riscv32) echo "  QEMU Makinesi: virt  (RV32I, NS16550 UART @ 0x10000000)" ;;
    aarch64) echo "  QEMU Makinesi: virt  (Cortex-A57, PL011 @ 0x09000000)" ;;
esac
echo "════════════════════════════════════════════════════════════════"
echo ""

# ================================================================
# ████████████████████████  x86-64  ██████████████████████████████
# ================================================================
if [ "$ARCH" = "x86_64" ]; then

# ----------------------------------------------------------------
# [1/5] Araçları doğrula
# ----------------------------------------------------------------
echo "[1/5] Araçlar kontrol ediliyor..."
for TOOL in clang ld.lld objcopy objdump qemu-system-x86_64; do
    if ! $TOOL --version > /dev/null 2>&1; then
        echo "      [HATA] $TOOL bulunamadı." >&2; exit 1
    fi
done
echo "      Derleyici : $(clang --version | head -1)"
echo "      Linker    : $(ld.lld --version | head -1)"
echo "      QEMU      : $(qemu-system-x86_64 --version | head -1)"
echo ""

# ----------------------------------------------------------------
# [2/6] Mimari blob shimler
# ----------------------------------------------------------------
echo "[2/6] Mimari blob shimler üretiliyor (RV32 + A64)..."

# gen_blobs.c: çapraz derleyici gerektirmeden blob üretir
gcc gen_blobs.c -o gen_blobs 2>/dev/null && ./gen_blobs
echo "      fw_rv32.bin : $(wc -c < fw_rv32.bin) bayt  (VMA 0x200000)"
echo "      fw_a64.bin  : $(wc -c < fw_a64.bin) bayt  (VMA 0x300000)"
echo ""

echo "[3/6] Kaynak dosyaları derleniyor..."

CFLAGS="--target=x86_64-elf -ffreestanding -fno-stack-protector
        -mno-red-zone -mno-mmx -mno-sse -mno-sse2
        -nostdlib -nostdinc -c"

NIX_HARDENING_ENABLE='' clang $CFLAGS entry.s -o entry.o \
    2>&1 | grep -vE "^(Warning:|clang: warning)" || true
echo "      entry.s  → entry.o"

NIX_HARDENING_ENABLE='' clang $CFLAGS main.c  -o main.o  \
    2>&1 | grep -vE "^(Warning:|clang: warning)" || true
echo "      main.c   → main.o"

# vga.c isteğe bağlı
if [ -f vga.c ]; then
    NIX_HARDENING_ENABLE='' clang $CFLAGS vga.c -o vga.o  \
        2>&1 | grep -vE "^(Warning:|clang: warning)" || true
    echo "      vga.c    → vga.o"
    VGA_OBJ="vga.o"
else
    VGA_OBJ=""
fi
echo ""

# ----------------------------------------------------------------
# [4/6] Linkle
# ----------------------------------------------------------------
echo "[4/6] Linkleniyor (ld.lld)..."

# shellcheck disable=SC2086
ld.lld -T linker.ld -nostdlib entry.o main.o blob_shims.o $VGA_OBJ -o firmware.elf
echo "      firmware.elf : $(wc -c < firmware.elf) bayt (ELF64)"
echo ""
echo "      Bölüm haritası:"
llvm-objdump --section-headers firmware.elf 2>/dev/null \
    | awk '/^\s+[0-9]/{printf "        %-20s  vma=%s  boyut=%s\n", $2, $5, $3}' \
    || true
echo ""

# ----------------------------------------------------------------
# [5/6] ELF64 → ELF32
# ----------------------------------------------------------------
echo "[5/6] ELF64 → ELF32 (QEMU multiboot uyumu)..."
llvm-objcopy -O elf32-i386 firmware.elf firmware-32.elf
echo "      firmware-32.elf : $(wc -c < firmware-32.elf) bayt (ELF32)"
echo ""

# ----------------------------------------------------------------
# Mimari bloklar artık firmware.bin içine gömülü (NOLOAD kaldırıldı)
# — ayrı loader bayrağına gerek yok.
# ----------------------------------------------------------------

# ----------------------------------------------------------------
# .fiawo bayrakları
# ----------------------------------------------------------------
FIAWO_FLAGS=""
if [ -n "${FIAWO:-}" ]; then
    [ -f "$FIAWO" ] || { echo "[HATA] FIAWO dosyası bulunamadı: $FIAWO" >&2; exit 1; }
    echo "  .fiawo Seg0 : $FIAWO  ($(wc -c < "$FIAWO") bayt) → 0x80200000"
    FIAWO_FLAGS="$FIAWO_FLAGS -device loader,file=$FIAWO,addr=0x80200000,force-raw=on"
fi
if [ -n "${FIAWO1:-}" ]; then
    [ -f "$FIAWO1" ] || { echo "[HATA] FIAWO1 dosyası bulunamadı: $FIAWO1" >&2; exit 1; }
    echo "  .fiawo Seg1 : $FIAWO1  ($(wc -c < "$FIAWO1") bayt) → 0x80300000"
    FIAWO_FLAGS="$FIAWO_FLAGS -device loader,file=$FIAWO1,addr=0x80300000,force-raw=on"
fi

# ----------------------------------------------------------------
# [6/6] QEMU başlat
# ----------------------------------------------------------------
echo "[6/6] QEMU başlatılıyor..."
echo ""

if [ "$BOOT_MODE" = "custom" ]; then
    # ============================================================
    # Özel FIRMAWORK Bootloader Zinciri
    #   Stage1 MBR → Stage2 (A20+PM32+COM1) → entry.s → 64-bit
    # ============================================================
    echo "  [Özel Bootloader Modu — Stage1/Stage2/Kernel zinciri]"
    echo "  Disk imajı oluşturuluyor..."

    # NOLOAD bölümleri (.arch_rv32, .arch_a64) zaten PT_LOAD dışında —
    # --remove-section gerekmez; objcopy yalnızca PT_LOAD içeriğini yazar.
    llvm-objcopy -O binary firmware.elf firmware.bin
    FWSZ=$(wc -c < firmware.bin)
    FWSEC=$(( (FWSZ + 511) / 512 ))
    echo "  firmware.bin : $FWSZ bayt  ($FWSEC sektör)"

    if command -v nasm > /dev/null 2>&1; then
        NASM_CMD="nasm"
    elif nix-shell -p nasm --run "nasm --version" > /dev/null 2>&1; then
        NASM_CMD="nix-shell -p nasm --run nasm"
    else
        echo "[HATA] NASM bulunamadı." >&2; exit 1
    fi

    $NASM_CMD -f bin bootloader/stage1.s -o bootloader/stage1.bin
    $NASM_CMD -f bin bootloader/stage2.s -o bootloader/stage2.bin
    echo "  stage1.bin : $(wc -c < bootloader/stage1.bin) bayt"
    echo "  stage2.bin : $(wc -c < bootloader/stage2.bin) bayt"

    dd if=/dev/zero              of=disk.img bs=512 count=2048 2>/dev/null
    dd if=bootloader/stage1.bin  of=disk.img bs=512 count=1   conv=notrunc 2>/dev/null
    dd if=bootloader/stage2.bin  of=disk.img bs=512 seek=1 count=16 conv=notrunc 2>/dev/null
    dd if=firmware.bin            of=disk.img bs=512 seek=17  conv=notrunc 2>/dev/null
    echo "  disk.img    : $(wc -c < disk.img) bayt"

    echo ""
    echo "  Bellek Haritası:"
    echo "    0x00007C00   Stage1 MBR"
    echo "    0x00007E00   Stage2 (COM1 + PM32)"
    echo "    0x00008000   Yükleme tamponu (32 KB)"
    echo "    0x00100000   FIRMAWORK kernel (long mode)"
    echo "    0x000B8000   VGA metin tamponu (80×25)"
    echo "    0x80100000   FST[0..3]  (serial_puts|putc|getc|puthex)"
    [ -n "${FIAWO:-}" ] && echo "    0x80200000   $FIAWO  (.fiawo Seg0)"
    echo ""
    echo "  Boot sihri : EAX=0xF12AB007  (FIRMAB00T)"
    echo "  Zincir     : Stage1 → Stage2 → 0x100000 → long mode → main()"
    echo "  Çıkış      : Ctrl+A, ardından X"
    echo "════════════════════════════════════════════════════════════════"
    echo ""

    if [ "$VGA_MODE" = "1" ]; then
        qemu-system-x86_64                                  \
            -drive file=disk.img,format=raw,index=0,if=ide  \
            -serial stdio -monitor none                      \
            -vga std -display gtk                            \
            -no-reboot $FIAWO_FLAGS
    else
        qemu-system-x86_64                                  \
            -drive file=disk.img,format=raw,index=0,if=ide  \
            -serial stdio -monitor none -nographic           \
            -no-reboot $FIAWO_FLAGS
    fi

else
    # ============================================================
    # Multiboot / GRUB Modu  (QEMU -kernel)
    # ============================================================
    echo "  Bellek Haritası (Multiboot):"
    echo "    0x00100000   firmware-32.elf  (FIRMAWORK x86-64)"
    echo "    0x100000+    .arch_rv32       (RV32 blob, firmware.bin'e gömülü)"
    echo "    0x100000+    .arch_a64        (A64 blob,  firmware.bin'e gömülü)"
    echo "    0x000B8000   VGA metin tamponu (80×25)"
    echo "    0x80100000   FST[0..3]"
    [ -n "${FIAWO:-}" ]  && echo "    0x80200000   $FIAWO  (.fiawo Seg0)"
    [ -n "${FIAWO1:-}" ] && echo "    0x80300000   $FIAWO1  (.fiawo Seg1)"
    echo ""
    echo "  COM1 seri çıktısı bu terminale gelir."
    echo "  Cikis: Ctrl+A, ardindan X  --veya--  menü [7] ACPI kapat"
    [ -n "${FIAWO:-}" ] && echo "  .fiawo calistirmak icin menüden [3] secin."
    echo "════════════════════════════════════════════════════════════════"
    echo ""

    if [ "$VGA_MODE" = "1" ]; then
        qemu-system-x86_64          \
            -kernel firmware-32.elf \
            -serial stdio -monitor none \
            -vga std -display gtk   \
            -no-reboot $FIAWO_FLAGS
    else
        qemu-system-x86_64          \
            -kernel firmware-32.elf \
            -serial stdio -monitor none -nographic \
            -no-reboot $FIAWO_FLAGS
    fi
fi

# ================================================================
# ████████████████████████  RISC-V 32  ████████████████████████████
# ================================================================
elif [ "$ARCH" = "riscv32" ]; then

echo "[1/4] Araçlar kontrol ediliyor (RISC-V 32)..."
for TOOL in clang ld.lld qemu-system-riscv32; do
    if ! $TOOL --version > /dev/null 2>&1; then
        echo "      [HATA] $TOOL bulunamadı." >&2
        echo "      Not: qemu-system-riscv32 için:  nix-shell -p qemu" >&2
        exit 1
    fi
done
echo "      Clang : $(clang --version | head -1)"
echo "      QEMU  : $(qemu-system-riscv32 --version | head -1)"
echo ""

# ----------------------------------------------------------------
# [2/4] Başlatıcı + FST shim yaz
#
# Bellek haritası (QEMU virt RV32):
#   0x80000000   startup_rv32.s  (bu shim: sp kur, FST doldur, fiawo'ya atla)
#   0x80100000   FST[0..3] işlev işaretçileri (32-bit, shim tarafından doldurulur)
#   0x80200000   .fiawo  (meclpp derleyici çıktısı)
#
# UART: NS16550A @ 0x10000000  (QEMU virt varsayılanı)
# ----------------------------------------------------------------
echo "[2/4] RV32 başlatıcı + FST shim oluşturuluyor..."

cat > /tmp/fw_rv32.s << 'ASMEOF'
# ================================================================
# FIRMAWORK RV32I Başlatıcı + FST Shim
#
# RV32I QEMU virt: DRAM @ 0x80000000, UART @ 0x10000000
# ================================================================
.section .text
.global _start
_start:
    # Yığıt kur (DRAM'ın üst kısmı)
    li    sp, 0x80FF0000

    # FST tablosunu doldur @ 0x80100000
    li    t0, 0x80100000
    la    t1, fst_serial_puts
    sw    t1,  0(t0)          # FST[0] = serial_puts
    la    t1, fst_serial_putc
    sw    t1,  4(t0)          # FST[1] = serial_putc
    sw    zero, 8(t0)         # FST[2] = serial_getc  (stub)
    sw    zero, 12(t0)        # FST[3] = serial_puthex (stub)

    # .fiawo'ya atla
    li    t0, 0x80200000
    jalr  x0, 0(t0)

# ----------------------------------------------------------------
# fst_serial_putc(a0=char) — NS16550A @ 0x10000000
# ----------------------------------------------------------------
fst_serial_putc:
    li    t0, 0x10000000      # NS16550 taban
.Lwait_tx:
    lbu   t1, 5(t0)           # LSR: Line Status Register
    andi  t1, t1, 0x20        # THRE: Transmitter Holding Register Empty
    beqz  t1, .Lwait_tx
    sb    a0, 0(t0)           # THR'ye yaz
    ret

# ----------------------------------------------------------------
# fst_serial_puts(a0=const char*) — null-sonlandırmalı
# ----------------------------------------------------------------
fst_serial_puts:
    mv    t2, a0              # t2 = işaretçi
.Lnext_char:
    lbu   a0, 0(t2)           # a0 = *işaretçi
    beqz  a0, .Lend_str       # null → bitir
    addi  t2, t2, 1
    call  fst_serial_putc
    j     .Lnext_char
.Lend_str:
    ret
ASMEOF

# Linker scripti
cat > /tmp/link_rv32.ld << 'LDEOF'
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M }
SECTIONS {
    .text 0x80000000 : { *(.text) }
    .rodata : { *(.rodata) }
    .data   : { *(.data)   }
    .bss    : { *(.bss) *(COMMON) }
}
LDEOF

echo "      fw_rv32.s yazıldı."

# ----------------------------------------------------------------
# [3/4] Derle + linkle
# ----------------------------------------------------------------
echo "[3/4] Derleniyor (clang --target=riscv32-unknown-elf)..."

NIX_HARDENING_ENABLE='' clang               \
    --target=riscv32-unknown-elf            \
    -march=rv32i -mabi=ilp32               \
    -ffreestanding -nostdlib -nostdinc      \
    -fno-stack-protector                    \
    -c /tmp/fw_rv32.s -o /tmp/fw_rv32.o    \
    2>&1 | grep -vE "^(Warning:|clang: warning)" || true
echo "      fw_rv32.s → fw_rv32.o"

ld.lld -T /tmp/link_rv32.ld -nostdlib      \
    /tmp/fw_rv32.o -o /tmp/fw_rv32.elf
echo "      fw_rv32.elf : $(wc -c < /tmp/fw_rv32.elf) bayt (ELF32)"

llvm-objcopy -O binary /tmp/fw_rv32.elf /tmp/fw_rv32.bin
echo "      fw_rv32.bin : $(wc -c < /tmp/fw_rv32.bin) bayt (düz ikili)"
echo ""

# ----------------------------------------------------------------
# [4/4] QEMU başlat
# ----------------------------------------------------------------
echo "[4/4] QEMU başlatılıyor (qemu-system-riscv32)..."
echo ""

# .fiawo bayrakları
RV_FIAWO_FLAGS=""
if [ -n "${FIAWO:-}" ]; then
    [ -f "$FIAWO" ] || { echo "[HATA] FIAWO dosyası bulunamadı: $FIAWO" >&2; exit 1; }
    echo "  .fiawo Seg0 : $FIAWO  ($(wc -c < "$FIAWO") bayt) → 0x80200000"
    RV_FIAWO_FLAGS="-device loader,file=$FIAWO,addr=0x80200000,force-raw=on"
fi

echo "  Bellek Haritası (RISC-V 32, QEMU virt):"
echo "    0x02000000   CLINT  (MTIME: 10 MHz)"
echo "    0x10000000   NS16550 UART  (seri çıktı)"
echo "    0x80000000   fw_rv32.bin  (başlatıcı + FST shim)"
echo "    0x80100000   FST[0..3]  (32-bit işlev işaretçileri)"
[ -n "${FIAWO:-}" ] && echo "    0x80200000   $FIAWO  (.fiawo — derleyici çıktısı)"
echo ""
echo "  Çıkış: Ctrl+A, ardından X"
echo "════════════════════════════════════════════════════════════════"
echo ""

qemu-system-riscv32                                              \
    -machine virt                                                \
    -cpu rv32                                                    \
    -m 512M                                                      \
    -bios none                                                   \
    -device loader,file=/tmp/fw_rv32.bin,addr=0x80000000,force-raw=on \
    $RV_FIAWO_FLAGS                                              \
    -serial stdio                                                \
    -monitor none                                                \
    -nographic                                                   \
    -no-reboot

# ================================================================
# ████████████████████████  AArch64  ██████████████████████████████
# ================================================================
elif [ "$ARCH" = "aarch64" ]; then

echo "[1/4] Araçlar kontrol ediliyor (AArch64)..."
for TOOL in clang ld.lld qemu-system-aarch64; do
    if ! $TOOL --version > /dev/null 2>&1; then
        echo "      [HATA] $TOOL bulunamadı." >&2
        echo "      Not: qemu-system-aarch64 için:  nix-shell -p qemu" >&2
        exit 1
    fi
done
echo "      Clang : $(clang --version | head -1)"
echo "      QEMU  : $(qemu-system-aarch64 --version | head -1)"
echo ""

# ----------------------------------------------------------------
# [2/4] Başlatıcı + FST shim yaz
#
# Bellek haritası (QEMU virt AArch64):
#   0x40000000   fw_a64.s  (shim)
#   0x80100000   FST[0..3] (64-bit işaretçiler, shim doldurur)
#   0x80200000   .fiawo
#
# UART: PL011 @ 0x09000000  (QEMU virt varsayılanı)
# CNTVCT_EL0: 62.5 MHz
# ----------------------------------------------------------------
echo "[2/4] AArch64 başlatıcı + FST shim oluşturuluyor..."

cat > /tmp/fw_a64.s << 'ASMEOF'
// ================================================================
// FIRMAWORK AArch64 Başlatıcı + FST Shim
//
// AArch64 QEMU virt: RAM @ 0x40000000, PL011 @ 0x09000000
// ================================================================
.section .text
.global _start
_start:
    // Yığıt kur
    movz x9, #0x80FF, lsl #16      // x9 = 0x80FF0000
    mov  sp, x9

    // FST tablosunu doldur @ 0x80100000 (64-bit işaretçiler)
    movz x10, #0x8010, lsl #16     // x10 = 0x80100000
    adr  x9, fst_serial_puts
    str  x9, [x10, #0]             // FST[0] = serial_puts
    adr  x9, fst_serial_putc
    str  x9, [x10, #8]             // FST[1] = serial_putc
    str  xzr, [x10, #16]           // FST[2] = serial_getc  (stub)
    str  xzr, [x10, #24]           // FST[3] = serial_puthex (stub)

    // .fiawo'ya atla
    movz x9, #0x8020, lsl #16      // x9 = 0x80200000
    br   x9

// ----------------------------------------------------------------
// fst_serial_putc(w0=char) — PL011 UART @ 0x09000000
// ----------------------------------------------------------------
fst_serial_putc:
    movz x1, #0x0900, lsl #16      // x1 = 0x09000000  PL011 base
.Lwait_tx:
    ldr  w2, [x1, #0x18]           // UARTFR (Flag Register)
    tst  w2, #0x20                 // bit5 = TXFF (TX FIFO Full)
    b.ne .Lwait_tx
    str  w0, [x1, #0]              // UARTDR = karakter
    ret

// ----------------------------------------------------------------
// fst_serial_puts(x0=const char*) — null-sonlandırmalı
// ----------------------------------------------------------------
fst_serial_puts:
    mov  x2, x0                    // x2 = işaretçi
.Lnext_char:
    ldrb w0, [x2], #1             // w0 = *işaretçi++
    cbz  w0, .Lend_str             // null → bitir
    bl   fst_serial_putc
    b    .Lnext_char
.Lend_str:
    ret
ASMEOF

# Linker scripti
cat > /tmp/link_a64.ld << 'LDEOF'
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 1024M }
SECTIONS {
    .text 0x40000000 : { *(.text) }
    .rodata : { *(.rodata) }
    .data   : { *(.data)   }
    .bss    : { *(.bss) *(COMMON) }
}
LDEOF

echo "      fw_a64.s yazıldı."

# ----------------------------------------------------------------
# [3/4] Derle + linkle
# ----------------------------------------------------------------
echo "[3/4] Derleniyor (clang --target=aarch64-elf)..."

NIX_HARDENING_ENABLE='' clang               \
    --target=aarch64-elf                    \
    -ffreestanding -nostdlib -nostdinc      \
    -fno-stack-protector                    \
    -c /tmp/fw_a64.s -o /tmp/fw_a64.o      \
    2>&1 | grep -vE "^(Warning:|clang: warning)" || true
echo "      fw_a64.s → fw_a64.o"

ld.lld -T /tmp/link_a64.ld -nostdlib       \
    /tmp/fw_a64.o -o /tmp/fw_a64.elf
echo "      fw_a64.elf : $(wc -c < /tmp/fw_a64.elf) bayt (ELF64)"

llvm-objcopy -O binary /tmp/fw_a64.elf /tmp/fw_a64.bin
echo "      fw_a64.bin : $(wc -c < /tmp/fw_a64.bin) bayt (düz ikili)"
echo ""

# ----------------------------------------------------------------
# [4/4] QEMU başlat
# ----------------------------------------------------------------
echo "[4/4] QEMU başlatılıyor (qemu-system-aarch64)..."
echo ""

# .fiawo bayrakları
A64_FIAWO_FLAGS=""
if [ -n "${FIAWO:-}" ]; then
    [ -f "$FIAWO" ] || { echo "[HATA] FIAWO dosyası bulunamadı: $FIAWO" >&2; exit 1; }
    echo "  .fiawo Seg0 : $FIAWO  ($(wc -c < "$FIAWO") bayt) → 0x80200000"
    A64_FIAWO_FLAGS="-device loader,file=$FIAWO,addr=0x80200000,force-raw=on"
fi

echo "  Bellek Haritası (AArch64, QEMU virt):"
echo "    0x09000000   PL011 UART  (seri çıktı)"
echo "    0x40000000   fw_a64.bin  (başlatıcı + FST shim)"
echo "    0x80100000   FST[0..3]  (64-bit işlev işaretçileri)"
[ -n "${FIAWO:-}" ] && echo "    0x80200000   $FIAWO  (.fiawo — derleyici çıktısı)"
echo ""
echo "  CNTVCT_EL0 frekansı: 62.5 MHz (wait:N için)"
echo "  Çıkış: Ctrl+A, ardından X"
echo "════════════════════════════════════════════════════════════════"
echo ""

qemu-system-aarch64                                              \
    -machine virt                                                \
    -cpu cortex-a57                                              \
    -m 512M                                                      \
    -bios none                                                   \
    -device loader,file=/tmp/fw_a64.bin,addr=0x40000000,cpu-num=0,force-raw=on \
    $A64_FIAWO_FLAGS                                             \
    -serial stdio                                                \
    -monitor none                                                \
    -nographic                                                   \
    -no-reboot

else
    echo "[HATA] Bilinmeyen mimari: $ARCH" >&2
    echo "       Geçerli değerler: x86_64  riscv32  aarch64" >&2
    exit 1
fi
