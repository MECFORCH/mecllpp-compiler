/* ================================================================
 * gen_blobs.c — FIRMAWORK Mimari Blob Üretici  (v2)
 *
 * Çapraz derleyici olmadan fw_rv32.bin ve fw_a64.bin üretir.
 * Doğrudan makine kodu bayt kodlayıcıları kullanılır.
 *
 * Derleme: gcc gen_blobs.c -o gen_blobs
 * Kullanım: ./gen_blobs
 *
 * Her blob:
 *   - Banner / karşılama mesajı yazdırır
 *   - Komut döngüsü: help  info  quit
 *   - quit → QEMU power-off (platform mimarisine özgü)
 *   - x86 tarafında invoke_rv32 / invoke_a64 zaten geri döner
 *     (sonsuz döngüde kalmaz)
 * ================================================================ */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Yardımcı: little-endian 32-bit yaz
 * ---------------------------------------------------------------- */
static void w32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
    fwrite(b, 1, 4, f);
}

/* ================================================================
 * RV32I Kodlayıcıları
 * ================================================================ */

/* Yazmaç adları */
#define RV_ZERO 0
#define RV_RA   1
#define RV_SP   2
#define RV_T0   5
#define RV_T1   6
#define RV_T2   7
#define RV_A0  10
#define RV_A1  11

static uint32_t rv_lui(int rd, uint32_t imm20) {
    return ((imm20 & 0xFFFFF) << 12) | ((uint32_t)rd << 7) | 0x37u;
}
static uint32_t rv_auipc(int rd) { /* imm20=0, gives PC */
    return ((uint32_t)rd << 7) | 0x17u;
}
static uint32_t rv_jal(int rd, int32_t off) {
    uint32_t u = (uint32_t)off;
    uint32_t i20    = (u >> 20) & 1u;
    uint32_t i19_12 = (u >> 12) & 0xFFu;
    uint32_t i11    = (u >> 11) & 1u;
    uint32_t i10_1  = (u >>  1) & 0x3FFu;
    return (i20 << 31) | (i10_1 << 21) | (i11 << 20) |
           (i19_12 << 12) | ((uint32_t)rd << 7) | 0x6Fu;
}
static uint32_t rv_jalr(int rd, int rs1, int32_t imm) {
    return (((uint32_t)(imm & 0xFFF)) << 20) | ((uint32_t)rs1 << 15) |
           ((uint32_t)rd << 7) | 0x67u;
}
static uint32_t rv_addi(int rd, int rs1, int32_t imm) {
    return (((uint32_t)(imm & 0xFFF)) << 20) | ((uint32_t)rs1 << 15) |
           ((uint32_t)rd << 7) | 0x13u;
}
static uint32_t rv_lbu(int rd, int rs1, int32_t off) {
    return (((uint32_t)(off & 0xFFF)) << 20) | ((uint32_t)rs1 << 15) |
           (4u << 12) | ((uint32_t)rd << 7) | 0x03u;
}
static uint32_t rv_lw(int rd, int rs1, int32_t off) {
    return (((uint32_t)(off & 0xFFF)) << 20) | ((uint32_t)rs1 << 15) |
           (2u << 12) | ((uint32_t)rd << 7) | 0x03u;
}
static uint32_t rv_sb(int rs1, int rs2, int32_t off) {
    uint32_t i11_5 = ((uint32_t)(off >> 5)) & 0x7Fu;
    uint32_t i4_0  = (uint32_t)(off & 0x1F);
    return (i11_5 << 25) | ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) |
           (i4_0 << 7) | 0x23u;
}
static uint32_t rv_sw(int rs1, int rs2, int32_t off) {
    uint32_t i11_5 = ((uint32_t)(off >> 5)) & 0x7Fu;
    uint32_t i4_0  = (uint32_t)(off & 0x1F);
    return (i11_5 << 25) | ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) |
           (2u << 12) | (i4_0 << 7) | 0x23u;
}
static uint32_t rv_andi(int rd, int rs1, int32_t imm) {
    return (((uint32_t)(imm & 0xFFF)) << 20) | ((uint32_t)rs1 << 15) |
           (7u << 12) | ((uint32_t)rd << 7) | 0x13u;
}
static uint32_t rv_beq(int rs1, int rs2, int32_t off) {
    uint32_t u    = (uint32_t)off;
    uint32_t i12  = (u >> 12) & 1u;
    uint32_t i11  = (u >> 11) & 1u;
    uint32_t i10_5 = (u >> 5) & 0x3Fu;
    uint32_t i4_1  = (u >> 1) & 0xFu;
    return (i12 << 31) | (i10_5 << 25) | ((uint32_t)rs2 << 20) |
           ((uint32_t)rs1 << 15) | (i4_1 << 8) | (i11 << 7) | 0x63u;
}
static uint32_t rv_bne(int rs1, int rs2, int32_t off) {
    uint32_t u    = (uint32_t)off;
    uint32_t i12  = (u >> 12) & 1u;
    uint32_t i11  = (u >> 11) & 1u;
    uint32_t i10_5 = (u >> 5) & 0x3Fu;
    uint32_t i4_1  = (u >> 1) & 0xFu;
    return (i12 << 31) | (i10_5 << 25) | ((uint32_t)rs2 << 20) |
           ((uint32_t)rs1 << 15) | (1u << 12) | (i4_1 << 8) | (i11 << 7) | 0x63u;
}
static uint32_t rv_wfi(void) { return 0x10500073u; }
static uint32_t rv_ret(void) { return 0x00008067u; } /* jalr x0, ra, 0 */

/* ================================================================
 * AArch64 Kodlayıcıları
 * ================================================================ */

/* Yazmaç adları */
#define A64_X0  0
#define A64_X1  1
#define A64_X2  2
#define A64_X3  3
#define A64_X4  4
#define A64_X5  5
#define A64_X30 30
#define A64_SP  31
#define A64_XZR 31

#define A64_W0  0
#define A64_W2  2
#define A64_W3  3
#define A64_W4  4
#define A64_W5  5

/* B.cond condition codes */
#define A64_EQ  0
#define A64_NE  1

static uint32_t a64_movz_x(int rd, uint32_t imm16, int hw) { /* hw: 0=lsl0, 1=lsl16, 2=lsl32 */
    return (1u<<31)|(2u<<29)|(0x25u<<23)|((uint32_t)hw<<21)|((imm16&0xFFFF)<<5)|(uint32_t)rd;
}
static uint32_t a64_movk_x(int rd, uint32_t imm16, int hw) {
    return (1u<<31)|(3u<<29)|(0x25u<<23)|((uint32_t)hw<<21)|((imm16&0xFFFF)<<5)|(uint32_t)rd;
}
static uint32_t a64_movz_w(int rd, uint32_t imm16) { /* 32-bit MOVZ, hw=0 */
    return 0x52800000u|((imm16&0xFFFF)<<5)|(uint32_t)rd;
}
static uint32_t a64_add_x_imm(int rd, int rn, uint32_t imm12) { /* 64-bit ADD immediate */
    return (1u<<31)|(0u<<29)|(0x11u<<24)|((imm12&0xFFF)<<10)|((uint32_t)rn<<5)|(uint32_t)rd;
}
static uint32_t a64_b(int32_t off_bytes) {
    uint32_t i26 = ((uint32_t)(off_bytes/4)) & 0x3FFFFFFu;
    return (5u<<26)|i26;
}
static uint32_t a64_bl(int32_t off_bytes) {
    uint32_t i26 = ((uint32_t)(off_bytes/4)) & 0x3FFFFFFu;
    return (0x25u<<26)|i26;
}
static uint32_t a64_b_cond(int cond, int32_t off_bytes) {
    uint32_t i19 = ((uint32_t)(off_bytes/4)) & 0x7FFFFu;
    return (0x54u<<24)|(i19<<5)|(uint32_t)cond;
}
static uint32_t a64_adr(int rd, int32_t off_bytes) {
    uint32_t u   = (uint32_t)off_bytes;
    uint32_t lo  = u & 3u;
    uint32_t hi  = (u >> 2) & 0x7FFFFu;
    return (lo<<29)|(0x10u<<24)|(hi<<5)|(uint32_t)rd;
}
/* LDR w_rt, [x_rn, #off] — unsigned scaled (off must be 4-aligned) */
static uint32_t a64_ldr_w(int rt, int rn, int off) {
    uint32_t i12 = (uint32_t)(off/4) & 0xFFFu;
    return 0xB9400000u|(i12<<10)|((uint32_t)rn<<5)|(uint32_t)rt;
}
/* STR w_rt, [x_rn, #0] */
static uint32_t a64_str_w0(int rt, int rn) {
    return 0xB9000000u|((uint32_t)rn<<5)|(uint32_t)rt;
}
/* LDRB w_rt, [x_rn] (offset 0) */
static uint32_t a64_ldrb(int rt, int rn) {
    return 0x39400000u|((uint32_t)rn<<5)|(uint32_t)rt;
}
/* CBZ w_rt, +off_bytes */
static uint32_t a64_cbz_w(int rt, int32_t off_bytes) {
    uint32_t i19 = ((uint32_t)(off_bytes/4)) & 0x7FFFFu;
    return 0x34000000u|(i19<<5)|(uint32_t)rt;
}
/* CMP w_rn, #imm12 = SUBS WZR, Wn, #imm */
static uint32_t a64_cmp_w(int rn, uint32_t imm12) {
    return 0x71000000u|((imm12&0xFFF)<<10)|((uint32_t)rn<<5)|31u;
}
/* TST w_rn, #(1<<bit) — 32-bit AND with single-bit mask
 * For 32-bit: N=0, immr=(32-bit)%32, imms=0 */
static uint32_t a64_tst_bit_w(int rn, int bit) {
    uint32_t immr = (uint32_t)((32 - bit) & 31);
    return 0x72000000u|(immr<<16)|(0u<<10)|((uint32_t)rn<<5)|31u;
}
/* STR x30, [sp, #-16]! — pre-index save LR */
static uint32_t a64_str_x30_pre16(void) { return 0xF81F8FFEu; }
/* LDR x30, [sp], #16  — post-index restore LR */
static uint32_t a64_ldr_x30_post16(void) { return 0xF8410FFEu; }
/* RET */
static uint32_t a64_ret(void) { return 0xD65F03C0u; }
/* WFI */
static uint32_t a64_wfi(void) { return 0xD503207Fu; }
/* HVC #0 — PSCI SYSTEM_OFF */
static uint32_t a64_hvc0(void) { return 0xD4000002u; }
/* NOP */
static uint32_t a64_nop(void) { return 0xD503201Fu; }

/* ================================================================
 * RISC-V 32 Blobu
 *
 * Donanım: NS16550 UART @ 0x10000000  (QEMU RISC-V virt)
 * Yığın:   0x80020000  (DRAM 0x80000000 + 128 KB)
 * Kapatma: sifive_test @ 0x100000, yazma 0x5555
 *
 * Kod düzeni (her giriş 4 bayt):
 *   Byte  Etiket
 *      0  START     — lui t0; lui sp; auipc+addi a0=banner; jal PUTS
 *     20  CMD_LOOP  — "> " yazdır; getc; 'q'/'h'/'i' karş.; drain+unk
 *     84  DO_HELP
 *    104  DO_INFO
 *    124  DO_QUIT   — kapatma sırası
 *    164  DRAIN     — '\n'e kadar oku (yığın çerçeveli)
 *    196  PUTS      — dizgiyi UART'a yaz
 *    232  PUTC      — tek karakter gönder
 *    252  GETC      — karakter al + yankıla (yığın çerçeveli)
 *    292  [kod sonu] → dizgiler buradan başlar
 * ================================================================ */
static void gen_rv32(void) {
    /* Dizgi içerikleri */
    const char *s_banner =
        "\n=== FIRMAWORK RV32 Mikrokerneli ===\n"
        "    UART: NS16550 @ 0x10000000\n"
        "    Komutlar: help  info  quit\n"
        "===================================\n";
    const char *s_unk  = "Bilinmeyen komut ('help' yazin)\n";
    const char *s_help =
        "Komutlar:\n"
        "  help  - bu yardim\n"
        "  info  - sistem bilgisi\n"
        "  quit  - cikis (QEMU kapatir)\n";
    const char *s_info =
        "Sistem: RISC-V 32 | QEMU virt\n"
        "UART  : NS16550 @ 0x10000000\n"
        "DRAM  : 0x80000000\n"
        "Yigin : 0x80020000\n";
    const char *s_quit = "Hosca kalin! QEMU kapatiliyor...\n";

    /* Dizgi konumları (kod sonu = 292 bayt) */
    int CODE_END = 292;
    int off_banner = CODE_END;
    int off_unk    = off_banner + (int)strlen(s_banner) + 1;
    int off_help   = off_unk   + (int)strlen(s_unk)    + 1;
    int off_info   = off_help  + (int)strlen(s_help)   + 1;
    int off_quit   = off_info  + (int)strlen(s_info)   + 1;

    /* Etiket bayt ofssetleri (kodlayıcıda kullanılır) */
    int CMD_LOOP  =  20;
    int DO_HELP   =  84;
    int DO_INFO   = 104;
    int DO_QUIT   = 124;
    /* int HALT   = 156; */
    int DRAIN     = 164;
    int PUTS      = 196;
    int PUTC      = 232;
    int GETC      = 252;

    FILE *f = fopen("fw_rv32.bin", "wb");
    if (!f) { perror("fw_rv32.bin"); return; }

    /* -------- START (bayt 0–19, 5 komut) -------- */

    /* 0: lui t0, 0x10000 → t0 = 0x10000000 (UART taban adresi) */
    w32(f, rv_lui(RV_T0, 0x10000));

    /* 4: lui sp, 0x80020 → sp = 0x80020000 (yığın tepesi) */
    w32(f, rv_lui(RV_SP, 0x80020));

    /* 8: auipc a0, 0 → a0 = 8 (bu komutun adresi) */
    w32(f, rv_auipc(RV_A0));

    /* 12: addi a0, a0, (off_banner - 8) → a0 = &banner */
    w32(f, rv_addi(RV_A0, RV_A0, off_banner - 8));

    /* 16: jal ra, PUTS */
    w32(f, rv_jal(RV_RA, PUTS - 16));

    /* -------- CMD_LOOP (bayt 20–83, 16 komut) -------- */

    /* 20: li a0, '>' */
    w32(f, rv_addi(RV_A0, RV_ZERO, '>'));
    /* 24: jal ra, PUTC */
    w32(f, rv_jal(RV_RA, PUTC - 24));
    /* 28: li a0, ' ' */
    w32(f, rv_addi(RV_A0, RV_ZERO, ' '));
    /* 32: jal ra, PUTC */
    w32(f, rv_jal(RV_RA, PUTC - 32));

    /* 36: jal ra, GETC */
    w32(f, rv_jal(RV_RA, GETC - 36));

    /* 40: li t1, 'q' */
    w32(f, rv_addi(RV_T1, RV_ZERO, 'q'));
    /* 44: beq a0, t1, DO_QUIT */
    w32(f, rv_beq(RV_A0, RV_T1, DO_QUIT - 44));

    /* 48: li t1, 'h' */
    w32(f, rv_addi(RV_T1, RV_ZERO, 'h'));
    /* 52: beq a0, t1, DO_HELP */
    w32(f, rv_beq(RV_A0, RV_T1, DO_HELP - 52));

    /* 56: li t1, 'i' */
    w32(f, rv_addi(RV_T1, RV_ZERO, 'i'));
    /* 60: beq a0, t1, DO_INFO */
    w32(f, rv_beq(RV_A0, RV_T1, DO_INFO - 60));

    /* 64: jal ra, DRAIN (kalan satırı tüket) */
    w32(f, rv_jal(RV_RA, DRAIN - 64));

    /* 68: auipc a0, 0 */
    w32(f, rv_auipc(RV_A0));
    /* 72: addi a0, a0, (off_unk - 68) */
    w32(f, rv_addi(RV_A0, RV_A0, off_unk - 68));
    /* 76: jal ra, PUTS */
    w32(f, rv_jal(RV_RA, PUTS - 76));
    /* 80: j CMD_LOOP */
    w32(f, rv_jal(RV_ZERO, CMD_LOOP - 80));

    /* -------- DO_HELP (bayt 84–103, 5 komut) -------- */
    /* 84: jal ra, DRAIN */
    w32(f, rv_jal(RV_RA, DRAIN - 84));
    /* 88: auipc a0 */
    w32(f, rv_auipc(RV_A0));
    /* 92: addi a0, a0, (off_help - 88) */
    w32(f, rv_addi(RV_A0, RV_A0, off_help - 88));
    /* 96: jal ra, PUTS */
    w32(f, rv_jal(RV_RA, PUTS - 96));
    /* 100: j CMD_LOOP */
    w32(f, rv_jal(RV_ZERO, CMD_LOOP - 100));

    /* -------- DO_INFO (bayt 104–123, 5 komut) -------- */
    /* 104: jal ra, DRAIN */
    w32(f, rv_jal(RV_RA, DRAIN - 104));
    /* 108: auipc a0 */
    w32(f, rv_auipc(RV_A0));
    /* 112: addi a0, a0, (off_info - 108) */
    w32(f, rv_addi(RV_A0, RV_A0, off_info - 108));
    /* 116: jal ra, PUTS */
    w32(f, rv_jal(RV_RA, PUTS - 116));
    /* 120: j CMD_LOOP */
    w32(f, rv_jal(RV_ZERO, CMD_LOOP - 120));

    /* -------- DO_QUIT (bayt 124–163, 10 komut) -------- */
    /* 124: jal ra, DRAIN */
    w32(f, rv_jal(RV_RA, DRAIN - 124));
    /* 128: auipc a0 */
    w32(f, rv_auipc(RV_A0));
    /* 132: addi a0, a0, (off_quit - 128) */
    w32(f, rv_addi(RV_A0, RV_A0, off_quit - 128));
    /* 136: jal ra, PUTS */
    w32(f, rv_jal(RV_RA, PUTS - 136));
    /* 140: lui t1, 0x100 → t1 = 0x100000 (sifive_test cihazı) */
    w32(f, rv_lui(RV_T1, 0x100));
    /* 144: lui t2, 5 → t2 = 0x5000 */
    w32(f, rv_lui(RV_T2, 5));
    /* 148: addi t2, t2, 0x555 → t2 = 0x5555 (QEMU power-off sihri) */
    w32(f, rv_addi(RV_T2, RV_T2, 0x555));
    /* 152: sw t2, 0(t1) → *0x100000 = 0x5555 → QEMU kapanır */
    w32(f, rv_sw(RV_T1, RV_T2, 0));
    /* 156: wfi (HALT etiketi) */
    w32(f, rv_wfi());
    /* 160: j halt (offset = 156 - 164 = -8... actually HALT=156, j at 160, off=156-160=-4) */
    w32(f, rv_jal(RV_ZERO, 156 - 160));

    /* -------- DRAIN (bayt 164–195, 8 komut) -------- */
    /* 164: addi sp, sp, -4 */
    w32(f, rv_addi(RV_SP, RV_SP, -4));
    /* 168: sw ra, 0(sp) */
    w32(f, rv_sw(RV_SP, RV_RA, 0));
    /* 172: [DRAIN_LOOP] jal ra, GETC */
    w32(f, rv_jal(RV_RA, GETC - 172));
    /* 176: li t1, '\n' */
    w32(f, rv_addi(RV_T1, RV_ZERO, '\n'));
    /* 180: bne a0, t1, DRAIN_LOOP (offset = 172-180 = -8) */
    w32(f, rv_bne(RV_A0, RV_T1, 172 - 180));
    /* 184: lw ra, 0(sp) */
    w32(f, rv_lw(RV_RA, RV_SP, 0));
    /* 188: addi sp, sp, 4 */
    w32(f, rv_addi(RV_SP, RV_SP, 4));
    /* 192: ret */
    w32(f, rv_ret());

    /* -------- PUTS (bayt 196–231, 9 komut) -------- */
    /* 196: [PUTS] lbu t1, 0(a0) */
    w32(f, rv_lbu(RV_T1, RV_A0, 0));
    /* 200: beq t1, x0, +28 (PUTS_RET=228, offset=228-200=28) */
    w32(f, rv_beq(RV_T1, RV_ZERO, 228 - 200));
    /* 204: [PUTS_TX] lbu t2, 0x14(t0) — LSR */
    w32(f, rv_lbu(RV_T2, RV_T0, 0x14));
    /* 208: andi t2, t2, 0x20 — TX Empty bit */
    w32(f, rv_andi(RV_T2, RV_T2, 0x20));
    /* 212: beq t2, x0, PUTS_TX (offset=204-212=-8) */
    w32(f, rv_beq(RV_T2, RV_ZERO, 204 - 212));
    /* 216: sb t1, 0(t0) — karakter gönder */
    w32(f, rv_sb(RV_T0, RV_T1, 0));
    /* 220: addi a0, a0, 1 — sonraki karakter */
    w32(f, rv_addi(RV_A0, RV_A0, 1));
    /* 224: j PUTS (offset=196-228=-32) */
    w32(f, rv_jal(RV_ZERO, 196 - 224));
    /* 228: [PUTS_RET] ret */
    w32(f, rv_ret());

    /* -------- PUTC (bayt 232–251, 5 komut) -------- */
    /* 232: [PUTC] lbu t1, 0x14(t0) — LSR */
    w32(f, rv_lbu(RV_T1, RV_T0, 0x14));
    /* 236: andi t1, t1, 0x20 */
    w32(f, rv_andi(RV_T1, RV_T1, 0x20));
    /* 240: beq t1, x0, PUTC (offset=232-240=-8) */
    w32(f, rv_beq(RV_T1, RV_ZERO, 232 - 240));
    /* 244: sb a0, 0(t0) */
    w32(f, rv_sb(RV_T0, RV_A0, 0));
    /* 248: ret */
    w32(f, rv_ret());

    /* -------- GETC (bayt 252–291, 10 komut) -------- */
    /* 252: [GETC] addi sp, sp, -4 */
    w32(f, rv_addi(RV_SP, RV_SP, -4));
    /* 256: sw ra, 0(sp) */
    w32(f, rv_sw(RV_SP, RV_RA, 0));
    /* 260: [GETC_WAIT] lbu t1, 0x14(t0) — LSR */
    w32(f, rv_lbu(RV_T1, RV_T0, 0x14));
    /* 264: andi t1, t1, 0x01 — RX Ready bit */
    w32(f, rv_andi(RV_T1, RV_T1, 0x01));
    /* 268: beq t1, x0, GETC_WAIT (offset=260-268=-8) */
    w32(f, rv_beq(RV_T1, RV_ZERO, 260 - 268));
    /* 272: lbu a0, 0(t0) — karakter al */
    w32(f, rv_lbu(RV_A0, RV_T0, 0));
    /* 276: jal ra, PUTC — yankıla (offset=232-280=-48) */
    w32(f, rv_jal(RV_RA, PUTC - 280));
    /* 280: lw ra, 0(sp) */
    w32(f, rv_lw(RV_RA, RV_SP, 0));
    /* 284: addi sp, sp, 4 */
    w32(f, rv_addi(RV_SP, RV_SP, 4));
    /* 288: ret */
    w32(f, rv_ret());

    /* ======== Dizgi verileri (bayt 292'den itibaren) ======== */
    fwrite(s_banner, 1, strlen(s_banner)+1, f);
    fwrite(s_unk,    1, strlen(s_unk)+1,    f);
    fwrite(s_help,   1, strlen(s_help)+1,   f);
    fwrite(s_info,   1, strlen(s_info)+1,   f);
    fwrite(s_quit,   1, strlen(s_quit)+1,   f);

    long total = ftell(f);
    fclose(f);
    printf("fw_rv32.bin: %ld bayt (%d komut + %ld bayt dizgi)\n",
           total, CODE_END/4, total - CODE_END);
}

/* ================================================================
 * AArch64 Blobu
 *
 * Donanım: PL011 UART @ 0x09000000 (QEMU RISC-V virt değil, AArch64 virt)
 * Yığın:   0x40020000 (DRAM 0x40000000 + 128 KB)
 * Kapatma: PSCI SYSTEM_OFF via HVC #0 (x0 = 0x84000008)
 *
 * Yazmaç sözleşmesi:
 *   x1  = UART_BASE (sabit, tüm alt rutinlerde geçerli)
 *   x0  = argüman / dönüş değeri / dizi işaretçisi
 *   x2,x3,x4,x5 = geçici
 *   x30 = bağlantı yazmacı (LR)
 *   sp  = yığın işaretçisi
 *
 * Kod düzeni (her giriş 4 bayt):
 *   Bayt  Etiket
 *      0  START     — MOVZ x1; MOVZ x2; MOV sp,x2; ADR x0; BL PUTS
 *     20  CMD_LOOP  — "> " yaz; GETC; 'q'/'h'/'i' karşılaştır
 *     80  DO_HELP
 *     96  DO_INFO
 *    112  DO_QUIT   — PSCI kapatma
 *    144  DRAIN     — '\n'e kadar oku
 *    168  PUTS      — dizgiyi UART'a yaz
 *    208  PUTC      — tek karakter gönder
 *    228  GETC      — karakter al + yankıla
 *    260  [kod sonu] → dizgiler buradan başlar
 * ================================================================ */
static void gen_a64(void) {
    /* Dizgi içerikleri */
    const char *s_banner =
        "\n=== FIRMAWORK AArch64 Mikrokerneli ===\n"
        "    UART: PL011 @ 0x09000000\n"
        "    Komutlar: help  info  quit\n"
        "======================================\n";
    const char *s_unk  = "Bilinmeyen komut ('help' yazin)\n";
    const char *s_help =
        "Komutlar:\n"
        "  help  - bu yardim\n"
        "  info  - sistem bilgisi\n"
        "  quit  - cikis (QEMU kapatir)\n";
    const char *s_info =
        "Sistem: AArch64 | QEMU virt\n"
        "UART  : PL011 @ 0x09000000\n"
        "DRAM  : 0x40000000\n"
        "PSCI  : SYSTEM_OFF = 0x84000008\n";
    const char *s_quit = "Hosca kalin! QEMU kapatiliyor...\n";

    /* Dizgi konumları (kod sonu = 260 bayt) */
    int CODE_END = 260;
    int off_banner = CODE_END;
    int off_unk    = off_banner + (int)strlen(s_banner) + 1;
    int off_help   = off_unk   + (int)strlen(s_unk)    + 1;
    int off_info   = off_help  + (int)strlen(s_help)   + 1;
    int off_quit   = off_info  + (int)strlen(s_info)   + 1;

    /* Etiket bayt ofsetleri */
    int CMD_LOOP = 20;
    int DO_HELP  = 80;
    int DO_INFO  = 96;
    int DO_QUIT  = 112;
    /* int HALT  = 136; */
    int DRAIN    = 144;
    int PUTS     = 168;
    int PUTC     = 208;
    int GETC     = 228;

    FILE *f = fopen("fw_a64.bin", "wb");
    if (!f) { perror("fw_a64.bin"); return; }

    /* -------- START (bayt 0–19, 5 komut) -------- */

    /* 0: MOVZ x1, #0x0900, LSL#16 → x1 = 0x09000000 (UART taban) */
    w32(f, a64_movz_x(A64_X1, 0x0900, 1));

    /* 4: MOVZ x2, #0x4002, LSL#16 → x2 = 0x40020000 (yığın tepesi) */
    w32(f, a64_movz_x(A64_X2, 0x4002, 1));

    /* 8: MOV sp, x2 = ADD sp, x2, #0 */
    w32(f, a64_add_x_imm(A64_SP, A64_X2, 0));

    /* 12: ADR x0, &banner */
    w32(f, a64_adr(A64_X0, off_banner - 12));

    /* 16: BL PUTS */
    w32(f, a64_bl(PUTS - 16));

    /* -------- CMD_LOOP (bayt 20–79, 15 komut) -------- */

    /* 20: MOVZ w0, #'>' */
    w32(f, a64_movz_w(A64_W0, '>'));
    /* 24: BL PUTC */
    w32(f, a64_bl(PUTC - 24));
    /* 28: MOVZ w0, #' ' */
    w32(f, a64_movz_w(A64_W0, ' '));
    /* 32: BL PUTC */
    w32(f, a64_bl(PUTC - 32));

    /* 36: BL GETC */
    w32(f, a64_bl(GETC - 36));

    /* 40: CMP w0, #'q' */
    w32(f, a64_cmp_w(A64_W0, 'q'));
    /* 44: B.EQ DO_QUIT */
    w32(f, a64_b_cond(A64_EQ, DO_QUIT - 44));

    /* 48: CMP w0, #'h' */
    w32(f, a64_cmp_w(A64_W0, 'h'));
    /* 52: B.EQ DO_HELP */
    w32(f, a64_b_cond(A64_EQ, DO_HELP - 52));

    /* 56: CMP w0, #'i' */
    w32(f, a64_cmp_w(A64_W0, 'i'));
    /* 60: B.EQ DO_INFO */
    w32(f, a64_b_cond(A64_EQ, DO_INFO - 60));

    /* 64: BL DRAIN */
    w32(f, a64_bl(DRAIN - 64));

    /* 68: ADR x0, &unk_msg */
    w32(f, a64_adr(A64_X0, off_unk - 68));
    /* 72: BL PUTS */
    w32(f, a64_bl(PUTS - 72));
    /* 76: B CMD_LOOP */
    w32(f, a64_b(CMD_LOOP - 76));

    /* -------- DO_HELP (bayt 80–95, 4 komut) -------- */
    /* 80: BL DRAIN */
    w32(f, a64_bl(DRAIN - 80));
    /* 84: ADR x0, &help_msg */
    w32(f, a64_adr(A64_X0, off_help - 84));
    /* 88: BL PUTS */
    w32(f, a64_bl(PUTS - 88));
    /* 92: B CMD_LOOP */
    w32(f, a64_b(CMD_LOOP - 92));

    /* -------- DO_INFO (bayt 96–111, 4 komut) -------- */
    /* 96: BL DRAIN */
    w32(f, a64_bl(DRAIN - 96));
    /* 100: ADR x0, &info_msg */
    w32(f, a64_adr(A64_X0, off_info - 100));
    /* 104: BL PUTS */
    w32(f, a64_bl(PUTS - 104));
    /* 108: B CMD_LOOP */
    w32(f, a64_b(CMD_LOOP - 108));

    /* -------- DO_QUIT (bayt 112–143, 8 komut) -------- */
    /* 112: BL DRAIN */
    w32(f, a64_bl(DRAIN - 112));
    /* 116: ADR x0, &quit_msg */
    w32(f, a64_adr(A64_X0, off_quit - 116));
    /* 120: BL PUTS */
    w32(f, a64_bl(PUTS - 120));
    /* 124: MOVZ x0, #0x8400, LSL#16 → x0 = 0x84000000 */
    w32(f, a64_movz_x(A64_X0, 0x8400, 1));
    /* 128: MOVK x0, #8 → x0 = 0x84000008 (PSCI SYSTEM_OFF) */
    w32(f, a64_movk_x(A64_X0, 8, 0));
    /* 132: HVC #0 → PSCI kapatma çağrısı */
    w32(f, a64_hvc0());
    /* 136: [HALT] WFI */
    w32(f, a64_wfi());
    /* 140: B HALT (offset = 136-144 = -8) */
    w32(f, a64_b(136 - 140));

    /* -------- DRAIN (bayt 144–167, 6 komut) -------- */
    /* 144: STR x30, [sp, #-16]! — LR kaydet */
    w32(f, a64_str_x30_pre16());
    /* 148: [DRAIN_LOOP] BL GETC */
    w32(f, a64_bl(GETC - 148));
    /* 152: CMP w0, #'\n' */
    w32(f, a64_cmp_w(A64_W0, '\n'));
    /* 156: B.NE DRAIN_LOOP (offset=148-160=-12) */
    w32(f, a64_b_cond(A64_NE, 148 - 156));
    /* 160: LDR x30, [sp], #16 — LR geri yükle */
    w32(f, a64_ldr_x30_post16());
    /* 164: RET */
    w32(f, a64_ret());

    /* -------- PUTS (bayt 168–259, 10 komut) -------- */
    /* 168: ADD x3, x0, #0 — x3 = dizgi işaretçisi */
    w32(f, a64_add_x_imm(A64_X3, A64_X0, 0));
    /* 172: [PUTS_LOOP] LDRB w4, [x3] */
    w32(f, a64_ldrb(A64_W4, A64_X3));
    /* 176: CBZ w4, PUTS_RET (offset=204-176=28) */
    w32(f, a64_cbz_w(A64_W4, 204 - 176));
    /* 180: [PUTS_TX] LDR w5, [x1, #0x18] — UARTFR */
    w32(f, a64_ldr_w(A64_W5, A64_X1, 0x18));
    /* 184: TST w5, #(1<<5) — TXFF bit */
    w32(f, a64_tst_bit_w(A64_W5, 5));
    /* 188: B.NE PUTS_TX (offset=180-192=-12) */
    w32(f, a64_b_cond(A64_NE, 180 - 188));
    /* 192: STR w4, [x1] — karakter gönder */
    w32(f, a64_str_w0(A64_W4, A64_X1));
    /* 196: ADD x3, x3, #1 — sonraki karakter */
    w32(f, a64_add_x_imm(A64_X3, A64_X3, 1));
    /* 200: B PUTS_LOOP (offset=172-204=-32) */
    w32(f, a64_b(172 - 200));
    /* 204: [PUTS_RET] RET */
    w32(f, a64_ret());

    /* -------- PUTC (bayt 208–227, 5 komut) -------- */
    /* 208: [PUTC] LDR w2, [x1, #0x18] — UARTFR */
    w32(f, a64_ldr_w(A64_W2, A64_X1, 0x18));
    /* 212: TST w2, #(1<<5) — TXFF bit */
    w32(f, a64_tst_bit_w(A64_W2, 5));
    /* 216: B.NE PUTC (offset=208-220=-12) */
    w32(f, a64_b_cond(A64_NE, 208 - 216));
    /* 220: STR w0, [x1] — karakter gönder */
    w32(f, a64_str_w0(A64_W0, A64_X1));
    /* 224: RET */
    w32(f, a64_ret());

    /* -------- GETC (bayt 228–259, 8 komut) -------- */
    /* 228: [GETC] STR x30, [sp, #-16]! */
    w32(f, a64_str_x30_pre16());
    /* 232: [GETC_WAIT] LDR w2, [x1, #0x18] — UARTFR */
    w32(f, a64_ldr_w(A64_W2, A64_X1, 0x18));
    /* 236: TST w2, #(1<<4) — RXFE bit */
    w32(f, a64_tst_bit_w(A64_W2, 4));
    /* 240: B.NE GETC_WAIT (offset=232-244=-12) */
    w32(f, a64_b_cond(A64_NE, 232 - 240));
    /* 244: LDR w0, [x1] — karakter oku */
    w32(f, a64_ldr_w(A64_W0, A64_X1, 0));
    /* 248: BL PUTC — yankıla */
    w32(f, a64_bl(PUTC - 252));
    /* 252: LDR x30, [sp], #16 */
    w32(f, a64_ldr_x30_post16());
    /* 256: RET */
    w32(f, a64_ret());

    /* ======== Dizgi verileri (bayt 260'dan itibaren) ======== */
    fwrite(s_banner, 1, strlen(s_banner)+1, f);
    fwrite(s_unk,    1, strlen(s_unk)+1,    f);
    fwrite(s_help,   1, strlen(s_help)+1,   f);
    fwrite(s_info,   1, strlen(s_info)+1,   f);
    fwrite(s_quit,   1, strlen(s_quit)+1,   f);

    long total = ftell(f);
    fclose(f);
    printf("fw_a64.bin : %ld bayt (%d komut + %ld bayt dizgi)\n",
           total, CODE_END/4, total - CODE_END);
}

int main(void) {
    printf("=== FIRMAWORK Blob Üretici v2 ===\n");
    gen_rv32();
    gen_a64();
    printf("Tamamlandı.\n");
    return 0;
}
