/* ================================================================
 * FIRMAWORK VGA Framebuffer Sürücüsü — Başlık
 *
 * Hedef: QEMU RISC-V Virt board, 640×480, 32bpp
 *
 * QEMU başlatma komutu (örnek):
 *   qemu-system-riscv32 -machine virt \
 *     -device VGA \
 *     -kernel firmware.elf \
 *     ...
 *
 * VGA_FB_BASE: QEMU virt board'da PCI MMIO bölgesi 0x40000000'dan
 *              başlar. VGA cihazının BAR0'ı buraya düşer.
 *              Farklı bir adres gerekirse bu sabiti değiştirin.
 * ================================================================ */

#ifndef VGA_H
#define VGA_H

typedef unsigned int       vga_uint32_t;
typedef unsigned short     vga_uint16_t;
typedef unsigned char      vga_uint8_t;

/* ----------------------------------------------------------------
 * Donanım sabitleri
 * ---------------------------------------------------------------- */
#define VGA_FB_BASE     0x40000000UL   /* PCI BAR0 — framebuffer başlangıcı */
#define VGA_WIDTH       640
#define VGA_HEIGHT      480
#define VGA_BPP         4              /* bayt/piksel (32-bit XRGB8888)      */
#define VGA_STRIDE      (VGA_WIDTH * VGA_BPP)

/* ----------------------------------------------------------------
 * Renk sabitleri  (0xXXRRGGBB)
 * ---------------------------------------------------------------- */
#define VGA_BLACK       0x00000000UL
#define VGA_WHITE       0x00FFFFFFUL
#define VGA_RED         0x00FF0000UL
#define VGA_GREEN       0x0000FF00UL
#define VGA_BLUE        0x000000FFUL
#define VGA_YELLOW      0x00FFFF00UL
#define VGA_CYAN        0x0000FFFFUL
#define VGA_MAGENTA     0x00FF00FFUL
#define VGA_GRAY        0x00AAAAAAUL
#define VGA_DARK_GRAY   0x00555555UL
#define VGA_ORANGE      0x00FF8800UL

/* ----------------------------------------------------------------
 * Ekran boyutu (karakter bazlı, 8×16 font)
 * ---------------------------------------------------------------- */
#define VGA_COLS        (VGA_WIDTH  / 8)   /* 80 sütun  */
#define VGA_ROWS        (VGA_HEIGHT / 16)  /* 30 satır  */

/* ----------------------------------------------------------------
 * API
 * ---------------------------------------------------------------- */

/* Ekranı tek renkle doldur */
void vga_clear(vga_uint32_t bg_color);

/* x,y piksel konumuna 32-bit renk yaz */
void vga_set_pixel(int x, int y, vga_uint32_t color);

/* Piksel rengi oku */
vga_uint32_t vga_get_pixel(int x, int y);

/* Bir ASCII karakteri çiz (col, row = karakter hücresi koordinatı) */
void vga_putc_ascii(int col, int row,
                    unsigned char c,
                    vga_uint32_t fg, vga_uint32_t bg);

/* Bir Türkçe karakteri çiz (idx = FONT_TR_xxx) */
void vga_putc_tr(int col, int row,
                 int tr_idx,
                 vga_uint32_t fg, vga_uint32_t bg);

/* ASCII string yaz; '\n' ile satır başına döner.
 * Türkçe için vga_puts_tr() kullanın.                     */
void vga_puts(int col, int row,
              const char *s,
              vga_uint32_t fg, vga_uint32_t bg,
              int *out_col, int *out_row);

/* ----------------------------------------------------------------
 * Türkçe dizi yazdırma yardımcı yapısı.
 *
 * Kullanım:
 *   VgaStr parçalar[] = {
 *       VGA_ASCII("Merhaba "),
 *       VGA_TR(FONT_TR_d_umlaut),   // ö
 *       VGA_ASCII("rnek"),
 *       VGA_END
 *   };
 *   vga_puts_ex(2, 5, parçalar, VGA_WHITE, VGA_BLACK);
 * ---------------------------------------------------------------- */
#define VGA_CHUNK_ASCII  0
#define VGA_CHUNK_TR     1
#define VGA_CHUNK_END    2

typedef struct {
    int         type;        /* VGA_CHUNK_ASCII / VGA_CHUNK_TR / VGA_CHUNK_END */
    const char *ascii_str;   /* VGA_CHUNK_ASCII için */
    int         tr_idx;      /* VGA_CHUNK_TR    için */
} VgaStr;

#define VGA_ASCII(s)  { VGA_CHUNK_ASCII, (s), 0 }
#define VGA_TR(idx)   { VGA_CHUNK_TR, 0, (idx) }
#define VGA_END       { VGA_CHUNK_END,  0, 0 }

/* Karma ASCII+Türkçe dizi yaz */
void vga_puts_ex(int col, int row,
                 const VgaStr *chunks,
                 vga_uint32_t fg, vga_uint32_t bg);

/* Yatay çizgi çiz */
void vga_hline(int x, int y, int len, vga_uint32_t color);

/* Dikey çizgi çiz */
void vga_vline(int x, int y, int len, vga_uint32_t color);

/* Dikdörtgen çerçeve çiz */
void vga_rect(int x, int y, int w, int h, vga_uint32_t color);

/* Dolu dikdörtgen */
void vga_fill_rect(int x, int y, int w, int h, vga_uint32_t color);

#endif /* VGA_H */
