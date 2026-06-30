/* ================================================================
 * FIRMAWORK Bare-Metal Firmware  —  x86-64 Long Mode
 *
 * Bellek Haritası:
 *   0x000B8000  VGA Metin Tamponu (80×25, 16 renk)
 *   0x000F0000  BIOS ROM bölgesi
 *   0x00100000  Firmware kodu (bu ikili, Multiboot tarafından yüklenir)
 *   0x00200000  RISC-V 32 mikro-kernel blobu  (firmware.bin içine gömülü)
 *   0x00300000  AArch64 mikro-kernel blobu   (firmware.bin içine gömülü)
 *   0x80100000  Firmware Servis Tablosu (FST) — 6 yuva
 *   0x80200000  .fiawo Segment 0  (QEMU -device loader ile yüklenir)
 *   0x80300000  .fiawo Segment 1  (visit hedefi — main.meclpp)
 *
 * Firmware Servis Tablosu (x86-64, 64-bit işaretçiler):
 *   FST[0]  serial_puts(const char *)
 *   FST[1]  serial_putc(char)
 *   FST[2]  serial_getc(void) → char
 *   FST[3]  serial_puthex(unsigned long)
 *   FST[4]  invoke_rv32(void) — RV32 blob @ 0x200000 hakkında bilgi ver
 *   FST[5]  invoke_a64(void)  — A64 blob @ 0x300000 hakkında bilgi ver
 *
 * .fiawo Yükleyici:
 *   QEMU komutu: -device loader,file=kod.fiawo,addr=0x80200000,force-raw=on
 *   Firmware menüsü [3] ile o adrese atlanır ve x86-64 kodu çalıştırılır.
 *   .fiawo ikilileri derleyici v4 tarafından üretilir (derleyici.c → x86-64).
 *
 * Donanım:
 *   Seri Port : COM1 (0x3F8) — UART MMIO yerine x86 I/O portları
 *   Ekran     : VGA Metin Modu (0xB8000), 80×25, CP437
 *
 * Derleme:
 *   clang --target=x86_64-elf -nostdlib -ffreestanding \
 *         -fno-stack-protector -mno-red-zone           \
 *         -T linker.ld entry.s main.c -o firmware.elf
 * ================================================================ */

/* ---- Temel türler (libc yok) ---- */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;
typedef unsigned long      uintptr_t;
typedef signed long        intptr_t;
typedef unsigned long      size_t;

/* ================================================================
 * COM1 Seri Port Sürücüsü  (x86 I/O port 0x3F8)
 *
 * NS16550A MMIO yerine x86 IN/OUT komutları kullanılır.
 * ================================================================ */
#define COM1_BASE   0x3F8u

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1"
                      : : "a"(val), "Nd"(port) : "memory");
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1"
                      : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0"
                      : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

static void serial_init(void)
{
    outb(COM1_BASE + 1, 0x00);  /* Tüm kesmeler devre dışı       */
    outb(COM1_BASE + 3, 0x80);  /* DLAB aç (Baud Rate Bölücü)    */
    outb(COM1_BASE + 0, 0x01);  /* 115200 baud — bölücü düşük    */
    outb(COM1_BASE + 1, 0x00);  /* bölücü yüksek                  */
    outb(COM1_BASE + 3, 0x03);  /* 8 bit, parite yok, 1 stop biti */
    outb(COM1_BASE + 2, 0xC7);  /* FIFO etkin, 14 bayt eşiği      */
    outb(COM1_BASE + 4, 0x0B);  /* RTS/DSR set                    */
}

/* TX hazır olana dek bekle, sonra karakteri gönder */
void serial_putc(char c)
{
    while (!(inb(COM1_BASE + 5) & 0x20))
        ;
    outb(COM1_BASE, (uint8_t)c);
}

/* Null-sonlandırmalı dizeyi gönder; '\n' → '\r\n' */
void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

/* RX hazır olana dek bekle, karakteri oku */
char serial_getc(void)
{
    while (!(inb(COM1_BASE + 5) & 0x01))
        ;
    return (char)inb(COM1_BASE);
}

/* 64-bit değeri "0x" + 16 onaltılık basamak olarak yaz */
void serial_puthex(uint64_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    serial_puts("0x");
    for (i = 15; i >= 0; i--)
        serial_putc(hex[(val >> (i * 4)) & 0xF]);
    serial_puts("\r\n");
}

/* ================================================================
 * PIT (8253/8254) + TSC Sürücüsü
 *
 * Port haritası:
 *   0x40  Kanal 0 veri   (sistem timer — IRQ0 için)
 *   0x42  Kanal 2 veri   (hoparlör — polling delay için)
 *   0x43  Mode/Komut yazmaçı
 *   0x61  Sistem kontrol — bit0=Kanal2 gate, bit1=hoparlör, bit5=OUT2
 *
 * RDTSC ile uptime:
 *   Boot'ta g_boot_tsc kaydedilir.
 *   PIT kanal 2 üzerinden 10 ms ölçümle g_tsc_khz kalibre edilir.
 *   Sonraki her rdtsc() - g_boot_tsc / g_tsc_khz = ms uptime verir.
 * ================================================================ */

/* İleri bildirimler — PIT yardımcıları bunlara ihtiyaç duyar */
static void int_to_dec(uint64_t val, char *buf);
static void serial_put2d(uint8_t v);

#define PIT_FREQ_HZ   1193182UL   /* PIT referans frekansı */
#define PIT_TICK_HZ   1000u       /* Kanal 0 hedef frekansı (scheduler) */

static uint64_t g_boot_tsc = 0;   /* rdtsc() değeri boot anında */
static uint64_t g_tsc_khz  = 0;   /* kalibre edilmiş TSC frekansı kHz */

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi) : : "ecx");
    return ((uint64_t)hi << 32) | lo;
}

/* ---- PIT Kanal 2  —  Gecikme (IRQ gerektirmez, OUT2 polling) ----
 *
 * port 0x61:
 *   bit 0 = Kanal 2 gate (1=etkin)
 *   bit 1 = hoparlör (0=kapalı)
 *   bit 5 = OUT2 çıkışı (1=sayım bitti)
 *
 * Yaklaşık: count = istenen_us * 1193 / 1000
 */
static void pit_ch2_oneshot(uint16_t count)
{
    /* Gate açık, hoparlör kapalı */
    uint8_t p61 = (uint8_t)((inb(0x61) | 0x01u) & ~0x02u);
    outb(0x61, p61);

    outb(0x43, 0xB0u);                      /* Kanal 2, lobyte/hibyte, mod 0, ikili */
    outb(0x42, (uint8_t)(count & 0xFFu));   /* düşük bayt */
    outb(0x42, (uint8_t)(count >> 8));      /* yüksek bayt */

    /* OUT2 (bit5) 0→1 olana dek bekle */
    while (!(inb(0x61) & 0x20u))
        ;
}

/* n milisaniye gecikme (~1 ms = 1193 PIT sayımı @ 1193182 Hz) */
void ms_delay(uint32_t ms)
{
    while (ms--)
        pit_ch2_oneshot(1193u);
}

/* ---- PIT Kanal 0  —  Sistem Timer Kurulumu (scheduler temeli) ----
 *
 * Bölücü = 1193182 / 1000 = 1193  →  ~1 kHz = 1 ms/tick
 * IRQ0 (bkz. IDT + PIC init ile genişletilebilir)
 * ---------------------------------------------------------------- */
static void pit_init(void)
{
    uint16_t divisor = (uint16_t)(PIT_FREQ_HZ / PIT_TICK_HZ);
    outb(0x43, 0x36u);                      /* Kanal 0, lobyte/hibyte, mod 3, ikili */
    outb(0x40, (uint8_t)(divisor & 0xFFu)); /* düşük bayt */
    outb(0x40, (uint8_t)(divisor >> 8));    /* yüksek bayt */
}

/* ---- TSC Kalibrasyonu  —  PIT ile 10 ms ölçüm ---- */
static void tsc_calibrate(void)
{
    uint64_t t0 = rdtsc();
    ms_delay(10u);
    uint64_t t1 = rdtsc();
    uint64_t diff = t1 - t0;
    /* diff cycles / 10 ms = kHz */
    g_tsc_khz = (diff > 0u) ? (diff / 10u) : 1u;
}

/* Boot'tan bu yana geçen süreyi ms olarak döndür */
static uint64_t uptime_ms(void)
{
    if (g_tsc_khz == 0u) return 0u;
    return (rdtsc() - g_boot_tsc) / g_tsc_khz;
}

/* Uptime'ı "Xg Xsa Xdk Xsn.XXX" formatında seri porta yaz */
static void serial_print_uptime(uint64_t ms)
{
    uint64_t total_s = ms / 1000u;
    uint64_t frac    = ms % 1000u;
    uint64_t sn  = total_s % 60u;
    uint64_t dk  = (total_s / 60u) % 60u;
    uint64_t sa  = (total_s / 3600u) % 24u;
    uint64_t gun = total_s / 86400u;

    char buf[24];
    if (gun > 0u) {
        int_to_dec(gun, buf); serial_puts(buf); serial_puts("g ");
    }
    serial_put2d((uint8_t)sa);  serial_putc(':');
    serial_put2d((uint8_t)dk);  serial_putc(':');
    serial_put2d((uint8_t)sn);  serial_putc('.');
    serial_putc('0' + (char)(frac / 100u));
    serial_putc('0' + (char)((frac / 10u) % 10u));
    serial_putc('0' + (char)(frac % 10u));
}

/* ================================================================
 * VGA Metin Modu Sürücüsü  (0xB8000)
 *
 * Her hücre 2 bayt: [karakter][renk-niteliği]
 * Renk niteliği: [arka-plan:4bit][ön-plan:4bit]
 * ================================================================ */
#define VGA_TEXT_BASE   0x000B8000UL
#define VGA_COLS        80
#define VGA_ROWS        25

/* VGA renk kodları */
#define VC_BLACK      0
#define VC_BLUE       1
#define VC_GREEN      2
#define VC_CYAN       3
#define VC_RED        4
#define VC_MAGENTA    5
#define VC_BROWN      6
#define VC_LGRAY      7
#define VC_DGRAY      8
#define VC_LBLUE      9
#define VC_LGREEN     10
#define VC_LCYAN      11
#define VC_LRED       12
#define VC_LMAGENTA   13
#define VC_YELLOW     14
#define VC_WHITE      15

#define VGA_ATTR(fg, bg)  ((uint8_t)(((bg) << 4) | (fg)))

static inline volatile uint16_t *vga_fb(void)
{
    return (volatile uint16_t *)VGA_TEXT_BASE;
}

static void vga_clear(uint8_t attr)
{
    volatile uint16_t *fb = vga_fb();
    uint16_t blank = (uint16_t)((uint16_t)attr << 8) | ' ';
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        fb[i] = blank;
}

static void vga_putc(int col, int row, char c, uint8_t attr)
{
    if ((unsigned)col >= VGA_COLS || (unsigned)row >= VGA_ROWS) return;
    vga_fb()[row * VGA_COLS + col] = (uint16_t)((uint16_t)attr << 8) | (uint8_t)c;
}

static void vga_fill(int col, int row, int len, char c, uint8_t attr)
{
    for (int i = 0; i < len; i++)
        vga_putc(col + i, row, c, attr);
}

/* String yaz; '\n' ile satır başına döner.
 * out_col / out_row: sonraki yazma konumu (NULL iletebilirsin). */
static void vga_puts(int col, int row, const char *s, uint8_t attr,
                     int *out_col, int *out_row)
{
    int c = col, r = row;
    while (*s) {
        if (*s == '\n') {
            c = col; r++;
        } else {
            if (c >= VGA_COLS) { c = 0; r++; }
            if (r >= VGA_ROWS) r = 0;
            vga_putc(c, r, *s, attr);
            c++;
        }
        s++;
    }
    if (out_col) *out_col = c;
    if (out_row) *out_row = r;
}

/* Sayıyı onaltılık olarak VGA'ya yaz (16 basamak) */
static void vga_puthex64(int col, int row, uint64_t val, uint8_t attr)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex[(val >> ((15 - i) * 4)) & 0xF];
    buf[18] = '\0';
    vga_puts(col, row, buf, attr, 0, 0);
}

/* ================================================================
 * VGA Boot Ekranı
 * ================================================================ */

static void draw_header(void)
{
    uint8_t hdr = VGA_ATTR(VC_YELLOW, VC_BLUE);
    vga_fill(0, 0, VGA_COLS, ' ', hdr);
    vga_puts(2, 0,
             "FIRMAWORK v0.0003  |  x86-64 Long Mode  |  COM1:115200",
             hdr, 0, 0);
}

static void draw_logo(void)
{
    uint8_t lo = VGA_ATTR(VC_YELLOW, VC_BLACK);
    uint8_t bo = VGA_ATTR(VC_WHITE,  VC_BLACK);

    vga_fill(0, 1, VGA_COLS, '-', VGA_ATTR(VC_DGRAY, VC_BLACK));

    vga_puts(4, 2,  " _____ ___ ____  __  __   _    _____  ___  ____  _  __", lo, 0, 0);
    vga_puts(4, 3,  "|  ___|_ _|  _ \\|  \\/  | / \\  |_   _|/ _ \\|  _ \\| |/ /", lo, 0, 0);
    vga_puts(4, 4,  "| |_   | || |_) | |\\/| |/ _ \\   | | | | | | |_) | ' / ", lo, 0, 0);
    vga_puts(4, 5,  "|  _|  | ||  _ <| |  | / ___ \\  | | | |_| |  _ <| . \\ ", lo, 0, 0);
    vga_puts(4, 6,  "|_|   |___|_| \\_\\_|  |_/_/   \\_\\ |_|  \\___/|_| \\_\\_|\\_\\", lo, 0, 0);

    vga_puts(4, 7,  "  Bare-Metal x86-64 Firmware  |  Kendi fontumuz: font.h  |  VGA:0xB8000",
             bo, 0, 0);

    vga_fill(0, 8, VGA_COLS, '-', VGA_ATTR(VC_DGRAY, VC_BLACK));
}

static void draw_sysinfo(void)
{
    uint8_t gr = VGA_ATTR(VC_LGREEN,  VC_BLACK);
    uint8_t cy = VGA_ATTR(VC_LCYAN,   VC_BLACK);
    uint8_t wh = VGA_ATTR(VC_WHITE,   VC_BLACK);
    uint8_t yw = VGA_ATTR(VC_YELLOW,  VC_BLACK);

    vga_puts(2,  9,  "Mimari    :", yw, 0, 0);
    vga_puts(14, 9,  "x86-64 Long Mode (64-bit)", gr, 0, 0);

    vga_puts(2,  10, "Bellenim  :", yw, 0, 0);
    vga_puts(14, 10, "FIRMAWORK v0.0003", gr, 0, 0);

    vga_puts(2,  11, "Seri Port :", yw, 0, 0);
    vga_puts(14, 11, "COM1  0x3F8  115200-8N1  (x86 IN/OUT)", gr, 0, 0);

    vga_puts(2,  12, "Ekran     :", yw, 0, 0);
    vga_puts(14, 12, "VGA Metin Modu  0xB8000  80x25  CP437", gr, 0, 0);

    vga_puts(2,  13, "Sayfalama :", yw, 0, 0);
    vga_puts(14, 13, "4-seviyeli (PML4)  1 GB devasa sayfalar  kimlik esleme", gr, 0, 0);

    vga_fill(0, 14, VGA_COLS, '-', VGA_ATTR(VC_DGRAY, VC_BLACK));

    vga_puts(2,  15, "Firmware Servis Tablosu (FST) @ 0x80100000:", cy, 0, 0);
    vga_puts(4,  16, "FST[0]  serial_puts (const char *)", wh, 0, 0);
    vga_puts(4,  17, "FST[1]  serial_putc (char)", wh, 0, 0);
    vga_puts(4,  18, "FST[2]  serial_getc () -> char", wh, 0, 0);
    vga_puts(4,  19, "FST[3]  serial_puthex (uint64_t)", wh, 0, 0);

    vga_fill(0, 20, VGA_COLS, '-', VGA_ATTR(VC_DGRAY, VC_BLACK));

    vga_puts(2,  21, "Turkce font (font.h, 8x16 bitmap):", yw, 0, 0);
    vga_puts(2,  22, "  s+cedilla  S+cedilla  g+breve  G+breve",
             VGA_ATTR(VC_LMAGENTA, VC_BLACK), 0, 0);
    vga_puts(2,  23, "  dotless-i  dotted-I   o-umlaut  O-umlaut  u-umlaut  U-umlaut",
             VGA_ATTR(VC_LMAGENTA, VC_BLACK), 0, 0);
}

static void draw_footer(void)
{
    uint8_t ft = VGA_ATTR(VC_YELLOW, VC_BLUE);
    vga_fill(0, 24, VGA_COLS, ' ', ft);
    vga_puts(2, 24, "COM1: [1] Sistem Bilgisi   [2] Echo Modu   Cikis: Ctrl+A x",
             ft, 0, 0);
}

static void vga_boot_screen(void)
{
    vga_clear(VGA_ATTR(VC_LGRAY, VC_BLACK));
    draw_header();
    draw_logo();
    draw_sysinfo();
    draw_footer();
}

/* ================================================================
 * Mimari Blob Sembolleri  (blob_shims.s / linker tarafından tanımlanır)
 * ================================================================ */
extern char rv32_blob_start[], rv32_blob_end[];
extern char a64_blob_start[],  a64_blob_end[];

/* RV32 blob boyutunu bayt olarak döndür */
static uint64_t rv32_blob_size(void)
{
    return (uint64_t)(rv32_blob_end - rv32_blob_start);
}

/* A64 blob boyutunu bayt olarak döndür */
static uint64_t a64_blob_size(void)
{
    return (uint64_t)(a64_blob_end - a64_blob_start);
}

/* İlk N baytı seri porta hex olarak yaz */
static void dump_blob_head(const char *p, uint64_t sz)
{
    static const char h[] = "0123456789ABCDEF";
    uint64_t n = sz < 8 ? sz : 8;
    uint64_t i;
    for (i = 0; i < n; i++) {
        serial_putc(h[(p[i] >> 4) & 0xF]);
        serial_putc(h[ p[i]       & 0xF]);
        serial_putc(' ');
    }
    serial_puts("\n");
}

/* ================================================================
 * Mimari Blob Çağırıcıları  (FST[4] ve FST[5])
 *
 * meclpp'den "invoke:riscv32" / "invoke:aarch64" komutu bu
 * fonksiyonları çağırır.  x86-64 CPU doğrudan RV32/A64 kodu
 * çalıştıramaz; bu fonksiyonlar blob bilgisi verir ve gelecekteki
 * yorumlayıcı/hipervizör katmanı için yer tutar.
 * ================================================================ */
void invoke_rv32(void)
{
    uint64_t addr = (uint64_t)(uintptr_t)rv32_blob_start;
    uint64_t sz   = rv32_blob_size();
    serial_puts("\n[INVOKE] RISC-V 32 blogu\n");
    serial_puts("  Adres : ");  serial_puthex(addr);
    serial_puts("  Boyut : ");  serial_puthex(sz);
    serial_puts("  ilk 8B: "); dump_blob_head(rv32_blob_start, sz);
    serial_puts("  [LOAD ADDR: 0x80000000 | NS16550 @ 0x10000000]\n");
    serial_puts("  Blogu calıstırmak icin: --arch=riscv32 ile ./calistir.sh\n");
}

void invoke_a64(void)
{
    uint64_t addr = (uint64_t)(uintptr_t)a64_blob_start;
    uint64_t sz   = a64_blob_size();
    serial_puts("\n[INVOKE] AArch64 blogu\n");
    serial_puts("  Adres : ");  serial_puthex(addr);
    serial_puts("  Boyut : ");  serial_puthex(sz);
    serial_puts("  ilk 8B: "); dump_blob_head(a64_blob_start, sz);
    serial_puts("  [LOAD ADDR: 0x40000000 | PL011 @ 0x09000000]\n");
    serial_puts("  Blogu calıstırmak icin: --arch=aarch64 ile ./calistir.sh\n");
}

/* ================================================================
 * .fiawo Segment Adresleri
 *
 * QEMU ile yükleme:
 *   -device loader,file=kod.fiawo,addr=0x80200000,force-raw=on
 *
 * Adres seçimi:
 *   0x80200000 → Segment 0  (birincil .fiawo giriş noktası)
 *   0x80300000 → Segment 1  (visit:main.meclpp hedefi)
 *
 * Not: Her iki adres de PML4 kimlik eşlemesinin içindedir
 *      (PDPT[2]: 0x80000000–0xBFFFFFFF).
 * ================================================================ */
#define FIAWO_SEG0  0x80200000UL   /* birincil segment */
#define FIAWO_SEG1  0x80300000UL   /* visit hedefi     */

/* İlk birkaç baytı oku, sıfır olmayan içerik var mı diye bak.
 * Yüklenmiş .fiawo: 0x48 (REX.W öneki) ile başlar.
 * Yüklenmemiş bellek: genellikle 0x00 veya 0xFF olur. */
static int fiawo_probe(uint64_t addr)
{
    volatile uint8_t *p = (volatile uint8_t *)addr;
    /* İlk 8 baytın hepsinin 0x00 ya da 0xFF olup olmadığını kontrol et */
    uint8_t first = p[0];
    if (first == 0x00 || first == 0xFF) return 0;
    return 1;  /* büyük olasılıkla yüklü */
}

/* .fiawo segmentini bilgi olarak yazdır */
static void fiawo_info(uint64_t addr, const char *label)
{
    uint8_t yw  = VGA_ATTR(VC_YELLOW, VC_BLACK);
    uint8_t gn  = VGA_ATTR(VC_LGREEN, VC_BLACK);
    uint8_t rd  = VGA_ATTR(VC_LRED,   VC_BLACK);

    serial_puts("  ");
    serial_puts(label);
    serial_puts(" @ ");
    serial_puthex(addr);
    if (fiawo_probe(addr)) {
        serial_puts("      durum: YUKLU  (ilk bayt=");
        serial_puthex(*(volatile uint8_t *)addr);
        serial_puts(")\n");
        (void)gn;
    } else {
        serial_puts("      durum: YOK / BOSH\n");
        (void)rd;
    }
    (void)yw;
}

/* .fiawo'ya mutlak atlama — inline asm ile temiz bir JMP.
 * Bu fonksiyon asla geri dönmez; .fiawo FREEZE (EB FE) ile biter. */
static void __attribute__((noreturn)) fiawo_jump(uint64_t addr)
{
    __asm__ volatile (
        "movq %0, %%rax\n\t"
        "jmp  *%%rax"
        :
        : "r"(addr)
        : "rax"
    );
    __builtin_unreachable();
}

/* ================================================================
 * Firmware Servis Tablosu (FST)
 * 0x80100000 adresine 64-bit işaretçiler yazılır.
 * ================================================================ */
#define FST_BASE  0x80100000UL

static void fst_init(void)
{
    volatile uint64_t *fst = (volatile uint64_t *)FST_BASE;
    fst[0] = (uint64_t)(uintptr_t)serial_puts;
    fst[1] = (uint64_t)(uintptr_t)serial_putc;
    fst[2] = (uint64_t)(uintptr_t)serial_getc;
    fst[3] = (uint64_t)(uintptr_t)serial_puthex;
    fst[4] = (uint64_t)(uintptr_t)invoke_rv32;
    fst[5] = (uint64_t)(uintptr_t)invoke_a64;
}

/* ================================================================
 * Multiboot Bilgi Yapısı (Multiboot 1, ilgili alanlar)
 * ================================================================ */
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} MultibootInfo;

/* ================================================================
 * UART Menü Komutları
 * ================================================================ */
static void print_menu(void)
{
    serial_puts("\n========================================\n");
    serial_puts("   FIRMAWORK x86-64  --  Ana Menu\n");
    serial_puts("========================================\n");
    serial_puts("  [1] Sistem Bilgisi\n");
    serial_puts("  [2] Klavye Echo Modu  (Cikis: Ctrl+])\n");
    serial_puts("  [3] .fiawo Calistir  @ 0x80200000\n");
    serial_puts("  [4] .fiawo Durum     (segment kontrol)\n");
    serial_puts("  [5] Arch Bloblari    (RV32 + A64 bilgi)\n");
    serial_puts("  [6] Saat / RTC       (CMOS tarih+saat)\n");
    serial_puts("  [7] Kapat            (ACPI S5 shutdown)\n");
    serial_puts("========================================\n");
    serial_puts("Seciminiz: ");
}

/* ---- .fiawo komutları ---- */

static void cmd_fiawo_run(void)
{
    serial_puts("\n--- .fiawo Segment Kontrol ---\n");
    fiawo_info(FIAWO_SEG0, "Seg0 @ 0x80200000");
    fiawo_info(FIAWO_SEG1, "Seg1 @ 0x80300000");

    if (!fiawo_probe(FIAWO_SEG0)) {
        serial_puts("\n[HATA] Segment 0 bos veya yuklenmemis!\n");
        serial_puts("QEMU'yu su parametreyle calistirin:\n");
        serial_puts("  FIAWO=kod.fiawo ./calistir.sh\n");
        serial_puts("ya da dogrudan:\n");
        serial_puts("  -device loader,file=kod.fiawo,"
                    "addr=0x80200000,force-raw=on\n");
        return;
    }

    serial_puts("\n[OK] Segment 0 yuklu. Kontrolu .fiawo'ya devrediyorum...\n");
    serial_puts("(Geri donus yok; .fiawo FREEZE ile sonlanir.)\n");
    serial_puts("------------------------------------------\n");

    /* Seri port tamponunun bitmesini bekle */
    for (volatile int i = 0; i < 200000; i++) {}

    fiawo_jump(FIAWO_SEG0);
    /* NOTREACHED */
}

static void cmd_fiawo_info(void)
{
    serial_puts("\n--- .fiawo Segment Durumlari ---\n");
    fiawo_info(FIAWO_SEG0, "Seg0 (birincil, giris)");
    fiawo_info(FIAWO_SEG1, "Seg1 (visit hedefi)   ");
    serial_puts("\nSegment yuklemek icin:\n");
    serial_puts("  FIAWO=kod.fiawo ./calistir.sh\n");
    serial_puts("\nDerlemek icin:\n");
    serial_puts("  gcc derleyici.c -o derleyici\n");
    serial_puts("  ./derleyici kod.meclpp  ->  kod.fiawo\n");
}

static void int_to_dec(uint64_t val, char *buf)
{
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[21];
    int i = 0;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

/* 2 basamaklı ondalık yaz (seri porta), örn: 07, 23 */
static void serial_put2d(uint8_t v)
{
    serial_putc('0' + (v / 10));
    serial_putc('0' + (v % 10));
}

/* ================================================================
 * CMOS RTC Sürücüsü  (x86 I/O portları 0x70 / 0x71)
 *
 * 0x70 : CMOS adres (bit7=NMI devre dışı)
 * 0x71 : CMOS veri
 * Değerler BCD formatındadır (Status Reg B bit2=0 varsayımı).
 * ================================================================ */
static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, (uint8_t)(reg & 0x7F));  /* NMI mask'siz seç */
    /* küçük gecikme — gerçek donanımda gerekli */
    inb(0x80);
    return inb(0x71);
}

static uint8_t bcd2bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) & 0x0F) * 10u + (bcd & 0x0F));
}

static void cmd_rtc(void)
{
    /* Güncelleme döngüsü bitmeden okuma yaparsan yanlış değer alırsın */
    while (cmos_read(0x0A) & 0x80)   /* Status Reg A bit7 = UIP */
        ;

    uint8_t sn  = bcd2bin(cmos_read(0x00));   /* saniye */
    uint8_t dk  = bcd2bin(cmos_read(0x02));   /* dakika */
    uint8_t sa  = bcd2bin(cmos_read(0x04));   /* saat   */
    uint8_t gun = bcd2bin(cmos_read(0x07));   /* gün    */
    uint8_t ay  = bcd2bin(cmos_read(0x08));   /* ay     */
    uint8_t yil = bcd2bin(cmos_read(0x09));   /* yıl (son 2 hane) */

    serial_puts("\n--- CMOS RTC + Uptime ---\n");

    serial_puts("Saat   : ");
    serial_put2d(sa);  serial_putc(':');
    serial_put2d(dk);  serial_putc(':');
    serial_put2d(sn);  serial_puts("\n");

    serial_puts("Tarih  : 20");
    serial_put2d(yil); serial_putc('-');
    serial_put2d(ay);  serial_putc('-');
    serial_put2d(gun); serial_puts("\n");

    serial_puts("Uptime : ");
    serial_print_uptime(uptime_ms());
    serial_puts("\n");

    serial_puts("TSC    : ");
    char buf[24];
    int_to_dec(g_tsc_khz / 1000u, buf);
    serial_puts(buf);
    serial_puts(" MHz  (PIT ile kalibre)\n");
    serial_puts("(CMOS RTC BCD okuma — QEMU saatini yansıtır)\n");
}

/* ================================================================
 * ACPI Kapatma  (QEMU PM1a_CNT — S5 Uyku Durumu)
 *
 * QEMU PIIX4 ACPI: PM_IO_BASE=0x600, PM1a_CNT=+0x04 → port 0x604
 * SLP_EN (bit13) | SLP_TYP_S5 (QEMU'da genellikle 0) = 0x2000
 * ================================================================ */
static void __attribute__((noreturn)) cmd_shutdown(void)
{
    serial_puts("\n[KAPAT] ACPI S5 durumuna geciliyor...\n");
    /* UART tamponunu boşalt */
    for (volatile int i = 0; i < 500000; i++) {}

    outw(0x604, 0x2000);   /* QEMU PIIX4 ACPI PM1a_CNT — S5 shutdown */
    outw(0xB004, 0x2000);  /* eski QEMU SeaBIOS uyumu (yedek)         */

    /* Buraya gelirse CPU'yu durdur */
    serial_puts("[HALT] ACPI yanit vermedi, CPU durduruluyor.\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* Blob boyutu <= 64 bayt ise gen_blobs.c stub'u → "henüz boş" */
#define BLOB_STUB_THRESHOLD  64u

static void cmd_arch_blobs(void)
{
    uint64_t rv_sz = rv32_blob_size();
    uint64_t a6_sz = a64_blob_size();

    serial_puts("\n--- Gomulu Mimari Bloblari ---\n");

    /* ---- RISC-V 32 ---- */
    serial_puts("  RV32  @ ");
    serial_puthex((uint64_t)(uintptr_t)rv32_blob_start);
    serial_puts("  boyut=");
    serial_puthex(rv_sz);
    if (rv_sz <= BLOB_STUB_THRESHOLD) {
        serial_puts("  [HENUZ BOS — sadece gen_blobs.c stub]\n");
        serial_puts("    Gercek blob icin: sh calistir.sh --arch=riscv32\n");
    } else {
        serial_puts("  ilk 8B: ");
        dump_blob_head(rv32_blob_start, rv_sz);
    }

    /* ---- AArch64 ---- */
    serial_puts("  A64   @ ");
    serial_puthex((uint64_t)(uintptr_t)a64_blob_start);
    serial_puts("  boyut=");
    serial_puthex(a6_sz);
    if (a6_sz <= BLOB_STUB_THRESHOLD) {
        serial_puts("  [HENUZ BOS — sadece gen_blobs.c stub]\n");
        serial_puts("    Gercek blob icin: sh calistir.sh --arch=aarch64\n");
    } else {
        serial_puts("  ilk 8B: ");
        dump_blob_head(a64_blob_start, a6_sz);
    }

    serial_puts("\nmeclpp'den cagirmak icin:\n");
    serial_puts("  invoke:riscv32  ->  FST[4] = invoke_rv32()\n");
    serial_puts("  invoke:aarch64  ->  FST[5] = invoke_a64()\n");
    serial_puts("\nAyri QEMU oturumunda calıstırmak icin:\n");
    serial_puts("  sh calistir.sh --arch=riscv32\n");
    serial_puts("  sh calistir.sh --arch=aarch64\n");
}

static void cmd_sysinfo(void)
{
    serial_puts("\n--- Sistem Bilgisi (x86-64) ---\n");
    serial_puts("Mimari    : x86-64 Long Mode\n");
    serial_puts("Bellenim  : FIRMAWORK v0.0003\n");
    serial_puts("Seri Port : COM1 (0x3F8)  115200-8N1\n");
    serial_puts("Ekran     : VGA Metin 0xB8000  80x25\n");
    serial_puts("FST Tabanı: 0x80100000\n");
    serial_puts("  FST[0] <- serial_puts\n");
    serial_puts("  FST[1] <- serial_putc\n");
    serial_puts("  FST[2] <- serial_getc\n");
    serial_puts("  FST[3] <- serial_puthex\n");
    serial_puts("  FST[4] <- invoke_rv32  (RV32 blob @ 0x200000)\n");
    serial_puts("  FST[5] <- invoke_a64   (A64 blob @ 0x300000)\n");
    serial_puts("Arch Bloblari:\n");
    serial_puts("  RV32 @ 0x200000  (yukl: 0x80000000 NS16550)\n");
    serial_puts("  A64  @ 0x300000  (yukl: 0x40000000 PL011)\n");
}

static void cmd_echo(void)
{
    serial_puts("\n[Echo Modu baslatildi -- Ctrl+] ile cikin]\n");
    for (;;) {
        char c = serial_getc();
        if (c == 0x1D) break;
        serial_putc(c);
        if (c == '\r') serial_putc('\n');
    }
    serial_puts("\n[Echo Modu sonlandi]\n");
}

/* ================================================================
 * Firmware Giriş Noktası  (entry.s'ten çağrılır)
 *   mbi   : Multiboot bilgi yapısı işaretçisi
 *   magic : Multiboot sihirli sayısı (0x2BADB002)
 * ================================================================ */
int main(void *mbi, uint64_t magic)
{
    /* COM1 seri portunu başlat */
    serial_init();

    /* PIT kanal 0'ı 1000 Hz'e kur (scheduler temeli) */
    pit_init();

    /* Boot TSC'sini kaydet, ardından PIT ile kalibre et (~10 ms) */
    g_boot_tsc = rdtsc();
    tsc_calibrate();

    /* VGA boot ekranını çiz */
    vga_boot_screen();

    /* FST'yi kur */
    fst_init();

    /* Hoş geldin mesajı */
    serial_puts("\n>>> FIRMAWORK x86-64 LONG MODE CALISIYOR <<<\n");
    serial_puts("COM1: 115200-8N1  |  VGA: 0xB8000 80x25\n");
    serial_puts("Sayfalama: PML4 kimlik esleme  ilk 4 GB\n");
    serial_puts("FST hazir @ 0x80100000\n");

    /* Boot modunu doğrula ve bilgi yaz */
    if (magic == 0x2BADB002UL) {
        /* ---- Multiboot / GRUB modu ---- */
        serial_puts("Boot : Multiboot (GRUB/QEMU -kernel)\n");
        if (mbi) {
            MultibootInfo *mb = (MultibootInfo *)mbi;
            if (mb->flags & (1 << 0)) {
                char buf[24];
                serial_puts("Bellek: Alt=");
                int_to_dec(mb->mem_lower, buf);
                serial_puts(buf);
                serial_puts(" KB  Ust=");
                int_to_dec(mb->mem_upper, buf);
                serial_puts(buf);
                serial_puts(" KB\n");
            }
        }
    } else if (magic == 0xF12AB007UL) {
        /* ---- FIRMAWORK Özel Bootloader modu (Stage 2) ---- */
        serial_puts("Boot : FIRMAWORK Ozel Bootloader  (Stage1->Stage2->Kernel)\n");
        serial_puts("       EAX=0xF12AB007  (FIRMAB00T)\n");
        serial_puts("       Disk: LBA0=S1 LBA1-16=S2 LBA17+=firmware.bin\n");
    } else {
        serial_puts("Boot : Bilinmeyen mod (magic=");
        serial_puthex((unsigned long)magic);
        serial_puts(")\n");
    }

    /* Ana menü döngüsü */
    for (;;) {
        print_menu();
        char choice = serial_getc();
        serial_putc(choice);
        serial_puts("\n");
        switch (choice) {
            case '1': cmd_sysinfo();    break;
            case '2': cmd_echo();       break;
            case '3': cmd_fiawo_run();  break;
            case '4': cmd_fiawo_info(); break;
            case '5': cmd_arch_blobs(); break;
            case '6': cmd_rtc();        break;
            case '7': cmd_shutdown();   break;  /* noreturn */
            default:
                serial_puts("Gecersiz secim.\n");
                break;
        }
    }

    return 0;
}
