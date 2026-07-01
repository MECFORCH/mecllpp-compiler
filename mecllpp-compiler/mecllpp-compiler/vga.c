/* ================================================================
 * FIRMAWORK VGA Framebuffer Sürücüsü — Uygulama
 * ================================================================ */

#include "vga.h"
#include "font.h"

/* ----------------------------------------------------------------
 * İç yardımcılar
 * ---------------------------------------------------------------- */

/* Varsayılan framebuffer taban adresi — gerçek mod INT 0x10 (VBE) geçişi
 * henüz bağlanmadıysa kullanılır (QEMU std VGA/Bochs VBE varsayılan LFB). */
static vga_uint32_t vga_fb_base = 0xFD000000UL;

void vga_set_base(vga_uint32_t addr)
{
    vga_fb_base = addr;
}

vga_uint32_t vga_get_base(void)
{
    return vga_fb_base;
}

static inline volatile vga_uint32_t *fb_ptr(void)
{
    return (volatile vga_uint32_t *)(unsigned long)vga_fb_base;
}

/* ----------------------------------------------------------------
 * vga_set_pixel / vga_get_pixel
 * ---------------------------------------------------------------- */
void vga_set_pixel(int x, int y, vga_uint32_t color)
{
    if ((unsigned int)x >= VGA_WIDTH || (unsigned int)y >= VGA_HEIGHT)
        return;
    fb_ptr()[y * VGA_WIDTH + x] = color;
}

vga_uint32_t vga_get_pixel(int x, int y)
{
    if ((unsigned int)x >= VGA_WIDTH || (unsigned int)y >= VGA_HEIGHT)
        return 0;
    return fb_ptr()[y * VGA_WIDTH + x];
}

/* ----------------------------------------------------------------
 * vga_clear  —  ekranı tek renkle doldur
 * ---------------------------------------------------------------- */
void vga_clear(vga_uint32_t bg_color)
{
    volatile vga_uint32_t *fb = fb_ptr();
    int total = VGA_WIDTH * VGA_HEIGHT;
    for (int i = 0; i < total; i++)
        fb[i] = bg_color;
}

/* ----------------------------------------------------------------
 * draw_glyph_at  —  iç çizim motoru
 *
 * Verilen glyph (16-bayt satır dizisi) col,row karakter hücresine
 * fg/bg renkleriyle piksel piksel çizilir.
 * ---------------------------------------------------------------- */
static void draw_glyph_at(int col, int row,
                           const vga_uint8_t *glyph,
                           vga_uint32_t fg, vga_uint32_t bg)
{
    int px = col * FONT_W;
    int py = row * FONT_H;

    if (px + FONT_W > VGA_WIDTH || py + FONT_H > VGA_HEIGHT)
        return;

    volatile vga_uint32_t *fb = fb_ptr();

    for (int r = 0; r < FONT_H; r++) {
        vga_uint8_t row_bits = glyph[r];
        int base = (py + r) * VGA_WIDTH + px;
        for (int bit = 7; bit >= 0; bit--) {
            fb[base + (7 - bit)] = (row_bits >> bit) & 1 ? fg : bg;
        }
    }
}

/* ----------------------------------------------------------------
 * vga_putc_ascii
 * ---------------------------------------------------------------- */
void vga_putc_ascii(int col, int row,
                    unsigned char c,
                    vga_uint32_t fg, vga_uint32_t bg)
{
    draw_glyph_at(col, row, font_get_glyph_ascii(c), fg, bg);
}

/* ----------------------------------------------------------------
 * vga_putc_tr
 * ---------------------------------------------------------------- */
void vga_putc_tr(int col, int row,
                 int tr_idx,
                 vga_uint32_t fg, vga_uint32_t bg)
{
    draw_glyph_at(col, row, font_get_glyph_tr(tr_idx), fg, bg);
}

/* ----------------------------------------------------------------
 * vga_puts  —  ASCII string yaz, '\n' ile alt satıra geç
 * ---------------------------------------------------------------- */
void vga_puts(int col, int row,
              const char *s,
              vga_uint32_t fg, vga_uint32_t bg,
              int *out_col, int *out_row)
{
    int c = col, r = row;
    while (*s) {
        if (*s == '\n') {
            c = col;
            r++;
        } else {
            if (c >= VGA_COLS) { c = 0; r++; }
            if (r >= VGA_ROWS) r = 0;
            vga_putc_ascii(c, r, (unsigned char)*s, fg, bg);
            c++;
        }
        s++;
    }
    if (out_col) *out_col = c;
    if (out_row) *out_row = r;
}

/* ----------------------------------------------------------------
 * vga_puts_ex  —  karma ASCII + Türkçe karakter dizisi yaz
 * ---------------------------------------------------------------- */
void vga_puts_ex(int col, int row,
                 const VgaStr *chunks,
                 vga_uint32_t fg, vga_uint32_t bg)
{
    int c = col, r = row;
    for (int i = 0; chunks[i].type != VGA_CHUNK_END; i++) {
        if (chunks[i].type == VGA_CHUNK_ASCII) {
            const char *s = chunks[i].ascii_str;
            while (*s) {
                if (*s == '\n') { c = col; r++; }
                else {
                    if (c >= VGA_COLS) { c = 0; r++; }
                    if (r >= VGA_ROWS) r = 0;
                    vga_putc_ascii(c, r, (unsigned char)*s, fg, bg);
                    c++;
                }
                s++;
            }
        } else if (chunks[i].type == VGA_CHUNK_TR) {
            if (c >= VGA_COLS) { c = 0; r++; }
            if (r >= VGA_ROWS) r = 0;
            vga_putc_tr(c, r, chunks[i].tr_idx, fg, bg);
            c++;
        }
    }
}

/* ----------------------------------------------------------------
 * Çizgi ve dikdörtgen yardımcıları
 * ---------------------------------------------------------------- */
void vga_hline(int x, int y, int len, vga_uint32_t color)
{
    for (int i = 0; i < len; i++)
        vga_set_pixel(x + i, y, color);
}

void vga_vline(int x, int y, int len, vga_uint32_t color)
{
    for (int i = 0; i < len; i++)
        vga_set_pixel(x, y + i, color);
}

void vga_rect(int x, int y, int w, int h, vga_uint32_t color)
{
    vga_hline(x, y,         w, color);
    vga_hline(x, y + h - 1, w, color);
    vga_vline(x,         y, h, color);
    vga_vline(x + w - 1, y, h, color);
}

void vga_fill_rect(int x, int y, int w, int h, vga_uint32_t color)
{
    for (int row = y; row < y + h; row++)
        vga_hline(x, row, w, color);
}
