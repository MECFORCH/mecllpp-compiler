/* ================================================================
 * gen_blobs.c — FIRMAWORK Mimari Blob Üretici
 *
 * Çapraz derleyici gerektirmeden fw_rv32.bin ve fw_a64.bin üretir.
 * Doğrudan makine kodu baytları gömülüdür.
 *
 * Derleme: gcc gen_blobs.c -o gen_blobs
 * Kullanım: ./gen_blobs
 * ================================================================ */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void w32le(FILE *f, uint32_t v) {
    uint8_t b[4] = {v, v>>8, v>>16, v>>24};
    fwrite(b, 1, 4, f);
}

/* ----------------------------------------------------------------
 * RISC-V 32 (RV32I) — NS16550 @ 0x10000000
 *
 * Echo döngüsü:
 *   lui  t0, 0x10000      # t0 = 0x10000000
 * loop:
 *   lbu  t1, 20(t0)       # t1 = LSR (offset 0x14)
 *   andi t1, t1, 1        # bit0 = Data Ready
 *   beqz t1, loop         # bekle
 *   lbu  a0, 0(t0)        # a0 = RBR (alınan bayt)
 * tx_wait:
 *   lbu  t1, 20(t0)       # LSR
 *   andi t1, t1, 0x20     # bit5 = TX Empty
 *   beqz t1, tx_wait
 *   sb   a0, 0(t0)        # THR = a0 (gönder)
 *   j    loop
 *
 * RV32I kodlama (elde hesaplanmış):
 *   lui  t0, 0x10000   → 0x100002B7
 *   lbu  t1, 20(t0)    → 0x01424303
 *   andi t1, t1, 1     → 0x00136313
 *   beqz t1, -8        → 0xFE030CE3  (BEQ t1,x0,-8)
 *   lbu  a0, 0(t0)     → 0x00024503
 *   lbu  t1, 20(t0)    → 0x01424303
 *   andi t1, t1, 0x20  → 0x02036313
 *   beqz t1, -8        → 0xFE030CE3
 *   sb   a0, 0(t0)     → 0x00A20023
 *   jal  x0, -36       → 0xFDDFF06F  (j loop = jal x0, -36)
 * ---------------------------------------------------------------- */
static void gen_rv32(void) {
    FILE *f = fopen("fw_rv32.bin", "wb");
    if (!f) { perror("fw_rv32.bin"); return; }

    /* lui t0, 0x10000  →  imm[31:12]=0x10000, rd=t0(5), opcode=0x37 */
    w32le(f, 0x100002B7u);   /* lui  t0, 0x10000          */

    /* [loop] lbu t1, 20(t0)  → offset=20=0x14, rs1=t0(5), rd=t1(6), funct3=4, opcode=0x03 */
    w32le(f, 0x01424303u);   /* lbu  t1, 20(t0)           */

    /* andi t1, t1, 1  → imm=1, rs1=t1(6), funct3=7, rd=t1(6), opcode=0x13 */
    w32le(f, 0x00136313u);   /* andi t1, t1, 1            */

    /* BEQ t1, x0, -8: opcode=0x63, funct3=0, rs1=t1(6), rs2=x0(0)
       imm=-8 → imm[12|10:5]=0b1111111, imm[4:1|11]=0b1100
       31..25=1111111, 24..20=00000, 19..15=00110, 14..12=000, 11..8=1100, 7=1, 6..0=0x63
       = 0xFE030CE3 */
    w32le(f, 0xFE030CE3u);   /* beqz t1, loop (-8)        */

    /* lbu a0, 0(t0)  → offset=0, rs1=t0(5), rd=a0(10), funct3=4, opcode=0x03 */
    w32le(f, 0x00024503u);   /* lbu  a0, 0(t0)            */

    /* [tx_wait] lbu t1, 20(t0) */
    w32le(f, 0x01424303u);   /* lbu  t1, 20(t0)           */

    /* andi t1, t1, 0x20 → imm=0x20=32 */
    w32le(f, 0x02036313u);   /* andi t1, t1, 0x20         */

    /* beqz t1, tx_wait (-8) */
    w32le(f, 0xFE030CE3u);   /* beqz t1, tx_wait (-8)     */

    /* sb a0, 0(t0)  → rs1=t0(5), rs2=a0(10), offset=0, funct3=0, opcode=0x23
       imm[11:5]=0, imm[4:0]=0, rs2=01010, rs1=00101, funct3=000, opcode=0100011
       = 0x00A20023 */
    w32le(f, 0x00A20023u);   /* sb   a0, 0(t0)            */

    /* JAL x0, -36 (geri loop'a): PC şu an offset=36, loop=0 → rel=-36
       JAL imm=-36=0xFFFFFFDC
       imm[20|10:1|11|19:12] → 0xFDDFF06F */
    w32le(f, 0xFDDFF06Fu);   /* j    loop  (jal x0, -36)  */

    fclose(f);
    printf("fw_rv32.bin: %d bayt (10 komut, RV32I echo döngüsü)\n", 10*4);
}

/* ----------------------------------------------------------------
 * AArch64 — PL011 UART @ 0x09000000
 *
 * Echo döngüsü (x1=UART_BASE):
 *   MOVZ x1, #0x0900, LSL#16    # x1 = 0x09000000
 * loop:
 *   LDR  w2, [x1, #0x18]        # w2 = UARTFR
 *   TST  w2, #0x10              # RXFE bit
 *   B.NE loop                   # dolu değilse bekle
 *   LDR  w0, [x1, #0]           # w0 = UARTDR (alınan)
 * tx_wait:
 *   LDR  w2, [x1, #0x18]        # UARTFR
 *   TST  w2, #0x20              # TXFF bit
 *   B.NE tx_wait
 *   STR  w0, [x1, #0]           # UARTDR = w0 (gönder)
 *   B    loop
 *
 * A64 kodlama (elde):
 *   MOVZ x1, #0x0900, LSL#16   → 0xD2A12001
 *   LDR  w2, [x1, #0x18]       → 0xB9401822
 *   TST  w2, #0x10              → 0x7201005F (ANDS wzr,w2,#0x10)
 *   B.NE -12                    → 0x54FFFF61 (B.NE -3 insn = -12 byte)
 *   LDR  w0, [x1, #0]          → 0xB9400020
 *   LDR  w2, [x1, #0x18]       → 0xB9401822
 *   TST  w2, #0x20              → 0x7201805F (ANDS wzr,w2,#0x20)
 *   B.NE -12                    → 0x54FFFF61
 *   STR  w0, [x1, #0]          → 0xB9000020
 *   B    loop (-36)             → 0x17FFFFF6 (B #-36 = -9 insn)
 * ---------------------------------------------------------------- */
static void gen_a64(void) {
    FILE *f = fopen("fw_a64.bin", "wb");
    if (!f) { perror("fw_a64.bin"); return; }

    /* MOVZ x1, #0x0900, LSL#16 → opc=10, hw=01, imm16=0x0900, Rd=1
       [31]=1, [30:29]=10, [28:23]=100101, [22:21]=01, [20:5]=0x0900, [4:0]=00001
       = 0xD2A12001 */
    w32le(f, 0xD2A12001u);   /* MOVZ x1, #0x09000000      */

    /* [loop] LDR w2, [x1, #0x18]  (imm12=6, opc=01, Rn=1, Rt=2)
       0xB9401822 */
    w32le(f, 0xB9401822u);   /* LDR  w2, [x1, #0x18]      */

    /* TST w2, #0x10  = ANDS wzr, w2, #0x10
       immr=0, imms=3 (4-bit mask 0..3 → 0xF? No: #0x10 = bit4)
       #0x10: N=0,immr=27,imms=0 → 0x720100 5F... 
       Doğrudan encode: 0x7201005F */
    w32le(f, 0x7201005Fu);   /* TST  w2, #0x10            */

    /* B.NE -12 (3 komut geri = -3*4=-12)
       B.cond: [31:24]=0101 0100, imm19=-3=0x7FFFD, cond=0001(NE)
       imm19 = -3 → 0x1FFFD = 0b1_1111_1111_1111_1101
       [23:5] = imm19, [4]=0, [3:0]=cond
       = 0101 0100 | 1111111111111101 000 | 0 | 0001
       Doğrudan: 0x54FFFF61 */
    w32le(f, 0x54FFFF61u);   /* B.NE loop (-12)           */

    /* LDR w0, [x1, #0]  → 0xB9400020 */
    w32le(f, 0xB9400020u);   /* LDR  w0, [x1, #0]         */

    /* [tx_wait] LDR w2, [x1, #0x18] */
    w32le(f, 0xB9401822u);   /* LDR  w2, [x1, #0x18]      */

    /* TST w2, #0x20 = ANDS wzr, w2, #0x20
       #0x20 = bit5: N=0, immr=27, imms=0... encode = 0x7201805F */
    w32le(f, 0x7201805Fu);   /* TST  w2, #0x20            */

    /* B.NE tx_wait (-12) */
    w32le(f, 0x54FFFF61u);   /* B.NE tx_wait (-12)        */

    /* STR w0, [x1, #0]  → 0xB9000020 */
    w32le(f, 0xB9000020u);   /* STR  w0, [x1, #0]         */

    /* B loop = B #-36 (-9 insn * 4 = -36)
       B imm26=-9=0x3FFFFF7
       0001 01 | imm26 = 0x17FFFFF7 */
    w32le(f, 0x17FFFFF7u);   /* B    loop  (-36)          */

    fclose(f);
    printf("fw_a64.bin: %d bayt (10 komut, AArch64 echo döngüsü)\n", 10*4);
}

int main(void) {
    printf("=== FIRMAWORK Blob Üretici ===\n");
    gen_rv32();
    gen_a64();
    printf("Tamamlandı.\n");
    return 0;
}
