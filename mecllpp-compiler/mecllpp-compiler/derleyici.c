/* ================================================================
 * FIRMAWORK meclpp Native Compiler  —  "derleyici"  (v6, çapraz platform)
 *
 * Desteklenen komutlar:
 *   write:"metin"      — seri porta metin yaz (\n eklenir)
 *   wait:N sec         — N saniye bekle
 *   clean              — terminal temizle (ESC[2J ESC[H)
 *   continue           — 'clean' sonrasında FREEZE engelini kaldır
 *   visit:dosya.meclpp — başka .fiawo segmentine kalıcı atla
 *   halt               — CPU'yu durdur
 *   :LABEL             — etiket tanımı (0 bayt)
 *   jump:LABEL         — koşulsuz atlama
 *   if:"c"             — akümülatör[7:0] != 'c' ise sonraki tokeni atla
 *   if                 — akümülatör != 0 ise sonraki tokeni atla
 *   read               — seri porttan 1 karakter oku → akümülatöre yaz
 *                        x86-64: COM1 (0x3F8/0x3FD)
 *                        riscv32: NS16550 @ 0x10000000
 *                        aarch64: PL011 @ 0x09000000
 *
 * Hedef mimariler (--target=<isa>):
 *   x86_64   — x86-64 Long Mode, COM1 I/O portları (0x3F8)
 *              Akümülatör: rax  |  FST[0] @ 0x80100000 (64-bit ptr)
 *              wait: RDTSC (~2 GHz varsayım)
 *
 *   riscv32  — RISC-V RV32I, QEMU virt makinesi
 *              Akümülatör: s0   |  FST[0] @ 0x80100000 (32-bit ptr)
 *              wait: CLINT MTIME @ 0x200BFF8 (10 MHz)
 *
 *   aarch64  — ARMv8-A AArch64, QEMU virt makinesi
 *              Akümülatör: x0   |  FST[0] @ 0x80100000 (64-bit ptr)
 *              wait: CNTVCT_EL0 (62.5 MHz varsayım)
 *
 * FST (Firmware Service Table) donanım sözleşmesi:
 *   FST[0]  serial_puts(const char *)
 *   FST[1]  serial_putc(char)
 *   FST[2]  serial_getc() → char
 *   FST[3]  serial_puthex(uint64_t)
 *
 * Host'ta derle:
 *   gcc derleyici.c -o derleyici
 * Kullanım:
 *   ./derleyici kod.meclpp [--target=x86_64|riscv32|aarch64]
 * ================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/* ================================================================
 * Hedef mimariler
 * ================================================================ */
typedef enum { TARGET_X86_64, TARGET_RISCV32, TARGET_AARCH64 } Target;
static Target g_target = TARGET_X86_64;

/* ================================================================
 * Donanım sabitleri (hedef başına)
 * ================================================================ */
#define FST_BASE          0x80100000ULL

/* x86-64 */
#define X64_TSC_HZ        2000000000ULL

/* RISC-V 32 */
#define RV32_MTIME_ADDR   0x0200BFF8u   /* CLINT MTIME (low word) */
#define RV32_MTIME_HZ     10000000u     /* 10 MHz */
#define RV32_LAUNCHER     0x80200000u

/* AArch64 */
#define A64_CNTFRQ        62500000ULL   /* QEMU virt: 62.5 MHz */
#define A64_LAUNCHER      0x80200000ULL

/* ================================================================
 * Ortak limitler
 * ================================================================ */
#define MAX_TOKENS      512
#define MAX_EMISSIONS   1024
#define MAX_STRINGS     256
#define MAX_STR_LEN     512
#define MAX_LABELS      128
#define MAX_LABEL_LEN   64

#define CLEAN_ESC_SEQ   "\x1B[2J\x1B[H"

/* ================================================================
 * Emisyon boyutları — her hedef için
 *
 * Emisyon tipi | x86_64 | riscv32 | aarch64
 * WRITE_CALL   |   22   |   20    |   20
 * WAIT         |   36   |   28    |   36
 * VISIT        |   12   |    8    |   12
 * FREEZE       |    2   |    4    |    4
 * HALT         |    3   |    8    |    8
 * LABEL_DEF    |    0   |    0    |    0
 * JUMP         |    5   |    4    |    4
 * IF_CHAR      |    8   |    8    |   12
 * IF_SYS       |    8   |    4    |    4
 * ================================================================ */

/* ================================================================
 * Ziyaret adresi haritası
 * ================================================================ */
typedef struct { const char *filename; uint32_t address; } VisitEntry;

static const VisitEntry g_visit_map[] = {
    { "main.meclpp", 0x80300000u },
    { "kod.meclpp",  0x80200000u },
    { NULL,          0u          }
};

static uint32_t resolve_visit_addr(const char *filename)
{
    for (int i = 0; g_visit_map[i].filename; i++)
        if (strcmp(filename, g_visit_map[i].filename) == 0)
            return g_visit_map[i].address;
    fprintf(stderr, "[hata] visit: bilinmeyen hedef '%s'\n", filename);
    exit(1);
}

/* ================================================================
 * Veri yapıları
 * ================================================================ */
typedef enum {
    TOK_WRITE, TOK_WAIT, TOK_CLEAN, TOK_CONTINUE, TOK_VISIT,
    TOK_HALT, TOK_LABEL_DEF, TOK_JUMP, TOK_IF_CHAR, TOK_IF_SYS, TOK_READ,
    TOK_INVOKE_RV32, TOK_INVOKE_A64
} TokType;

typedef struct {
    TokType type;
    char    str[MAX_STR_LEN];
    int     seconds;
    char    visit_target[MAX_STR_LEN];
    char    label_name[MAX_LABEL_LEN];
    uint8_t char_val;
} Token;

typedef enum {
    EM_WRITE_CALL, EM_WAIT, EM_VISIT, EM_FREEZE,
    EM_HALT, EM_LABEL_DEF, EM_JUMP, EM_IF_CHAR, EM_IF_SYS, EM_READ,
    EM_INVOKE_RV32, EM_INVOKE_A64
} EmType;

typedef struct {
    EmType   type;
    int      str_idx;
    int      seconds;
    uint64_t visit_addr;
    char     label_name[MAX_LABEL_LEN];
    char     jump_target[MAX_LABEL_LEN];
    uint8_t  char_val;
} Emission;

typedef struct {
    char data[MAX_STR_LEN + 2];
    int  len;
} StringEntry;

typedef struct {
    char label[MAX_LABEL_LEN];
    int  byte_offset;
} LabelEntry;

/* ================================================================
 * Global durum
 * ================================================================ */
static Token      g_tokens   [MAX_TOKENS];
static int        g_ntokens;
static Emission   g_emissions[MAX_EMISSIONS];
static int        g_nemissions;
static StringEntry g_strings [MAX_STRINGS];
static int        g_nstrings;
static LabelEntry g_labels   [MAX_LABELS];
static int        g_nlabels;
static int        g_em_byte_offset[MAX_EMISSIONS];

/* ================================================================
 * Yardımcılar
 * ================================================================ */
static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int n = (int)strlen(s);
    while (n > 0 && isspace((unsigned char)s[n-1])) s[--n] = '\0';
}

static int pool_add(const char *str, int add_newline)
{
    if (g_nstrings >= MAX_STRINGS) { fprintf(stderr,"[hata] Dize havuzu doldu.\n"); exit(1); }
    int idx = g_nstrings++;
    size_t sl = strlen(str);
    if (sl + 2 >= MAX_STR_LEN) { fprintf(stderr,"[hata] Dize çok uzun.\n"); exit(1); }
    memcpy(g_strings[idx].data, str, sl);
    if (add_newline) g_strings[idx].data[sl++] = '\n';
    g_strings[idx].data[sl] = '\0';
    g_strings[idx].len = (int)sl + 1;
    return idx;
}

static int label_lookup(const char *name)
{
    for (int i = 0; i < g_nlabels; i++)
        if (strcmp(g_labels[i].label, name) == 0)
            return g_labels[i].byte_offset;
    return -1;
}

/* ================================================================
 * emission_bytes — hedef başına boyutlar
 * ================================================================ */
static int emission_bytes(const Emission *em)
{
    switch (em->type) {
        case EM_LABEL_DEF: return 0;
        case EM_WRITE_CALL:
            return (g_target == TARGET_X86_64) ? 22 : 20;
        case EM_WAIT:
            if (g_target == TARGET_RISCV32) return 28;
            return 36;
        case EM_VISIT:
            return (g_target == TARGET_RISCV32) ? 8 : 12;
        case EM_FREEZE:
            return (g_target == TARGET_X86_64) ? 2 : 4;
        case EM_HALT:
            return (g_target == TARGET_X86_64) ? 3 : 8;
        case EM_JUMP:
            return (g_target == TARGET_X86_64) ? 5 : 4;
        case EM_IF_CHAR:
            if (g_target == TARGET_AARCH64) return 12;
            return 8;
        case EM_IF_SYS:
            if (g_target == TARGET_X86_64) return 8;
            return 4;
        case EM_READ:
            /* x86-64: 10B (inline COM1) | riscv32: 20B (inline NS16550) | aarch64: 20B (inline PL011) */
            return (g_target == TARGET_X86_64) ? 10 : 20;
        case EM_INVOKE_RV32:
        case EM_INVOKE_A64:
            /* x86-64: 16B (MOVABS+MOV[rax+off]+CALL) | rv32: 12B (lui+lw+jalr) | a64: 12B (movz+ldr+blr) */
            return (g_target == TARGET_X86_64) ? 16 : 12;
    }
    return 0;
}

/* ================================================================
 * Bayt yayma yardımcıları
 * ================================================================ */
static void emit_u8(FILE *f, uint8_t b)    { fputc(b, f); }

static void emit_u32le(FILE *f, uint32_t v)
{
    emit_u8(f, v & 0xFF); emit_u8(f, (v>>8)&0xFF);
    emit_u8(f, (v>>16)&0xFF); emit_u8(f, (v>>24)&0xFF);
}

static void emit_i32le(FILE *f, int32_t v) { emit_u32le(f, (uint32_t)v); }

static void emit_u64le(FILE *f, uint64_t v)
{
    emit_u32le(f, (uint32_t)(v & 0xFFFFFFFF));
    emit_u32le(f, (uint32_t)(v >> 32));
}

/* ================================================================
 * ── RISC-V 32 (RV32I) encoding helpers ──────────────────────────
 * ================================================================ */

/* hi20/lo12 bölmesi — ALU sign-extension için düzeltme */
static void rv32_hi_lo(uint32_t value, int32_t *hi, int32_t *lo)
{
    int32_t v = (int32_t)value;
    *lo = v & 0xFFF;
    if (*lo >= 0x800) *lo -= 0x1000;
    *hi = (v - *lo) >> 12;
}

/* auipc a0,hi + addi a0,a0,lo  — PC-relative adres → a0 */
static void rv32_auipc_a0(FILE *f, int32_t delta)
{
    int32_t hi, lo;
    rv32_hi_lo((uint32_t)delta, &hi, &lo);
    emit_u32le(f, ((uint32_t)(hi & 0xFFFFF) << 12) | 0x517u);   /* auipc a0 */
    emit_u32le(f, ((uint32_t)(lo & 0xFFF)   << 20) | 0x50513u); /* addi a0 */
}


/* lui t0,0x80100 + lw t0,slot*4(t0) + jalr ra,0(t0)  — FST çağrısı */
static void rv32_fst_call(FILE *f, int slot)
{
    uint32_t lw = ((uint32_t)(slot * 4) << 20) | (5u<<15) | (2u<<12) | (5u<<7) | 0x03u;
    emit_u32le(f, 0x801002B7u); /* lui t0, 0x80100 */
    emit_u32le(f, lw);          /* lw  t0, slot*4(t0) */
    emit_u32le(f, 0x000280E7u); /* jalr ra, 0(t0) */
}

/* lui t1,hi + jalr x0,lo(t1)  — mutlak atlama */
static void rv32_abs_jump(FILE *f, uint32_t addr)
{
    int32_t hi, lo;
    rv32_hi_lo(addr, &hi, &lo);
    emit_u32le(f, ((uint32_t)(hi & 0xFFFFF) << 12) | 0x337u);   /* lui  t1 */
    emit_u32le(f, ((uint32_t)(lo & 0xFFF)   << 20) | 0x30067u); /* jalr x0 */
}

/* jal x0, offset  — J-type koşulsuz atlama */
static void rv32_jal_x0(FILE *f, int32_t offset)
{
    uint32_t o = (uint32_t)(offset & 0x1FFFFF);
    uint32_t w = (((o>>20)&1u)<<31) | (((o>>1)&0x3FFu)<<21) |
                 (((o>>11)&1u)<<20) | (((o>>12)&0xFFu)<<12) | 0x6Fu;
    emit_u32le(f, w);
}

/* BNE rs1, rs2, offset  — B-type dallanma */
static void rv32_bne(FILE *f, int rs1, int rs2, int32_t offset)
{
    uint32_t o = (uint32_t)(offset & 0x1FFF);
    uint32_t w = (((o>>12)&1u)<<31) | (((o>>5)&0x3Fu)<<25) |
                 ((uint32_t)(rs2&0x1F)<<20) | ((uint32_t)(rs1&0x1F)<<15) |
                 0x1000u | (((o>>1)&0xFu)<<8) | (((o>>11)&1u)<<7) | 0x63u;
    emit_u32le(f, w);
}

/* MTIME meşgul-bekleme  — 7 komut = 28 bayt */
static void rv32_mtime_wait(FILE *f, uint64_t ticks)
{
    int32_t hi, lo;
    rv32_hi_lo((uint32_t)(ticks & 0xFFFFFFFF), &hi, &lo);
    emit_u32le(f, 0x0200C5B7u);                                   /* lui a1, 0x200C  */
    emit_u32le(f, 0xFF85A603u);                                    /* lw  a2, -8(a1)  */
    emit_u32le(f, ((uint32_t)(hi & 0xFFFFF) << 12) | 0x6B7u);    /* lui a3, hi      */
    emit_u32le(f, ((uint32_t)(lo & 0xFFF)   << 20) | 0x68693u);  /* addi a3, a3, lo */
    emit_u32le(f, 0x00C686B3u);                                    /* add a3, a3, a2  */
    emit_u32le(f, 0xFF85A603u);                                    /* lw  a2, -8(a1)  */
    emit_u32le(f, 0xFED66EE3u);                                    /* bltu a2, a3, -4 */
}

/* ================================================================
 * ── AArch64 (ARMv8-A) encoding helpers ──────────────────────────
 * ================================================================ */

/* ADRP Xd, page_delta (sayfa farkı, sayfa cinsinden) */
static uint32_t a64_adrp(int rd, int32_t page_delta)
{
    uint32_t imm = (uint32_t)(page_delta & 0x1FFFFF);
    return 0x90000000u | ((imm & 0x3u) << 29) | (((imm >> 2) & 0x7FFFFu) << 5) | ((uint32_t)rd & 0x1Fu);
}

/* ADD Xd, Xn, #imm12 (64-bit) */
static uint32_t a64_add_imm64(int rd, int rn, uint32_t imm12)
{
    return 0x91000000u | ((imm12 & 0xFFFu) << 10) | (((uint32_t)rn & 0x1Fu) << 5) | ((uint32_t)rd & 0x1Fu);
}

/* MOVZ Xd, #imm16, LSL #shift (shift=0/16/32/48) */
static uint32_t a64_movz(int rd, uint16_t imm16, int shift)
{
    return 0xD2800000u | (((uint32_t)(shift/16) & 0x3u) << 21) | ((uint32_t)imm16 << 5) | ((uint32_t)rd & 0x1Fu);
}

/* MOVK Xd, #imm16, LSL #shift */
static uint32_t a64_movk(int rd, uint16_t imm16, int shift)
{
    return 0xF2800000u | (((uint32_t)(shift/16) & 0x3u) << 21) | ((uint32_t)imm16 << 5) | ((uint32_t)rd & 0x1Fu);
}

/* LDR Xt, [Xn, #offset_bytes] (offset_bytes must be multiple of 8) */
static uint32_t a64_ldr(int rt, int rn, uint32_t off_bytes)
{
    return 0xF9400000u | (((off_bytes/8) & 0xFFFu) << 10) | (((uint32_t)rn & 0x1Fu) << 5) | ((uint32_t)rt & 0x1Fu);
}

/* BLR Xn */
static uint32_t a64_blr(int rn) { return 0xD63F0000u | (((uint32_t)rn & 0x1Fu) << 5); }

/* BR Xn */
static uint32_t a64_br(int rn)  { return 0xD61F0000u | (((uint32_t)rn & 0x1Fu) << 5); }

/* B #offset_bytes (offset_bytes multiple of 4) */
static uint32_t a64_b(int32_t off)
{
    return 0x14000000u | ((uint32_t)((off/4) & 0x3FFFFFF));
}

/* B.cond #offset_bytes */
static uint32_t a64_bcond(uint8_t cond, int32_t off)
{
    return 0x54000000u | (((uint32_t)((off/4) & 0x7FFFF)) << 5) | (cond & 0xFu);
}
#define A64_COND_LO  0x3   /* B.LO (unsigned below) */
#define A64_COND_NE  0x1   /* B.NE */

/* MRS Xd, CNTVCT_EL0 */
static uint32_t a64_mrs_cntvct(int rd) { return 0xD53BE040u | ((uint32_t)rd & 0x1Fu); }

/* ADD Xd, Xn, Xm (64-bit register) */
static uint32_t a64_add_reg(int rd, int rn, int rm)
{
    return 0x8B000000u | (((uint32_t)rm & 0x1Fu) << 16) | (((uint32_t)rn & 0x1Fu) << 5) | ((uint32_t)rd & 0x1Fu);
}

/* CMP Xn, Xm (= SUBS XZR, Xn, Xm) */
static uint32_t a64_cmp_reg(int rn, int rm)
{
    return 0xEB00001Fu | (((uint32_t)rm & 0x1Fu) << 16) | (((uint32_t)rn & 0x1Fu) << 5);
}

/* CBNZ Xd, #offset_bytes — if Xd != 0, branch */
static uint32_t a64_cbnz(int rt, int32_t off)
{
    return 0xB5000000u | (((uint32_t)((off/4) & 0x7FFFF)) << 5) | ((uint32_t)rt & 0x1Fu);
}

/* UXTB Wd, Wn */
static uint32_t a64_uxtb(int rd, int rn)
{
    return 0x53001C00u | (((uint32_t)rn & 0x1Fu) << 5) | ((uint32_t)rd & 0x1Fu);
}

/* CMP Wn, #imm12 (32-bit) */
static uint32_t a64_cmp_imm32(int rn, uint32_t imm12)
{
    return 0x7100001Fu | ((imm12 & 0xFFFu) << 10) | (((uint32_t)rn & 0x1Fu) << 5);
}

/* HLT #imm16 */
static uint32_t a64_hlt(uint16_t imm) { return 0xD4400000u | ((uint32_t)imm << 5); }

#define A64_NOP 0xD503201Fu


/* ================================================================
 * GEÇİŞ 1 — Tokenize
 * ================================================================ */
static void pass_tokenize(FILE *src)
{
    char line[4096];
    int lineno = 0;
    while (fgets(line, sizeof(line), src)) {
        lineno++;
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (g_ntokens >= MAX_TOKENS) { fprintf(stderr,"[hata] Token limiti aşıldı.\n"); exit(1); }
        Token *t = &g_tokens[g_ntokens];

        if      (strncmp(line, "write:\"", 7) == 0) {
            char *p = line+7, *e = strchr(p, '"');
            if (!e) { fprintf(stderr,"[hata] satır %d: kapanmayan tırnak.\n",lineno); exit(1); }
            t->type = TOK_WRITE;
            size_t l = (size_t)(e-p); memcpy(t->str,p,l); t->str[l]='\0';
            g_ntokens++;
        } else if (strncmp(line, "wait:", 5) == 0) {
            t->type = TOK_WAIT; t->seconds = atoi(line+5);
            if (t->seconds <= 0) { fprintf(stderr,"[uyarı] satır %d: wait: geçersiz süre, 1 sn.\n",lineno); t->seconds=1; }
            g_ntokens++;
        } else if (strcmp(line, "clean")    == 0) { t->type = TOK_CLEAN;    g_ntokens++;
        } else if (strcmp(line, "continue") == 0) { t->type = TOK_CONTINUE; g_ntokens++;
        } else if (strcmp(line, "halt")     == 0) { t->type = TOK_HALT;     g_ntokens++;
        } else if (strcmp(line, "if")       == 0) { t->type = TOK_IF_SYS;   g_ntokens++;
        } else if (strcmp(line, "read")            == 0) { t->type = TOK_READ;        g_ntokens++;
        } else if (strcmp(line, "invoke:riscv32") == 0) { t->type = TOK_INVOKE_RV32; g_ntokens++;
        } else if (strcmp(line, "invoke:aarch64") == 0) { t->type = TOK_INVOKE_A64;  g_ntokens++;
        } else if (strncmp(line, "visit:", 6) == 0) {
            const char *tgt = line+6; if (!*tgt) { fprintf(stderr,"[hata] satır %d: visit: hedef yok.\n",lineno); exit(1); }
            t->type = TOK_VISIT;
            size_t l = strlen(tgt); if (l >= MAX_STR_LEN) { fprintf(stderr,"[hata] satır %d: visit: uzun ad.\n",lineno); exit(1); }
            memcpy(t->visit_target, tgt, l+1); g_ntokens++;
        } else if (line[0] == ':') {
            const char *name = line+1; if (!*name) { fprintf(stderr,"[hata] satır %d: etiket adı boş.\n",lineno); exit(1); }
            t->type = TOK_LABEL_DEF;
            size_t l = strlen(name); if (l >= MAX_LABEL_LEN) { fprintf(stderr,"[hata] satır %d: etiket uzun.\n",lineno); exit(1); }
            memcpy(t->label_name, name, l+1); g_ntokens++;
        } else if (strncmp(line, "jump:", 5) == 0) {
            const char *name = line+5; if (!*name) { fprintf(stderr,"[hata] satır %d: jump: etiket yok.\n",lineno); exit(1); }
            t->type = TOK_JUMP;
            size_t l = strlen(name); if (l >= MAX_LABEL_LEN) { fprintf(stderr,"[hata] satır %d: jump: uzun ad.\n",lineno); exit(1); }
            memcpy(t->label_name, name, l+1); g_ntokens++;
        } else if (strncmp(line, "if:\"", 4) == 0 && line[5] == '"') {
            t->type = TOK_IF_CHAR; t->char_val = (uint8_t)line[4]; g_ntokens++;
        } else {
            fprintf(stderr,"[uyarı] satır %d: bilinmeyen token yoksayıldı: %s\n", lineno, line);
        }
    }
}

/* ================================================================
 * GEÇİŞ 2 — Emisyon planla
 * ================================================================ */
static void pass_plan(void)
{
    for (int i = 0; i < g_ntokens; i++) {
        Token    *t  = &g_tokens[i];
        Emission *em = &g_emissions[g_nemissions];
        if (g_nemissions >= MAX_EMISSIONS) { fprintf(stderr,"[hata] Emisyon limiti aşıldı.\n"); exit(1); }

        switch (t->type) {
            case TOK_WRITE:
                em->type = EM_WRITE_CALL; em->str_idx = pool_add(t->str,1); g_nemissions++;
                printf("[plan] WRITE      str[%d]=\"%s\"\n", em->str_idx, t->str); break;

            case TOK_WAIT:
                em->type = EM_WAIT; em->seconds = t->seconds; g_nemissions++;
                printf("[plan] WAIT       %d sn\n", t->seconds); break;

            case TOK_CLEAN: {
                em->type = EM_WRITE_CALL; em->str_idx = pool_add(CLEAN_ESC_SEQ,0); g_nemissions++;
                int skip = i+1;
                while (skip < g_ntokens &&
                       g_tokens[skip].type != TOK_CONTINUE &&
                       g_tokens[skip].type != TOK_WRITE    &&
                       g_tokens[skip].type != TOK_WAIT     &&
                       g_tokens[skip].type != TOK_CLEAN    &&
                       g_tokens[skip].type != TOK_VISIT)
                    skip++;
                if (skip < g_ntokens && g_tokens[skip].type == TOK_CONTINUE) {
                    i = skip; printf("[plan] CLEAN+CONTINUE\n");
                } else {
                    g_emissions[g_nemissions].type = EM_FREEZE; g_nemissions++;
                    printf("[plan] CLEAN+FREEZE\n");
                }
                break;
            }

            case TOK_CONTINUE:
                fprintf(stderr,"[uyarı] 'continue' 'clean' olmadan.\n"); break;

            case TOK_VISIT: {
                uint32_t addr = resolve_visit_addr(t->visit_target);
                em->type = EM_VISIT; em->visit_addr = addr; g_nemissions++;
                printf("[plan] VISIT      '%s' → 0x%08X\n", t->visit_target, addr); break;
            }

            case TOK_HALT:
                em->type = EM_HALT; g_nemissions++;
                printf("[plan] HALT\n"); break;

            case TOK_LABEL_DEF:
                em->type = EM_LABEL_DEF;
                memcpy(em->label_name, t->label_name, strlen(t->label_name)+1);
                g_nemissions++;
                printf("[plan] LABEL_DEF  :%s\n", t->label_name); break;

            case TOK_JUMP:
                em->type = EM_JUMP;
                memcpy(em->jump_target, t->label_name, strlen(t->label_name)+1);
                g_nemissions++;
                printf("[plan] JUMP       → :%s\n", t->label_name); break;

            case TOK_IF_CHAR:
                em->type = EM_IF_CHAR; em->char_val = t->char_val; g_nemissions++;
                printf("[plan] IF_CHAR    akü[7:0] != '%c'\n", (char)t->char_val); break;

            case TOK_IF_SYS:
                em->type = EM_IF_SYS; g_nemissions++;
                printf("[plan] IF_SYS     akü != 0\n"); break;

            case TOK_READ:
                em->type = EM_READ; g_nemissions++;
                printf("[plan] READ       seri port → akümülatör\n"); break;

            case TOK_INVOKE_RV32:
                em->type = EM_INVOKE_RV32; g_nemissions++;
                printf("[plan] INVOKE_RV32  FST[4] → invoke_rv32()\n"); break;

            case TOK_INVOKE_A64:
                em->type = EM_INVOKE_A64; g_nemissions++;
                printf("[plan] INVOKE_A64   FST[5] → invoke_a64()\n"); break;
        }
    }
    g_emissions[g_nemissions].type = EM_FREEZE; g_nemissions++;
    printf("[plan] FREEZE     → terminal döngü.\n");
}

/* ================================================================
 * GEÇİŞ 3a — Bayt ofsetleri + etiket tablosu
 * ================================================================ */
static int compute_offsets(void)
{
    int cur = 0;
    for (int i = 0; i < g_nemissions; i++) {
        g_em_byte_offset[i] = cur;
        if (g_emissions[i].type == EM_LABEL_DEF) {
            if (g_nlabels >= MAX_LABELS) { fprintf(stderr,"[hata] Etiket limiti aşıldı.\n"); exit(1); }
            const char *name = g_emissions[i].label_name;
            if (label_lookup(name) >= 0) { fprintf(stderr,"[hata] Etiket zaten tanımlı: '%s'\n",name); exit(1); }
            memcpy(g_labels[g_nlabels].label, name, strlen(name)+1);
            g_labels[g_nlabels].byte_offset = cur;
            g_nlabels++;
            printf("[ofset] :%s → bayt %d\n", name, cur);
        }
        cur += emission_bytes(&g_emissions[i]);
    }
    return cur;
}

/* ================================================================
 * GEÇİŞ 3b — x86-64 kodu yay
 * ================================================================ */
static void emit_x86_64(FILE *f, int data_start, int *str_byte_offset)
{
    for (int i = 0; i < g_nemissions; i++) {
        Emission *em  = &g_emissions[i];
        int       off = g_em_byte_offset[i];

        switch (em->type) {

            /* -- WRITE_CALL: 22 bayt ---------------------------------- */
            case EM_WRITE_CALL: {
                int str_at = data_start + str_byte_offset[em->str_idx];
                int32_t rel = (int32_t)(str_at - (off + 7));
                emit_u8(f,0x48); emit_u8(f,0x8D); emit_u8(f,0x3D);  /* lea rdi,[rip+rel] */
                emit_i32le(f, rel);
                emit_u8(f,0x48); emit_u8(f,0xB8); emit_u64le(f, FST_BASE);  /* movabs rax */
                emit_u8(f,0x48); emit_u8(f,0x8B); emit_u8(f,0x00);  /* mov rax,[rax] */
                emit_u8(f,0xFF); emit_u8(f,0xD0);                    /* call rax */
                printf("[x64]  WRITE_CALL  off=%d  rel=%d\n", off, rel); break;
            }

            /* -- WAIT: 36 bayt (RDTSC) -------------------------------- */
            case EM_WAIT: {
                uint64_t ticks = (uint64_t)em->seconds * X64_TSC_HZ;
                emit_u8(f,0x0F); emit_u8(f,0x31);                    /* rdtsc      2 */
                emit_u8(f,0x48); emit_u8(f,0xC1); emit_u8(f,0xE2); emit_u8(f,0x20); /* shl rdx,32 4 */
                emit_u8(f,0x48); emit_u8(f,0x09); emit_u8(f,0xD0);  /* or  rax,rdx 3 */
                emit_u8(f,0x48); emit_u8(f,0xB9); emit_u64le(f,ticks); /* movabs rcx 10 */
                emit_u8(f,0x48); emit_u8(f,0x01); emit_u8(f,0xC1);  /* add rcx,rax 3 */
                emit_u8(f,0x0F); emit_u8(f,0x31);                    /* rdtsc      2 */
                emit_u8(f,0x48); emit_u8(f,0xC1); emit_u8(f,0xE2); emit_u8(f,0x20); /* shl rdx,32 4 */
                emit_u8(f,0x48); emit_u8(f,0x09); emit_u8(f,0xD0);  /* or  rax,rdx 3 */
                emit_u8(f,0x48); emit_u8(f,0x3B); emit_u8(f,0xC1);  /* cmp rax,rcx 3 */
                emit_u8(f,0x72); emit_u8(f,0xF2);                    /* jb  -14     2 */
                printf("[x64]  WAIT        off=%d  sn=%d\n", off, em->seconds); break;
            }

            /* -- VISIT: 12 bayt --------------------------------------- */
            case EM_VISIT:
                emit_u8(f,0x48); emit_u8(f,0xB8); emit_u64le(f, em->visit_addr);
                emit_u8(f,0xFF); emit_u8(f,0xE0);
                printf("[x64]  VISIT        off=%d  addr=0x%08llX\n", off, (unsigned long long)em->visit_addr); break;

            /* -- FREEZE: 2 bayt --------------------------------------- */
            case EM_FREEZE:
                emit_u8(f,0xEB); emit_u8(f,0xFE);
                printf("[x64]  FREEZE       off=%d\n", off); break;

            /* -- HALT: 3 bayt ---------------------------------------- */
            case EM_HALT:
                emit_u8(f,0xF4); emit_u8(f,0xEB); emit_u8(f,0xFE);
                printf("[x64]  HALT         off=%d\n", off); break;

            /* -- LABEL_DEF: 0 bayt ------------------------------------ */
            case EM_LABEL_DEF:
                printf("[x64]  LABEL_DEF    :%s  off=%d\n", em->label_name, off); break;

            /* -- JUMP: 5 bayt (JMP rel32) ----------------------------- */
            case EM_JUMP: {
                int tgt = label_lookup(em->jump_target);
                if (tgt < 0) { fprintf(stderr,"[hata] jump: tanımsız etiket '%s'\n",em->jump_target); exit(1); }
                int32_t rel32 = (int32_t)(tgt - (off + 5));
                emit_u8(f,0xE9); emit_i32le(f, rel32);
                printf("[x64]  JUMP         off=%d  → :%s  rel=%d\n", off, em->jump_target, rel32); break;
            }

            /* -- IF_CHAR: 8 bayt (CMP al,imm + JNE rel32) ----------- */
            case EM_IF_CHAR: {
                int next_sz = (i+1 < g_nemissions) ? emission_bytes(&g_emissions[i+1]) : 0;
                emit_u8(f,0x3C); emit_u8(f, em->char_val);           /* cmp al, imm8  2 */
                emit_u8(f,0x0F); emit_u8(f,0x85); emit_i32le(f,(int32_t)next_sz); /* jne rel32 6 */
                printf("[x64]  IF_CHAR      off=%d  char='%c'  skip=%d\n", off, (char)em->char_val, next_sz); break;
            }

            /* -- IF_SYS: 8 bayt (TEST eax,eax + JNZ rel32) ----------- */
            case EM_IF_SYS: {
                int next_sz = (i+1 < g_nemissions) ? emission_bytes(&g_emissions[i+1]) : 0;
                emit_u8(f,0x85); emit_u8(f,0xC0);                    /* test eax,eax  2 */
                emit_u8(f,0x0F); emit_u8(f,0x85); emit_i32le(f,(int32_t)next_sz); /* jnz rel32 6 */
                printf("[x64]  IF_SYS       off=%d  skip=%d\n", off, next_sz); break;
            }

            /* -- READ: 10 bayt (inline COM1 poll) -------------------- */
            /* xor eax,eax / [loop] in al,0x3FD / test al,1 / jz loop / in al,0x3F8
             * loop target = off+2, JZ at off+6, after_JZ = off+8 → rel = -6 = 0xFA */
            case EM_READ:
                emit_u8(f,0x31); emit_u8(f,0xC0);  /* xor eax,eax       2 */
                emit_u8(f,0xE4); emit_u8(f,0xFD);  /* in  al,0x3FD      2 ← [loop] */
                emit_u8(f,0xA8); emit_u8(f,0x01);  /* test al,1         2 */
                emit_u8(f,0x74); emit_u8(f,0xFA);  /* jz  loop(-6)      2 */
                emit_u8(f,0xE4); emit_u8(f,0xF8);  /* in  al,0x3F8      2 */
                printf("[x64]  READ         off=%d  (COM1 0x3FD→0x3F8)\n", off); break;

            /* -- INVOKE_RV32: 16B (MOVABS FST_BASE + MOV [rax+32] + CALL) */
            /* FST[4]=invoke_rv32 @ FST_BASE+32. RAX clobberlanır.         */
            case EM_INVOKE_RV32:
                emit_u8(f,0x48); emit_u8(f,0xB8); emit_u64le(f, FST_BASE); /* MOVABS rax  10 */
                emit_u8(f,0x48); emit_u8(f,0x8B); emit_u8(f,0x40); emit_u8(f,0x20); /* MOV rax,[rax+32] 4 */
                emit_u8(f,0xFF); emit_u8(f,0xD0);                           /* CALL rax    2 */
                printf("[x64]  INVOKE_RV32  off=%d  FST[4] @ FST+32\n", off); break;

            /* -- INVOKE_A64: 16B (MOVABS FST_BASE + MOV [rax+40] + CALL) */
            /* FST[5]=invoke_a64 @ FST_BASE+40.                            */
            case EM_INVOKE_A64:
                emit_u8(f,0x48); emit_u8(f,0xB8); emit_u64le(f, FST_BASE); /* MOVABS rax  10 */
                emit_u8(f,0x48); emit_u8(f,0x8B); emit_u8(f,0x40); emit_u8(f,0x28); /* MOV rax,[rax+40] 4 */
                emit_u8(f,0xFF); emit_u8(f,0xD0);                           /* CALL rax    2 */
                printf("[x64]  INVOKE_A64   off=%d  FST[5] @ FST+40\n", off); break;
        }
    }
}

/* ================================================================
 * GEÇİŞ 3b — RISC-V 32 kodu yay
 * ================================================================ */
static void emit_riscv32(FILE *f, int data_start, int *str_byte_offset)
{
    for (int i = 0; i < g_nemissions; i++) {
        Emission *em  = &g_emissions[i];
        int       off = g_em_byte_offset[i];

        switch (em->type) {

            /* -- WRITE_CALL: 20 bayt (auipc+addi a0 + lui t0+lw+jalr) */
            case EM_WRITE_CALL: {
                int str_at = data_start + str_byte_offset[em->str_idx];
                int32_t delta = (int32_t)(str_at - off);  /* delta from auipc */
                rv32_auipc_a0(f, delta);                   /* 8 bayt */
                rv32_fst_call(f, 0);                       /* 12 bayt */
                printf("[rv32] WRITE_CALL  off=%d  delta=%d\n", off, delta); break;
            }

            /* -- WAIT: 28 bayt (MTIME busy-wait) --------------------- */
            case EM_WAIT: {
                uint64_t ticks = (uint64_t)em->seconds * RV32_MTIME_HZ;
                rv32_mtime_wait(f, ticks);
                printf("[rv32] WAIT        off=%d  sn=%d  ticks=%llu\n", off, em->seconds, (unsigned long long)ticks); break;
            }

            /* -- VISIT: 8 bayt (lui t1 + jalr x0) -------------------- */
            case EM_VISIT:
                rv32_abs_jump(f, (uint32_t)em->visit_addr);
                printf("[rv32] VISIT       off=%d  addr=0x%08X\n", off, (uint32_t)em->visit_addr); break;

            /* -- FREEZE: 4 bayt (jal x0, 0) -------------------------- */
            case EM_FREEZE:
                emit_u32le(f, 0x0000006Fu);
                printf("[rv32] FREEZE      off=%d\n", off); break;

            /* -- HALT: 8 bayt (wfi + jal x0,-4) ---------------------- */
            case EM_HALT:
                emit_u32le(f, 0x10500073u);   /* wfi */
                rv32_jal_x0(f, -4);
                printf("[rv32] HALT        off=%d\n", off); break;

            /* -- LABEL_DEF: 0 bayt ------------------------------------ */
            case EM_LABEL_DEF:
                printf("[rv32] LABEL_DEF   :%s  off=%d\n", em->label_name, off); break;

            /* -- JUMP: 4 bayt (jal x0, rel) --------------------------- */
            case EM_JUMP: {
                int tgt = label_lookup(em->jump_target);
                if (tgt < 0) { fprintf(stderr,"[hata] jump: tanımsız etiket '%s'\n",em->jump_target); exit(1); }
                rv32_jal_x0(f, (int32_t)(tgt - off));
                printf("[rv32] JUMP        off=%d  → :%s  rel=%d\n", off, em->jump_target, tgt-off); break;
            }

            /* -- IF_CHAR: 8 bayt (addi t0,x0,char + bne s0,t0,skip) -- */
            case EM_IF_CHAR: {
                int next_sz = (i+1 < g_nemissions) ? emission_bytes(&g_emissions[i+1]) : 0;
                uint32_t addi_t0 = ((uint32_t)(em->char_val & 0xFFF) << 20) | 0x293u; /* addi t0,x0,c */
                emit_u32le(f, addi_t0);
                /* bne s0(x8), t0(x5), +next_sz: offset from BNE itself = next_sz */
                rv32_bne(f, 8, 5, next_sz);
                printf("[rv32] IF_CHAR     off=%d  char='%c'  skip=%d\n", off, (char)em->char_val, next_sz); break;
            }

            /* -- IF_SYS: 4 bayt (bne s0,x0,skip) --------------------- */
            case EM_IF_SYS: {
                int next_sz = (i+1 < g_nemissions) ? emission_bytes(&g_emissions[i+1]) : 0;
                rv32_bne(f, 8, 0, next_sz);
                printf("[rv32] IF_SYS      off=%d  skip=%d\n", off, next_sz); break;
            }

            /* -- READ: 20 bayt (5 sözcük, inline NS16550) ------------ */
            /* NS16550A @ 0x10000000:
             *   lui  s0, 0x10000        ; s0 = 0x10000000
             * [loop:]
             *   lbu  t0, 5(s0)          ; t0 = LSR
             *   andi t0, t0, 1          ; bit0 = Data Ready
             *   beqz t0, loop (-8)      ; BEQ x5,x0,-8 = 0xFE028CE3
             *   lbu  s0, 0(s0)          ; s0 = RBR → akümülatör */
            case EM_READ:
                emit_u32le(f, 0x10000437u);  /* lui  s0, 0x10000             */
                emit_u32le(f, 0x00544283u);  /* lbu  t0, 5(s0)  [loop]       */
                emit_u32le(f, 0x0012F293u);  /* andi t0, t0, 1               */
                emit_u32le(f, 0xFE028CE3u);  /* beqz t0, -8  (BEQ x5,x0,-8) */
                emit_u32le(f, 0x00044403u);  /* lbu  s0, 0(s0)               */
                printf("[rv32] READ        off=%d  (NS16550 0x10000000)\n", off); break;

            /* -- INVOKE_RV32: 12B → rv32_fst_call(slot=4) ------------- */
            case EM_INVOKE_RV32:
                rv32_fst_call(f, 4);
                printf("[rv32] INVOKE_RV32  off=%d  FST[4]\n", off); break;

            /* -- INVOKE_A64: 12B → rv32_fst_call(slot=5) -------------- */
            case EM_INVOKE_A64:
                rv32_fst_call(f, 5);
                printf("[rv32] INVOKE_A64   off=%d  FST[5]\n", off); break;
        }
    }
}

/* ================================================================
 * GEÇİŞ 3b — AArch64 kodu yay
 * ================================================================ */
static void emit_aarch64(FILE *f, int data_start, int *str_byte_offset)
{
    for (int i = 0; i < g_nemissions; i++) {
        Emission *em  = &g_emissions[i];
        int       off = g_em_byte_offset[i];

        switch (em->type) {

            /* -- WRITE_CALL: 20 bayt (5 komut) ----------------------- */
            /*    ADRP x0, pg + ADD x0,x0,pgoff + MOVZ x9 + LDR x9 + BLR x9 */
            case EM_WRITE_CALL: {
                int str_at   = data_start + str_byte_offset[em->str_idx];
                /* ADRP: page_delta = (str_page - adrp_page) / 4096 */
                int pg_tgt   = str_at & ~0xFFF;
                int pg_adrp  = off    & ~0xFFF;
                int32_t pgd  = (int32_t)((pg_tgt - pg_adrp) / 4096);
                uint32_t pg_off = (uint32_t)(str_at & 0xFFF);
                emit_u32le(f, a64_adrp(0, pgd));                     /* ADRP x0 */
                emit_u32le(f, a64_add_imm64(0, 0, pg_off));          /* ADD  x0,x0,pgoff */
                emit_u32le(f, a64_movz(9, (uint16_t)((FST_BASE>>16)&0xFFFF), 16)); /* MOVZ x9 */
                emit_u32le(f, a64_ldr(9, 9, 0));                     /* LDR  x9,[x9] */
                emit_u32le(f, a64_blr(9));                            /* BLR  x9 */
                printf("[a64]  WRITE_CALL  off=%d  pgd=%d  pg_off=%u\n", off, pgd, pg_off); break;
            }

            /* -- WAIT: 36 bayt (CNTVCT_EL0 busy-wait, 9 komut) ------- */
            case EM_WAIT: {
                uint64_t ticks = (uint64_t)em->seconds * A64_CNTFRQ;
                emit_u32le(f, a64_mrs_cntvct(1));                    /* MRS x1,CNTVCT  (start) */
                emit_u32le(f, a64_movz(3, (uint16_t)(ticks & 0xFFFF), 0));
                emit_u32le(f, a64_movk(3, (uint16_t)((ticks>>16)&0xFFFF), 16));
                emit_u32le(f, a64_movk(3, (uint16_t)((ticks>>32)&0xFFFF), 32));
                emit_u32le(f, a64_add_reg(3, 3, 1));                 /* x3 = ticks + start */
                /* [loop: off+20] */
                emit_u32le(f, a64_mrs_cntvct(1));                    /* MRS x1,CNTVCT  (now) */
                emit_u32le(f, a64_cmp_reg(1, 3));                    /* CMP x1,x3 */
                emit_u32le(f, a64_bcond(A64_COND_LO, -8));           /* B.LO loop  (-8 bytes) */
                emit_u32le(f, A64_NOP);                               /* NOP padding */
                printf("[a64]  WAIT        off=%d  sn=%d  ticks=%llu\n", off, em->seconds, (unsigned long long)ticks); break;
            }

            /* -- VISIT: 12 bayt (MOVZ/MOVK x9 + BR x9) --------------- */
            case EM_VISIT: {
                uint64_t addr = em->visit_addr;
                emit_u32le(f, a64_movz(9, (uint16_t)(addr & 0xFFFF), 0));
                emit_u32le(f, a64_movk(9, (uint16_t)((addr>>16)&0xFFFF), 16));
                emit_u32le(f, a64_br(9));
                printf("[a64]  VISIT       off=%d  addr=0x%08llX\n", off, (unsigned long long)addr); break;
            }

            /* -- FREEZE: 4 bayt (B #0 = kendine atlama) --------------- */
            case EM_FREEZE:
                emit_u32le(f, a64_b(0));
                printf("[a64]  FREEZE      off=%d\n", off); break;

            /* -- HALT: 8 bayt (HLT #0 + B #-4) ----------------------- */
            case EM_HALT:
                emit_u32le(f, a64_hlt(0));                            /* HLT #0 */
                emit_u32le(f, a64_b(-4));                             /* B  #-4 (back to HLT) */
                printf("[a64]  HALT        off=%d\n", off); break;

            /* -- LABEL_DEF: 0 bayt ------------------------------------ */
            case EM_LABEL_DEF:
                printf("[a64]  LABEL_DEF   :%s  off=%d\n", em->label_name, off); break;

            /* -- JUMP: 4 bayt (B rel) --------------------------------- */
            case EM_JUMP: {
                int tgt = label_lookup(em->jump_target);
                if (tgt < 0) { fprintf(stderr,"[hata] jump: tanımsız etiket '%s'\n",em->jump_target); exit(1); }
                emit_u32le(f, a64_b((int32_t)(tgt - off)));
                printf("[a64]  JUMP        off=%d  → :%s  rel=%d\n", off, em->jump_target, tgt-off); break;
            }

            /* -- IF_CHAR: 12 bayt (UXTB + CMP + B.NE) ---------------- */
            case EM_IF_CHAR: {
                int next_sz = (i+1 < g_nemissions) ? emission_bytes(&g_emissions[i+1]) : 0;
                emit_u32le(f, a64_uxtb(1, 0));                       /* UXTB w1,w0  4 */
                emit_u32le(f, a64_cmp_imm32(1, em->char_val));       /* CMP  w1,#c  4 */
                emit_u32le(f, a64_bcond(A64_COND_NE, 4 + next_sz)); /* B.NE skip   4 */
                printf("[a64]  IF_CHAR     off=%d  char='%c'  skip=%d\n", off, (char)em->char_val, next_sz); break;
            }

            /* -- IF_SYS: 4 bayt (CBNZ x0, skip) --------------------- */
            case EM_IF_SYS: {
                int next_sz = (i+1 < g_nemissions) ? emission_bytes(&g_emissions[i+1]) : 0;
                emit_u32le(f, a64_cbnz(0, 4 + next_sz));
                printf("[a64]  IF_SYS      off=%d  skip=%d\n", off, next_sz); break;
            }

            /* -- READ: 20 bayt (5 komut, inline PL011) --------------- */
            /* PL011 @ 0x09000000  UARTFR=+0x18  UARTDR=+0x00  RXFE=bit4
             *   MOVZ x1, #0x0900, LSL#16    ; x1 = 0x09000000
             * [loop: +4]
             *   LDR  w0, [x1, #0x18]        ; w0 = UARTFR
             *   TST  w0, #0x10              ; RXFE?
             *   B.NE loop (-12)             ; boşsa bekle
             *   LDR  w0, [x1, #0]          ; w0 = UARTDR → akümülatör */
            case EM_READ:
                emit_u32le(f, 0xD2A12001u);  /* MOVZ x1,#0x0900,LSL#16 */
                emit_u32le(f, 0xB9401820u);  /* LDR  w0,[x1,#0x18]     */
                emit_u32le(f, 0x721C001Fu);  /* TST  w0,#0x10           */
                emit_u32le(f, 0x54FFFFA1u);  /* B.NE -12                */
                emit_u32le(f, 0xB9400020u);  /* LDR  w0,[x1,#0]        */
                printf("[a64]  READ        off=%d  (PL011 0x09000000)\n", off); break;

            /* -- INVOKE_RV32: 12B (MOVZ x9 + LDR x9,[x9,#32] + BLR x9) */
            /* FST[4] = invoke_rv32 @ FST_BASE + 4*8 = FST_BASE + 32     */
            case EM_INVOKE_RV32:
                emit_u32le(f, a64_movz(9, (uint16_t)((FST_BASE>>16)&0xFFFF), 16));
                emit_u32le(f, a64_ldr(9, 9, 4*8));  /* LDR x9,[x9,#32] */
                emit_u32le(f, a64_blr(9));
                printf("[a64]  INVOKE_RV32  off=%d  FST[4]+32\n", off); break;

            /* -- INVOKE_A64: 12B (MOVZ x9 + LDR x9,[x9,#40] + BLR x9) */
            /* FST[5] = invoke_a64 @ FST_BASE + 5*8 = FST_BASE + 40     */
            case EM_INVOKE_A64:
                emit_u32le(f, a64_movz(9, (uint16_t)((FST_BASE>>16)&0xFFFF), 16));
                emit_u32le(f, a64_ldr(9, 9, 5*8));  /* LDR x9,[x9,#40] */
                emit_u32le(f, a64_blr(9));
                printf("[a64]  INVOKE_A64   off=%d  FST[5]+40\n", off); break;
        }
    }
}

/* ================================================================
 * GEÇİŞ 3 — Ana yayıcı (hedef seçimi + veri bölümü)
 * ================================================================ */
static void pass_emit(FILE *out, const char *outfile)
{
    int total_code = compute_offsets();

    int str_byte_offset[MAX_STRINGS];
    int cum = 0;
    for (int i = 0; i < g_nstrings; i++) {
        str_byte_offset[i] = cum;
        cum += g_strings[i].len;
    }

    printf("[emit] Kod    : %d bayt\n", total_code);
    printf("[emit] Veri   : %d bayt  (%d dize)\n", cum, g_nstrings);
    printf("[emit] Toplam : %d bayt  →  %s\n", total_code + cum, outfile);

    /* Hedef başına kodu yay */
    if      (g_target == TARGET_X86_64)  emit_x86_64 (out, total_code, str_byte_offset);
    else if (g_target == TARGET_RISCV32) emit_riscv32(out, total_code, str_byte_offset);
    else                                  emit_aarch64(out, total_code, str_byte_offset);

    /* Veri bölümü: null-sonlu dizeler */
    for (int i = 0; i < g_nstrings; i++)
        fwrite(g_strings[i].data, 1, (size_t)g_strings[i].len, out);
}

/* ================================================================
 * Ana giriş noktası
 * ================================================================ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Kullanım: %s <kaynak.meclpp> [--target=x86_64|riscv32|aarch64]\n", argv[0]);
        return 1;
    }

    const char *src_path = argv[1];

    /* --target ayrıştır */
    for (int a = 2; a < argc; a++) {
        if      (strncmp(argv[a], "--target=x86_64",  15) == 0) g_target = TARGET_X86_64;
        else if (strncmp(argv[a], "--target=riscv32", 16) == 0) g_target = TARGET_RISCV32;
        else if (strncmp(argv[a], "--target=aarch64", 16) == 0) g_target = TARGET_AARCH64;
        else { fprintf(stderr,"[uyarı] Bilinmeyen bayrak yoksayıldı: %s\n", argv[a]); }
    }

    const char *tgt_name = (g_target == TARGET_X86_64) ? "x86-64" :
                           (g_target == TARGET_RISCV32) ? "RISC-V 32 (RV32I)" : "AArch64";

    char outfile[512];
    const char *dot = strrchr(src_path, '.');
    if (dot && strcmp(dot, ".meclpp") == 0) {
        size_t base = (size_t)(dot - src_path);
        if (base >= sizeof(outfile)-8) { fprintf(stderr,"[hata] Dosya adı uzun.\n"); return 1; }
        memcpy(outfile, src_path, base);
        memcpy(outfile+base, ".fiawo", 7);
    } else {
        memcpy(outfile, "program.fiawo", 14);
    }

    FILE *src = fopen(src_path, "r");
    if (!src) { fprintf(stderr,"[hata] Kaynak açılamadı: %s\n", src_path); return 1; }

    printf("================================================================\n");
    printf("  FIRMAWORK meclpp Derleyici  v6  (Çapraz Platform)\n");
    printf("  Hedef  : %s\n", tgt_name);
    printf("  Kaynak : %s\n", src_path);
    printf("  Çıktı  : %s\n", outfile);
    printf("================================================================\n");

    printf("[1] Tokenize ediliyor...\n");
    pass_tokenize(src);
    fclose(src);
    printf("[1] %d token.\n", g_ntokens);

    printf("[2] Emisyon planlanıyor...\n");
    pass_plan();
    printf("[2] %d emisyon | %d dize.\n", g_nemissions, g_nstrings);

    FILE *out = fopen(outfile, "wb");
    if (!out) { fprintf(stderr,"[hata] Çıktı oluşturulamadı: %s\n", outfile); return 1; }

    printf("[3] Makine kodu yayılıyor (%s)...\n", tgt_name);
    pass_emit(out, outfile);
    fclose(out);

    printf("================================================================\n");
    printf("  Derleme tamamlandı.  →  %s  [%s]\n", outfile, tgt_name);
    printf("================================================================\n");
    return 0;
}
