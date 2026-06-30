#!/bin/sh
# ================================================================
# bootloader/build.sh — FIRMAWORK Özel Önyükleyici Yapı Betiği
#
# Disk imajı düzeni:
#   LBA  0        (512 B)  : Stage 1  (MBR)
#   LBA  1–16     (8 KB)   : Stage 2  (A20 + PM geçişi + kernel yükle)
#   LBA  17+      (~192 KB): Çekirdek (firmware.bin, düz ikili)
#
# Çıktılar:
#   stage1.bin    — Stage 1 düz ikilisi (512 bayt)
#   stage2.bin    — Stage 2 düz ikilisi (≤8 KB)
#   disk.img      — Önyüklenebilir ham disk imajı
#
# Kullanım:
#   sh bootloader/build.sh
# ================================================================
set -e

BOOT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$BOOT_DIR")"

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "  FIRMAWORK Özel Önyükleyici  —  Derleme"
echo "════════════════════════════════════════════════════════════════"
echo ""

# ----------------------------------------------------------------
# 1. NASM varlık kontrolü
# ----------------------------------------------------------------
echo "[1/5] Araçlar kontrol ediliyor..."

NASM=""
if command -v nasm > /dev/null 2>&1; then
    NASM="nasm"
elif nix-shell -p nasm --run "nasm --version" > /dev/null 2>&1; then
    NASM="nix-shell -p nasm --run nasm"
else
    echo "      [HATA] NASM bulunamadi." >&2
    echo "      Kurmak icin: nix-env -iA nixpkgs.nasm" >&2
    exit 1
fi
echo "      NASM: $(nix-shell -p nasm --run 'nasm --version' 2>/dev/null | head -1)"

# ----------------------------------------------------------------
# 2. Stage 1 ve Stage 2'yi NASM ile derle
# ----------------------------------------------------------------
echo "[2/5] Stage 1 derleniyor  (MBR, 512 bayt)..."
cd "$BOOT_DIR"
nix-shell -p nasm --run \
    "nasm -f bin stage1.s -o stage1.bin" 2>&1
SZ=$(wc -c < stage1.bin)
if [ "$SZ" -ne 512 ]; then
    echo "      [HATA] stage1.bin $SZ bayt (512 olmali!)" >&2
    exit 1
fi
LAST=$(od -An -tx1 -j510 -N2 stage1.bin | tr -d ' \n')
if [ "$LAST" != "55aa" ] && [ "$LAST" != "aa55" ]; then
    # od çıktısı little-endian: 55 aa bekliyoruz
    echo "      [UYARI] Boot imzasi beklenmedik: $LAST"
fi
echo "      stage1.bin : 512 bayt  [imza: 0xAA55 ✓]"

echo "[2/5] Stage 2 derleniyor..."
nix-shell -p nasm --run \
    "nasm -f bin stage2.s -o stage2.bin" 2>&1
SZ2=$(wc -c < stage2.bin)
echo "      stage2.bin : $SZ2 bayt  ($(( SZ2 / 512 + 1 )) sektör)"
if [ "$SZ2" -gt $((16 * 512)) ]; then
    echo "      [HATA] Stage 2 16 sektoru asiyor! (max: 8192 bayt)" >&2
    exit 1
fi

# ----------------------------------------------------------------
# 3. Çekirdeği derle ve düz ikili üret
# ----------------------------------------------------------------
echo "[3/5] Cekirdek derleniyor..."
cd "$ROOT_DIR"

CFLAGS="--target=x86_64-elf -ffreestanding -fno-stack-protector
        -mno-red-zone -mno-mmx -mno-sse -mno-sse2
        -nostdlib -nostdinc -c"

NIX_HARDENING_ENABLE='' clang $CFLAGS entry.s -o entry.o \
    2>&1 | grep -vE "^(Warning:|clang: warning)" || true
NIX_HARDENING_ENABLE='' clang $CFLAGS main.c  -o main.o  \
    2>&1 | grep -vE "^(Warning:|clang: warning)" || true
NIX_HARDENING_ENABLE='' clang $CFLAGS vga.c   -o vga.o   \
    2>&1 | grep -vE "^(Warning:|clang: warning)" || true
ld.lld -T linker.ld -nostdlib entry.o main.o vga.o -o firmware.elf
llvm-objcopy -O binary firmware.elf firmware.bin
echo "      firmware.bin : $(wc -c < firmware.bin) bayt"

# ----------------------------------------------------------------
# 4. Disk imajını oluştur
# ----------------------------------------------------------------
echo "[4/5] Disk imaji olusturuluyor  (disk.img, 1 MB)..."

DISK="$ROOT_DIR/disk.img"
dd if=/dev/zero of="$DISK" bs=512 count=2048 2>/dev/null
dd if="$BOOT_DIR/stage1.bin" of="$DISK" bs=512 count=1       conv=notrunc 2>/dev/null
dd if="$BOOT_DIR/stage2.bin" of="$DISK" bs=512 seek=1 count=16 conv=notrunc 2>/dev/null
dd if="$ROOT_DIR/firmware.bin" of="$DISK" bs=512 seek=17       conv=notrunc 2>/dev/null

echo "      Disk imaji duzen:"
echo "        LBA  0       : Stage 1  (512 bayt)"
echo "        LBA  1–16    : Stage 2  ($SZ2 bayt)"
printf  "        LBA  17+     : firmware.bin  (%d bayt, %d sektor)\n" \
    "$(wc -c < firmware.bin)" "$(( ($(wc -c < firmware.bin) + 511) / 512 ))"
echo "        Toplam       : $(wc -c < "$DISK") bayt (1 MB)"

# .fiawo yükle (varsa)
if [ -n "${FIAWO:-}" ] && [ -f "$ROOT_DIR/$FIAWO" ]; then
    FIAWO_LBA=401   # 17 + 384 = 401 (çekirdekten sonra)
    dd if="$ROOT_DIR/$FIAWO" of="$DISK" bs=512 seek=$FIAWO_LBA conv=notrunc 2>/dev/null
    echo "        LBA $FIAWO_LBA+     : $FIAWO (.fiawo Seg0)"
fi

# ----------------------------------------------------------------
# 5. QEMU ile başlat
# ----------------------------------------------------------------
echo "[5/5] QEMU baslatiliyor  (disk imajindan boot)..."
echo ""
echo "  Boot zinciri:"
echo "    BIOS  →  Stage 1 (0x7C00)  →  Stage 2 (0x7E00)"
echo "    →  firmware.bin (0x100000)  →  main()"
echo ""
echo "  COM1 seri cikti bu terminale gelecek."
echo "  Cikis: Ctrl+A, ardindan X"
echo "════════════════════════════════════════════════════════════════"
echo ""

FIAWO_FLAGS=""
if [ -n "${FIAWO:-}" ] && [ -f "$ROOT_DIR/$FIAWO" ]; then
    FIAWO_FLAGS="-device loader,file=$ROOT_DIR/$FIAWO,addr=0x80200000,force-raw=on"
fi

qemu-system-x86_64                              \
    -drive file="$DISK",format=raw,index=0,if=ide \
    -serial stdio                                \
    -monitor none                                \
    -nographic                                   \
    -no-reboot                                   \
    -no-shutdown                                 \
    $FIAWO_FLAGS
