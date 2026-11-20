/* MyOS: a tiny x86 hobby kernel.
 *   - VGA text console (scroll region) mirrored to the COM1 serial port
 *   - polled PS/2 keyboard (arrows, F-keys, Caps Lock) and PIT timing
 *   - text-mode tabbed UI: Shell / Info / Settings / Games
 *   - boot splash, command history, snake
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VGA_W 80
#define VGA_H 25
#define VGA_MEM ((volatile uint16_t *)0xB8000)

/* Rows 1..23 are the content area; row 0 = tab bar, row 24 = status bar. */
#define CON_TOP 1
#define CON_BOT 23

#define OS_NAME "MyOS"
#define OS_VERSION "0.5"

static size_t cur_x, cur_y;
static uint8_t color = 0x0F;       /* shell text attribute: white on black */
static bool serial_on = true;      /* mirror console output to COM1 */

/* ---------- port I/O ---------- */
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ---------- serial port (COM1) ---------- */
#define COM1 0x3F8

static void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x80);   /* enable divisor latch */
    outb(COM1 + 0, 0x03);   /* divisor 3 = 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xC7);   /* enable and clear FIFO */
    outb(COM1 + 4, 0x0B);   /* DTR + RTS + OUT2 */
}

static void serial_raw(char c) {
    while (!(inb(COM1 + 5) & 0x20)) {}         /* wait for transmit empty */
    outb(COM1, (uint8_t)c);
}

static void serial_write(char c) {
    if (c == '\n') { serial_raw('\r'); serial_raw('\n'); }
    else if (c == '\b') { serial_raw('\b'); serial_raw(' '); serial_raw('\b'); }
    else serial_raw(c);
}

/* ---------- VGA console (scrolls inside rows CON_TOP..CON_BOT) ---------- */
static void update_cursor(void) {
    uint16_t pos = (uint16_t)(cur_y * VGA_W + cur_x);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}

static void cursor_hide(void) { outb(0x3D4, 0x0A); outb(0x3D5, 0x20); }
static void cursor_show(void) {
    outb(0x3D4, 0x0A); outb(0x3D5, 0x0E);
    outb(0x3D4, 0x0B); outb(0x3D5, 0x0F);
}

/* Clears only the content area and homes the cursor there. */
static void clear_screen(void) {
    for (size_t i = CON_TOP * VGA_W; i < (CON_BOT + 1) * VGA_W; i++)
        VGA_MEM[i] = (uint16_t)' ' | (uint16_t)color << 8;
    cur_x = 0; cur_y = CON_TOP;
    update_cursor();
}

static void scroll(void) {
    for (size_t i = CON_TOP * VGA_W; i < CON_BOT * VGA_W; i++)
        VGA_MEM[i] = VGA_MEM[i + VGA_W];
    for (size_t i = 0; i < VGA_W; i++)
        VGA_MEM[CON_BOT * VGA_W + i] = (uint16_t)' ' | (uint16_t)color << 8;
    cur_y = CON_BOT;
}

static void putc(char c) {
    if (serial_on) serial_write(c);
    if (c == '\n') {
        cur_x = 0; cur_y++;
    } else if (c == '\b') {
        bool moved = false;
        if (cur_x > 0) { cur_x--; moved = true; }
        else if (cur_y > CON_TOP) { cur_y--; cur_x = VGA_W - 1; moved = true; }
        if (moved)
            VGA_MEM[cur_y * VGA_W + cur_x] = (uint16_t)' ' | (uint16_t)color << 8;
    } else {
        VGA_MEM[cur_y * VGA_W + cur_x] = (uint16_t)(uint8_t)c | (uint16_t)color << 8;
        if (++cur_x == VGA_W) { cur_x = 0; cur_y++; }
    }
    if (cur_y > CON_BOT) scroll();
    update_cursor();
}

static void puts(const char *s) { while (*s) putc(*s++); }

static void put_dec(uint32_t n) {
    char buf[11]; int i = 0;
    if (n == 0) { putc('0'); return; }
    while (n) { buf[i++] = (char)('0' + n % 10); n /= 10; }
    while (i) putc(buf[--i]);
}

static void put_int(int32_t n) {
    if (n < 0) { putc('-'); put_dec(0u - (uint32_t)n); }
    else put_dec((uint32_t)n);
}

static void put_hex(uint32_t n) {
    bool started = false;
    puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uint8_t d = (uint8_t)((n >> i) & 0xF);
        if (d || started || i == 0) {
            putc((char)(d < 10 ? '0' + d : 'A' + d - 10));
            started = true;
        }
    }
}

static void put_dec2(uint32_t n) {            /* always two digits */
    putc((char)('0' + (n / 10) % 10));
    putc((char)('0' + n % 10));
}

/* ---------- string helpers (no libc here) ---------- */
static int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int starts_with(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}
static size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static void strcopy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void catstr(char *dst, const char *src, size_t max) {
    size_t n = strlen(dst);
    while (*src && n + 1 < max) dst[n++] = *src++;
    dst[n] = 0;
}
static const char *skip_spaces(const char *s) {
    while (*s == ' ') s++;
    return s;
}

/* Parses "123", "-5" or "0xFF". Advances *pp. Returns 1 on success. */
static int parse_num(const char **pp, int32_t *out) {
    const char *p = skip_spaces(*pp);
    bool neg = false;
    uint32_t v = 0;
    int digits = 0;
    if (*p == '-') { neg = true; p++; }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        for (;; p++, digits++) {
            char c = *p; uint32_t d;
            if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else break;
            v = v * 16 + d;
        }
    } else {
        for (; *p >= '0' && *p <= '9'; p++, digits++)
            v = v * 10 + (uint32_t)(*p - '0');
    }
    if (!digits) return 0;
    *out = neg ? (int32_t)(0u - v) : (int32_t)v;
    *pp = p;
    return 1;
}

/* ---------- direct VGA drawing (for UI pages and games) ---------- */
static void vga_put(int x, int y, char ch, uint8_t attr) {
    VGA_MEM[y * VGA_W + x] = (uint16_t)(uint8_t)ch | (uint16_t)attr << 8;
}
static void vga_text(int x, int y, const char *s, uint8_t attr) {
    while (*s && x < VGA_W) vga_put(x++, y, *s++, attr);
}
static void vga_clear_area(int x, int y, int w, int h, uint8_t attr) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) vga_put(x + i, y + j, ' ', attr);
}
/* Draws a number; returns the x just after the last digit. */
static int vga_num(int x, int y, uint32_t n, uint8_t attr) {
    char buf[11]; int i = 0;
    if (n == 0) buf[i++] = '0';
    while (n) { buf[i++] = (char)('0' + n % 10); n /= 10; }
    while (i) vga_put(x++, y, buf[--i], attr);
    return x;
}
static void vga_num2(int x, int y, uint32_t n, uint8_t attr) {
    vga_put(x, y, (char)('0' + (n / 10) % 10), attr);
    vga_put(x + 1, y, (char)('0' + n % 10), attr);
}
static void vga_hms(int x, int y, uint32_t h, uint32_t m, uint32_t s, uint8_t attr) {
    vga_num2(x, y, h, attr);     vga_put(x + 2, y, ':', attr);
    vga_num2(x + 3, y, m, attr); vga_put(x + 5, y, ':', attr);
    vga_num2(x + 6, y, s, attr);
}
static void vga_center(int y, const char *s, uint8_t attr) {
    vga_text((VGA_W - (int)strlen(s)) / 2, y, s, attr);
}

/* ---------- PIT timer (channel 0, polled; no interrupts needed) ---------- */
#define PIT_HZ 1193182u

static uint32_t uptime_sec;                    /* seconds since boot */
static uint32_t tick_rem;                      /* PIT ticks not yet a second */

static void pit_init(void) {
    outb(0x43, 0x34);       /* channel 0, lo/hi byte, mode 2, binary */
    outb(0x40, 0x00);       /* reload value 0 = 65536 */
    outb(0x40, 0x00);
}

static uint16_t pit_read(void) {
    outb(0x43, 0x00);       /* latch current count */
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)(lo | (hi << 8));
}

/* Ticks since the last call with this *prev (counter counts down).
 * Also advances the global uptime. Call at least every ~50 ms. */
static uint32_t pit_elapsed(uint16_t *prev) {
    uint16_t cur = pit_read();
    uint16_t d = (uint16_t)(*prev - cur);
    *prev = cur;
    tick_rem += d;
    while (tick_rem >= PIT_HZ) { tick_rem -= PIT_HZ; uptime_sec++; }
    return d;
}

static void sleep_ms(uint32_t ms) {
    uint32_t need = ms * 1193u, acc = 0;
    uint16_t prev = pit_read();
    while (acc < need) acc += pit_elapsed(&prev);
}

/* ---------- keyboard (polled, scancode set 1, US layout) ---------- */
#define KEY_UP    0x100
#define KEY_DOWN  0x101
#define KEY_LEFT  0x102
#define KEY_RIGHT 0x103
#define KEY_F1    0x110     /* F1..F4 = 0x110..0x113 */
#define KEY_F4    0x113

static const char map_lo[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' '
};
static const char map_hi[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' '
};

static bool shift;
static bool caps;
static bool e0;                                /* saw 0xE0 extended prefix */

/* Non-blocking: returns a character, a KEY_* code, or -1 if nothing. */
static int getkey_nb(void) {
    if (!(inb(0x64) & 1)) return -1;
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) { e0 = true; return -1; }
    if (e0) {
        e0 = false;
        if (sc & 0x80) return -1;              /* extended key release */
        switch (sc) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        default:   return -1;
        }
    }

    if (sc == 0x2A || sc == 0x36) { shift = true;  return -1; }
    if (sc == 0xAA || sc == 0xB6) { shift = false; return -1; }
    if (sc == 0x3A) { caps = !caps; return -1; }   /* Caps Lock press */
    if (sc & 0x80) return -1;                      /* key release */
    if (sc >= 0x3B && sc <= 0x3E) return KEY_F1 + (sc - 0x3B);

    char lo = map_lo[sc];
    /* Caps Lock only affects letters; Shift affects everything. */
    bool letter = lo >= 'a' && lo <= 'z';
    bool upper = letter ? (shift != caps) : shift;
    char c = upper ? map_hi[sc] : lo;
    return c ? (int)(unsigned char)c : -1;
}

static int getkey(void) {
    for (;;) {
        int k = getkey_nb();
        if (k >= 0) return k;
        __asm__ volatile("pause");
    }
}

/* ---------- command history ---------- */
#define HIST_N   16
#define LINE_MAX 128

static char hist[HIST_N][LINE_MAX];
static int hist_count;

static void hist_add(const char *line) {
    if (!*line) return;
    if (hist_count > 0 && !strcmp(hist[hist_count - 1], line)) return;
    if (hist_count == HIST_N) {                /* drop the oldest */
        for (int i = 1; i < HIST_N; i++) strcopy(hist[i - 1], hist[i], LINE_MAX);
        hist_count--;
    }
    strcopy(hist[hist_count++], line, LINE_MAX);
}

static void replace_line(char *buf, size_t *n, const char *src) {
    while (*n) { putc('\b'); (*n)--; }
    while (src[*n] && *n + 1 < LINE_MAX) { buf[*n] = src[*n]; putc(src[*n]); (*n)++; }
    buf[*n] = 0;
}

/* ---------- power ---------- */
static void reboot(void) {
    while (inb(0x64) & 2) {}
    outb(0x64, 0xFE);                          /* pulse CPU reset line */
    for (;;) __asm__ volatile("hlt");
}
static void poweroff(void) {
    outw(0x604, 0x2000);                       /* QEMU (q35/newer) */
    outw(0xB004, 0x2000);                      /* older QEMU/Bochs */
    puts("It is now safe to turn off your computer.\n");
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---------- hardware info: RTC and CPUID ---------- */
static uint32_t mem_upper_kb;

struct rtc_time { uint8_t s, m, h, d, mo; uint16_t y; };

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static void rtc_read(struct rtc_time *t) {
    while (cmos_read(0x0A) & 0x80) {}          /* wait out RTC update */
    uint8_t s = cmos_read(0x00), m = cmos_read(0x02), h = cmos_read(0x04);
    uint8_t d = cmos_read(0x07), mo = cmos_read(0x08), y = cmos_read(0x09);
    if (!(cmos_read(0x0B) & 4)) {              /* values are BCD */
        s  = (uint8_t)((s  & 0x0F) + (s  >> 4) * 10);
        m  = (uint8_t)((m  & 0x0F) + (m  >> 4) * 10);
        h  = (uint8_t)((h  & 0x0F) + (h  >> 4) * 10);
        d  = (uint8_t)((d  & 0x0F) + (d  >> 4) * 10);
        mo = (uint8_t)((mo & 0x0F) + (mo >> 4) * 10);
        y  = (uint8_t)((y  & 0x0F) + (y  >> 4) * 10);
    }
    t->s = s; t->m = m; t->h = h; t->d = d; t->mo = mo;
    t->y = (uint16_t)(2000u + y);
}

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                             : "a"(leaf), "c"(0));
}

static void cpu_vendor(char *out) {            /* out must hold 13 bytes */
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    uint32_t regs[3] = { b, d, c };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (char)((regs[i] >> (8 * j)) & 0xFF);
    out[12] = 0;
}

static void cpu_brand(char *out) {             /* out must hold 49 bytes */
    uint32_t a, b, c, d;
    cpuid(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000004u) { strcopy(out, "unknown", 49); return; }
    for (int i = 0; i < 3; i++) {
        cpuid(0x80000002u + (uint32_t)i, &a, &b, &c, &d);
        uint32_t r[4] = { a, b, c, d };
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                out[i * 16 + j * 4 + k] = (char)((r[j] >> (8 * k)) & 0xFF);
    }
    out[48] = 0;
    int s = 0;                                 /* trim leading spaces */
    while (out[s] == ' ') s++;
    if (s) { int i = 0; while (out[s + i]) { out[i] = out[s + i]; i++; } out[i] = 0; }
}

/* ---------- UI state shared with the game and shell ---------- */
enum { TAB_SHELL, TAB_INFO, TAB_SETTINGS, TAB_GAMES, TAB_COUNT };

static int cur_tab = TAB_SHELL;
static uint16_t pit_prev;                      /* main loop's PIT reading */
static uint32_t snake_best;
static bool set_clock = true;                  /* show clock in status bar */
static int  set_speed = 1;                     /* 0 slow, 1 normal, 2 fast */

static uint16_t snap[VGA_W * VGA_H];           /* full-screen snapshot */
static size_t snap_x, snap_y;

static void snap_save(void) {
    for (size_t i = 0; i < VGA_W * VGA_H; i++) snap[i] = VGA_MEM[i];
    snap_x = cur_x; snap_y = cur_y;
}
static void snap_restore(void) {
    for (size_t i = 0; i < VGA_W * VGA_H; i++) VGA_MEM[i] = snap[i];
    cur_x = snap_x; cur_y = snap_y;
    update_cursor();
}
static void ui_apply_cursor(void) {            /* only the Shell shows a cursor */
    if (cur_tab == TAB_SHELL) { cursor_show(); update_cursor(); }
    else cursor_hide();
}

/* ---------- boot splash ---------- */
/* 5x5 block font, A-Z. Anything else is drawn as blank space. */
static const char *const font5[26][5] = {
    {" ### ","#   #","#####","#   #","#   #"},  /* A */
    {"#### ","#   #","#### ","#   #","#### "},  /* B */
    {" ####","#    ","#    ","#    "," ####"},  /* C */
    {"#### ","#   #","#   #","#   #","#### "},  /* D */
    {"#####","#    ","#### ","#    ","#####"},  /* E */
    {"#####","#    ","#### ","#    ","#    "},  /* F */
    {" ####","#    ","#  ##","#   #"," ####"},  /* G */
    {"#   #","#   #","#####","#   #","#   #"},  /* H */
    {"#####","  #  ","  #  ","  #  ","#####"},  /* I */
    {"#####","   # ","   # ","#  # "," ##  "},  /* J */
    {"#   #","#  # ","###  ","#  # ","#   #"},  /* K */
    {"#    ","#    ","#    ","#    ","#####"},  /* L */
    {"#   #","## ##","# # #","#   #","#   #"},  /* M */
    {"#   #","##  #","# # #","#  ##","#   #"},  /* N */
    {" ### ","#   #","#   #","#   #"," ### "},  /* O */
    {"#### ","#   #","#### ","#    ","#    "},  /* P */
    {" ### ","#   #","# # #","#  # "," ## #"},  /* Q */
    {"#### ","#   #","#### ","#  # ","#   #"},  /* R */
    {" ####","#    "," ### ","    #","#### "},  /* S */
    {"#####","  #  ","  #  ","  #  ","  #  "},  /* T */
    {"#   #","#   #","#   #","#   #"," ### "},  /* U */
    {"#   #","#   #","#   #"," # # ","  #  "},  /* V */
    {"#   #","#   #","# # #","## ##","#   #"},  /* W */
    {"#   #"," # # ","  #  "," # # ","#   #"},  /* X */
    {"#   #"," # # ","  #  ","  #  ","  #  "},  /* Y */
    {"#####","   # ","  #  "," #   ","#####"},  /* Z */
};

static void splash(void) {
    static const uint8_t pal[4] = { 0x0B, 0x0A, 0x0E, 0x0D };   /* cyan green yellow pink */
    const char *name = OS_NAME;
    int len = (int)strlen(name);

    for (size_t i = 0; i < VGA_W * VGA_H; i++) VGA_MEM[i] = 0x0720;
    cursor_hide();

    int sc = (len * 6 * 2 <= VGA_W) ? 2 : 1;   /* stretch wide if it fits */
    int total = len * 5 * sc + (len - 1) * sc;
    int x0 = (VGA_W - total) / 2, y0 = 7;

    vga_center(4, "Welcome to", 0x07);
    for (int i = 0; i < len; i++) {
        char ch = name[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (ch < 'A' || ch > 'Z') continue;
        const char *const *g = font5[ch - 'A'];
        int gx = x0 + i * (5 * sc + sc);
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 5; c++)
                if (g[r][c] == '#')
                    for (int k = 0; k < sc; k++)
                        vga_put(gx + c * sc + k, y0 + r, (char)0xDB, pal[i % 4]);
    }
    vga_center(14, "Version " OS_VERSION, 0x0F);

    /* Loading bar that fills over 3 seconds: 40 steps x 75 ms. */
    const int bar = 40, bx = (VGA_W - bar) / 2, by = 18;
    vga_center(17, "Loading...", 0x08);
    for (int i = 0; i < bar; i++) vga_put(bx + i, by, (char)0xB0, 0x08);
    for (int i = 0; i < bar; i++) {
        sleep_ms(75);
        vga_put(bx + i, by, (char)0xDB, 0x0A);
    }
    sleep_ms(150);
}

/* ---------- snake ---------- */
#define SN_X0 1
#define SN_X1 78
#define SN_Y0 2
#define SN_Y1 23
#define SN_MAX ((SN_X1 - SN_X0 + 1) * (SN_Y1 - SN_Y0 + 1))

static uint8_t sx[SN_MAX], sy[SN_MAX];
static uint32_t rng_state = 2463534242u;

static uint32_t rnd(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static uint32_t rdtsc_lo(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return lo;
}

static void snake_food(uint32_t len, int *fx, int *fy) {
    for (;;) {
        int x = SN_X0 + (int)(rnd() % (SN_X1 - SN_X0 + 1));
        int y = SN_Y0 + (int)(rnd() % (SN_Y1 - SN_Y0 + 1));
        bool hit = false;
        for (uint32_t i = 0; i < len; i++)
            if (sx[i] == x && sy[i] == y) { hit = true; break; }
        if (!hit) { *fx = x; *fy = y; return; }
    }
}

static void snake_score(uint32_t score) {
    for (int i = 0; i < 6; i++) vga_put(15 + i, 0, ' ', 0x0F);
    vga_num(15, 0, score, 0x0E);
}

/* Takes over the whole screen. Returns the score; caller restores the UI. */
static uint32_t snake(void) {
    static const uint32_t speed_base[3] = { 180, 140, 100 };   /* ms per step */
    rng_state = rdtsc_lo() | 1u;
    cursor_hide();

    for (size_t i = 0; i < VGA_W * VGA_H; i++) VGA_MEM[i] = 0x0720;
    vga_text(0, 0, "SNAKE   Score:        Arrows/WASD move, Q quits", 0x0F);
    for (int x = 0; x < VGA_W; x++) { vga_put(x, 1, '#', 0x08); vga_put(x, 24, '#', 0x08); }
    for (int y = 1; y < VGA_H; y++) { vga_put(0, y, '#', 0x08); vga_put(VGA_W - 1, y, '#', 0x08); }

    uint32_t len = 3, score = 0;
    int dx = 1, dy = 0, ndx = 1, ndy = 0;
    sx[0] = 40; sy[0] = 12;
    sx[1] = 39; sy[1] = 12;
    sx[2] = 38; sy[2] = 12;
    vga_put(sx[0], sy[0], 'O', 0x0A);
    vga_put(sx[1], sy[1], 'o', 0x02);
    vga_put(sx[2], sy[2], 'o', 0x02);

    int fx, fy;
    snake_food(len, &fx, &fy);
    vga_put(fx, fy, '*', 0x0C);
    snake_score(score);

    bool quit = false, dead = false, won = false;
    while (!quit && !dead && !won) {
        /* Speeds up as the score grows, down to half the starting delay. */
        uint32_t base = speed_base[set_speed];
        uint32_t cut = score * 3u;
        if (cut > base / 2) cut = base / 2;
        uint32_t need = (base - cut) * 1193u, acc = 0;
        uint16_t prev = pit_read();

        while (acc < need) {                   /* wait, but keep reading keys */
            acc += pit_elapsed(&prev);
            int k = getkey_nb();
            if (k < 0) continue;
            if (k >= 'A' && k <= 'Z') k += 32;
            int cx = 0, cy = 0;
            if (k == KEY_UP || k == 'w') cy = -1;
            else if (k == KEY_DOWN || k == 's') cy = 1;
            else if (k == KEY_LEFT || k == 'a') cx = -1;
            else if (k == KEY_RIGHT || k == 'd') cx = 1;
            else if (k == 'q' || k == 27) { quit = true; break; }
            else continue;
            if (!(cx == -dx && cy == -dy)) { ndx = cx; ndy = cy; }
        }
        if (quit) break;

        dx = ndx; dy = ndy;
        int nx = (int)sx[0] + dx, ny = (int)sy[0] + dy;
        if (nx < SN_X0 || nx > SN_X1 || ny < SN_Y0 || ny > SN_Y1) { dead = true; break; }

        bool eating = (nx == fx && ny == fy);
        uint32_t chk = eating ? len : len - 1;     /* tail moves away if not eating */
        for (uint32_t i = 0; i < chk; i++)
            if (sx[i] == nx && sy[i] == ny) { dead = true; break; }
        if (dead) break;

        if (!eating) {
            vga_put(sx[len - 1], sy[len - 1], ' ', 0x07);
        } else {
            if (len >= SN_MAX) { won = true; break; }
            len++; score++;
        }
        for (uint32_t i = len - 1; i > 0; i--) { sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
        sx[0] = (uint8_t)nx; sy[0] = (uint8_t)ny;
        vga_put(sx[1], sy[1], 'o', 0x02);
        vga_put(nx, ny, 'O', 0x0A);

        if (eating) {
            if (len >= SN_MAX) { won = true; break; }
            snake_food(len, &fx, &fy);
            vga_put(fx, fy, '*', 0x0C);
            snake_score(score);
        }
    }

    if (!quit) {
        vga_text(35, 11, won ? " YOU WIN! " : " GAME OVER ", 0x4F);
        vga_text(30, 13, " Score:", 0x07);
        vga_num(38, 13, score, 0x07);
        vga_text(42, 13, "- press a key ", 0x07);
        sleep_ms(600);
        while (getkey_nb() >= 0) {}            /* drop keys pressed meanwhile */
        getkey();
    }
    return score;
}

/* Runs snake from any tab: saves the screen, plays, then puts the UI back. */
static uint32_t launch_snake(void) {
    snap_save();
    uint32_t sc = snake();
    if (sc > snake_best) snake_best = sc;
    snap_restore();
    pit_prev = pit_read();                     /* don't count game time twice */
    ui_apply_cursor();
    return sc;
}

/* ---------- shell commands ---------- */
static void cmd_time(void) {
    struct rtc_time t;
    rtc_read(&t);
    put_dec(t.y); putc('-'); put_dec2(t.mo); putc('-'); put_dec2(t.d);
    putc(' ');
    put_dec2(t.h); putc(':'); put_dec2(t.m); putc(':'); put_dec2(t.s);
    puts(" (UTC in QEMU by default)\n");
}

static void cmd_cpu(void) {
    char vendor[13], brand[49];
    cpu_vendor(vendor);
    cpu_brand(brand);
    puts("CPU vendor: "); puts(vendor); putc('\n');
    puts("CPU model:  "); puts(brand); putc('\n');
}

static void cmd_calc(const char *args) {
    const char *p = args;
    int32_t a, b;
    if (!parse_num(&p, &a)) goto usage;
    p = skip_spaces(p);
    char op = *p;
    if (!op) goto usage;
    p++;
    if (!parse_num(&p, &b)) goto usage;

    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    int32_t r;
    switch (op) {
    case '+': r = (int32_t)(ua + ub); break;
    case '-': r = (int32_t)(ua - ub); break;
    case '*': r = (int32_t)(ua * ub); break;
    case '/':
    case '%':
        if (b == 0) { puts("Division by zero\n"); return; }
        if (b == -1) r = (op == '/') ? (int32_t)(0u - ua) : 0; /* avoid INT_MIN/-1 trap */
        else r = (op == '/') ? a / b : a % b;
        break;
    default: goto usage;
    }
    put_int(r); puts("  ("); put_hex((uint32_t)r); puts(")\n");
    return;
usage:
    puts("Usage: calc <a> <+|-|*|/|%> <b>   e.g. calc 6 * 7\n");
}

static void cmd_history(void) {
    if (!hist_count) { puts("(no history yet)\n"); return; }
    for (int i = 0; i < hist_count; i++) {
        put_dec((uint32_t)i + 1); puts("  "); puts(hist[i]); putc('\n');
    }
}

static void run(const char *cmd) {
    if (!*cmd) return;
    if (!strcmp(cmd, "help")) {
        puts("help clear echo <text> rev <text> about ver mem cpu time\n");
        puts("hex <n>  dec <n>  calc <a> <op> <b>  color <0-15>  bg <0-15>\n");
        puts("history snake reboot shutdown\n");
        puts("F1-F4 switch tabs (Shell/Info/Settings/Games), Up/Down = history\n");
    } else if (!strcmp(cmd, "clear")) {
        clear_screen();
    } else if (!strcmp(cmd, "about")) {
        puts(OS_NAME " - a tiny hobby OS written in C.\n");
        puts("Runs on x86 under QEMU. Built with gcc -m32 on CachyOS.\n");
    } else if (!strcmp(cmd, "ver")) {
        puts(OS_NAME " v" OS_VERSION "\n");
    } else if (starts_with(cmd, "echo ")) {
        puts(cmd + 5); putc('\n');
    } else if (starts_with(cmd, "rev ")) {
        const char *s = cmd + 4;
        for (size_t i = strlen(s); i > 0; i--) putc(s[i - 1]);
        putc('\n');
    } else if (!strcmp(cmd, "mem")) {
        puts("Upper memory: "); put_dec(mem_upper_kb / 1024); puts(" MiB\n");
    } else if (!strcmp(cmd, "cpu")) {
        cmd_cpu();
    } else if (!strcmp(cmd, "time")) {
        cmd_time();
    } else if (!strcmp(cmd, "history")) {
        cmd_history();
    } else if (!strcmp(cmd, "snake")) {
        uint32_t sc = launch_snake();
        puts("Snake finished. Score: "); put_dec(sc);
        puts("  (best "); put_dec(snake_best); puts(")\n");
    } else if (starts_with(cmd, "hex ")) {
        const char *p = cmd + 4; int32_t n;
        if (parse_num(&p, &n)) { put_hex((uint32_t)n); putc('\n'); }
        else puts("Usage: hex <number>\n");
    } else if (starts_with(cmd, "dec ")) {
        const char *p = cmd + 4; int32_t n;
        if (parse_num(&p, &n)) { put_int(n); putc('\n'); }
        else puts("Usage: dec <number or 0xHEX>\n");
    } else if (starts_with(cmd, "calc ")) {
        cmd_calc(cmd + 5);
    } else if (starts_with(cmd, "color ")) {
        const char *p = cmd + 6; int32_t n;
        if (parse_num(&p, &n)) color = (uint8_t)((color & 0xF0) | ((uint32_t)n & 0x0F));
        else puts("Usage: color <0-15>\n");
    } else if (starts_with(cmd, "bg ")) {
        const char *p = cmd + 3; int32_t n;
        if (parse_num(&p, &n)) {
            color = (uint8_t)((color & 0x0F) | (((uint32_t)n & 0x0F) << 4));
            clear_screen();                    /* repaint so the bg shows */
        } else puts("Usage: bg <0-15>\n");
    } else if (!strcmp(cmd, "reboot")) {
        reboot();
    } else if (!strcmp(cmd, "shutdown")) {
        poweroff();
    } else {
        puts("Unknown command: "); puts(cmd); putc('\n');
    }
}

/* ---------- Shell tab: key-by-key line editor ---------- */
#define SH_CELLS (VGA_W * (CON_BOT - CON_TOP + 1))

static char sh_line[LINE_MAX];
static size_t sh_n;                            /* characters typed so far */
static int sh_pos;                             /* history position */
static char sh_scratch[LINE_MAX];              /* line being typed before Up */
static uint16_t sh_save[SH_CELLS];             /* screen while on another tab */
static size_t sh_cx, sh_cy;

static void shell_prompt(void) {
    puts("> ");
    sh_n = 0; sh_line[0] = 0;
    sh_pos = hist_count; sh_scratch[0] = 0;
}

static void shell_key(int c) {
    if (c == '\n') {
        putc('\n');
        sh_line[sh_n] = 0;
        hist_add(sh_line);
        run(sh_line);
        shell_prompt();
        return;
    }
    if (c == '\b') { if (sh_n) { sh_n--; putc('\b'); } return; }
    if (c == KEY_UP) {
        if (sh_pos > 0) {
            if (sh_pos == hist_count) { sh_line[sh_n] = 0; strcopy(sh_scratch, sh_line, LINE_MAX); }
            sh_pos--;
            replace_line(sh_line, &sh_n, hist[sh_pos]);
        }
        return;
    }
    if (c == KEY_DOWN) {
        if (sh_pos < hist_count) {
            sh_pos++;
            replace_line(sh_line, &sh_n, sh_pos == hist_count ? sh_scratch : hist[sh_pos]);
        }
        return;
    }
    if (c >= 32 && c < 127 && sh_n + 1 < LINE_MAX) { sh_line[sh_n++] = (char)c; putc((char)c); }
}

static void shell_leave(void) {                /* stash the screen and cursor */
    for (size_t i = 0; i < SH_CELLS; i++) sh_save[i] = VGA_MEM[CON_TOP * VGA_W + i];
    sh_cx = cur_x; sh_cy = cur_y;
}

static void shell_enter(void) {                /* bring it all back */
    for (size_t i = 0; i < SH_CELLS; i++) VGA_MEM[CON_TOP * VGA_W + i] = sh_save[i];
    cur_x = sh_cx; cur_y = sh_cy;
}

/* A color change should repaint the stashed shell screen too. */
static void shell_recolor(void) {
    for (size_t i = 0; i < SH_CELLS; i++)
        sh_save[i] = (uint16_t)((sh_save[i] & 0x00FF) | ((uint16_t)color << 8));
}

/* ---------- UI drawing helpers ---------- */
static void draw_box(int x, int y, int w, int h, uint8_t attr, const char *title) {
    for (int j = 1; j < h - 1; j++)
        for (int i = 1; i < w - 1; i++) vga_put(x + i, y + j, ' ', 0x07);
    for (int i = 1; i < w - 1; i++) {
        vga_put(x + i, y, (char)0xC4, attr);
        vga_put(x + i, y + h - 1, (char)0xC4, attr);
    }
    for (int j = 1; j < h - 1; j++) {
        vga_put(x, y + j, (char)0xB3, attr);
        vga_put(x + w - 1, y + j, (char)0xB3, attr);
    }
    vga_put(x, y, (char)0xDA, attr);               vga_put(x + w - 1, y, (char)0xBF, attr);
    vga_put(x, y + h - 1, (char)0xC0, attr);       vga_put(x + w - 1, y + h - 1, (char)0xD9, attr);
    if (title) {
        int l = (int)strlen(title);
        vga_put(x + 2, y, ' ', attr);
        vga_text(x + 3, y, title, 0x0F);
        vga_put(x + 3 + l, y, ' ', attr);
    }
}

static void vga_kv(int x, int y, const char *label, const char *value) {
    vga_text(x, y, label, 0x03);
    vga_text(x + 9, y, value, 0x0F);
}

static void page_clear(void) { vga_clear_area(0, CON_TOP, VGA_W, CON_BOT - CON_TOP + 1, 0x07); }

static const char *const tab_labels[TAB_COUNT] = {
    "F1 Shell", "F2 Info", "F3 Settings", "F4 Games"
};

static void draw_tabbar(void) {
    vga_clear_area(0, 0, VGA_W, 1, 0x70);
    vga_text(1, 0, OS_NAME, 0x74);
    int x = 7;
    for (int i = 0; i < TAB_COUNT; i++) {
        uint8_t a = (i == cur_tab) ? 0x1F : 0x70;
        int w = (int)strlen(tab_labels[i]) + 2;
        vga_clear_area(x, 0, w, 1, a);
        vga_text(x + 1, 0, tab_labels[i], a);
        x += w + 1;
    }
    vga_text(VGA_W - (int)strlen("v" OS_VERSION) - 1, 0, "v" OS_VERSION, 0x70);
}

static const char *const tab_hints[TAB_COUNT] = {
    " F1-F4: switch tabs  |  Up/Down: command history  |  try 'help'",
    " F1-F4 or Tab: switch tabs  |  Esc: back to the Shell",
    " Up/Down: select  |  Left/Right/Enter: change  |  Tab: next tab",
    " Up/Down: select  |  Enter: play  |  Tab: next tab",
};

static void draw_clock(const struct rtc_time *t) {
    if (set_clock) vga_hms(71, 24, t->h, t->m, t->s, 0x70);
    else vga_clear_area(71, 24, 8, 1, 0x70);
}

static void draw_status(void) {
    vga_clear_area(0, 24, VGA_W, 1, 0x70);
    vga_text(0, 24, tab_hints[cur_tab], 0x70);
    struct rtc_time t;
    rtc_read(&t);
    draw_clock(&t);
}

/* ---------- Info tab ---------- */
static void info_update(const struct rtc_time *t) {
    vga_clear_area(52, 3, 25, 6, 0x07);
    int x;
    x = vga_num(52, 3, t->y, 0x0F);
    vga_put(x, 3, '-', 0x0F); vga_num2(x + 1, 3, t->mo, 0x0F);
    vga_put(x + 3, 3, '-', 0x0F); vga_num2(x + 4, 3, t->d, 0x0F);
    vga_hms(52, 4, t->h, t->m, t->s, 0x0F);
    vga_text(61, 4, "(UTC in QEMU)", 0x08);
    vga_hms(52, 5, uptime_sec / 3600, (uptime_sec / 60) % 60, uptime_sec % 60, 0x0F);
    vga_text(52, 6, "PIT 1.193 MHz (polled)", 0x0F);
    x = vga_num(52, 7, (uint32_t)hist_count, 0x0F);
    vga_text(x, 7, hist_count == 1 ? " entry" : " entries", 0x0F);
    vga_num(52, 8, snake_best, 0x0F);
}

static void info_draw(void) {
    page_clear();
    draw_box(1, 2, 38, 9, 0x0B, "System");
    draw_box(41, 2, 38, 9, 0x0B, "Clock");
    draw_box(1, 12, 78, 8, 0x0B, "Keys");

    char vendor[13], brand[49];
    cpu_vendor(vendor);
    cpu_brand(brand);
    brand[26] = 0;                             /* keep it inside the box */

    vga_kv(3, 3, "OS", OS_NAME);
    vga_kv(3, 4, "Version", OS_VERSION " (x86, 32-bit)");
    vga_kv(3, 5, "CPU", vendor);
    vga_kv(3, 6, "Model", brand);
    vga_text(3, 7, "RAM", 0x03);
    int x = vga_num(12, 7, mem_upper_kb / 1024 + 1, 0x0F);
    vga_text(x, 7, " MiB", 0x0F);
    vga_kv(3, 8, "Video", "VGA text 80x25");
    vga_kv(3, 9, "Serial", serial_on ? "COM1 mirror on" : "COM1 mirror off");

    vga_text(43, 3, "Date", 0x03);
    vga_text(43, 4, "Time", 0x03);
    vga_text(43, 5, "Uptime", 0x03);
    vga_text(43, 6, "Timer", 0x03);
    vga_text(43, 7, "History", 0x03);
    vga_text(43, 8, "Best snake", 0x03);

    static const char *const keys[][2] = {
        { "F1",  "Shell: type commands (try 'help'), Up/Down recalls history" },
        { "F2",  "Info: this page, refreshed every second" },
        { "F3",  "Settings: colors, clock, serial mirror, snake speed" },
        { "F4",  "Games: launch snake" },
        { "Tab", "Next tab (outside the Shell)" },
        { "Esc", "Back to the Shell (outside the Shell)" },
    };
    for (int i = 0; i < 6; i++) {
        vga_text(3, 13 + i, keys[i][0], 0x0E);
        vga_text(10, 13 + i, keys[i][1], 0x07);
    }

    struct rtc_time t;
    rtc_read(&t);
    info_update(&t);
}

/* ---------- Settings tab ---------- */
#define SET_COUNT 5

static int set_sel;

static const char *const color_names[16] = {
    "Black", "Blue", "Green", "Cyan", "Red", "Magenta", "Brown", "Light Gray",
    "Dark Gray", "Light Blue", "Light Green", "Light Cyan", "Light Red",
    "Pink", "Yellow", "White"
};
static const char *const speed_names[3] = { "Slow", "Normal", "Fast" };

static void settings_draw(void) {
    static const char *const labels[SET_COUNT] = {
        "Text color", "Background", "Clock in status bar",
        "Mirror output to serial", "Snake speed"
    };
    page_clear();
    draw_box(1, 2, 56, 9, 0x0B, "Settings");

    for (int i = 0; i < SET_COUNT; i++) {
        int y = 4 + i;
        uint8_t a = (i == set_sel) ? 0x1F : 0x07;
        const char *name;
        switch (i) {
        case 0:  name = color_names[color & 0x0F]; break;
        case 1:  name = color_names[(color >> 4) & 7]; break;
        case 2:  name = set_clock ? "On" : "Off"; break;
        case 3:  name = serial_on ? "On" : "Off"; break;
        default: name = speed_names[set_speed]; break;
        }
        char v[40];
        v[0] = 0;
        catstr(v, "< ", sizeof v); catstr(v, name, sizeof v); catstr(v, " >", sizeof v);

        vga_clear_area(2, y, 54, 1, a);
        vga_text(4, y, labels[i], a);
        vga_text(30, y, v, a);
        if (i == 0) {                          /* color swatches */
            vga_put(50, y, (char)0xDB, color & 0x0F);
            vga_put(51, y, (char)0xDB, color & 0x0F);
        } else if (i == 1) {
            vga_put(50, y, ' ', (uint8_t)(((color >> 4) & 7) << 4));
            vga_put(51, y, ' ', (uint8_t)(((color >> 4) & 7) << 4));
        }
    }

    draw_box(1, 12, 56, 7, 0x0B, "Preview");
    vga_clear_area(2, 13, 54, 5, color);
    vga_text(4, 14, "The quick brown fox jumps over the lazy dog", color);
    vga_text(4, 16, "> help", color);
    vga_text(2, 20, " Colors apply to the Shell tab. Esc returns to the Shell.", 0x08);
}

static void settings_change(int dir) {
    int fg = color & 0x0F, bg = (color >> 4) & 0x0F;
    switch (set_sel) {
    case 0:
        do { fg = (fg + dir + 16) & 15; } while (fg == bg);   /* never invisible text */
        color = (uint8_t)((color & 0xF0) | fg);
        shell_recolor();
        break;
    case 1:
        bg &= 7;
        do { bg = (bg + dir + 8) & 7; } while (bg == fg);
        color = (uint8_t)((color & 0x0F) | (bg << 4));
        shell_recolor();
        break;
    case 2: set_clock = !set_clock; break;
    case 3: serial_on = !serial_on; break;
    default: set_speed = (set_speed + dir + 3) % 3; break;
    }
    settings_draw();
    draw_status();
}

/* ---------- Games tab ---------- */
static void games_draw(void) {
    page_clear();
    draw_box(1, 2, 78, 13, 0x0B, "Games");
    vga_clear_area(3, 4, 74, 1, 0x1F);
    vga_text(5, 4, "Snake", 0x1F);
    vga_text(5, 6, "Steer with the arrow keys or WASD and eat the * to grow.", 0x07);
    vga_text(5, 7, "Don't hit the walls or yourself. Q or Esc quits the game.", 0x07);
    vga_text(5, 8, "Speed can be changed in Settings (F3).", 0x07);
    vga_text(5, 10, "Best score this session:", 0x03);
    vga_num(30, 10, snake_best, 0x0F);
    vga_text(5, 12, "Press Enter to play.", 0x0E);
}

/* ---------- tab switching and the main loop ---------- */
static void ui_show_page(void) {
    switch (cur_tab) {
    case TAB_SHELL:    shell_enter(); break;
    case TAB_INFO:     info_draw(); break;
    case TAB_SETTINGS: settings_draw(); break;
    default:           games_draw(); break;
    }
    ui_apply_cursor();
}

static void ui_switch(int t) {
    if (t == cur_tab) return;
    if (cur_tab == TAB_SHELL) shell_leave();
    cur_tab = t;
    draw_tabbar();
    ui_show_page();
    draw_status();
}

static void ui_init(void) {
    cur_tab = TAB_SHELL;
    clear_screen();
    draw_tabbar();
    draw_status();
    puts(OS_NAME " v" OS_VERSION " booted.\n");
    puts("F1-F4 switch tabs. Type 'help' for commands.\n\n");
    shell_prompt();
    ui_apply_cursor();
    pit_prev = pit_read();
}

static uint32_t last_sec = 0xFFFFFFFFu;

static void ui_tick(void) {                    /* once a second: clock + info */
    pit_elapsed(&pit_prev);
    if (uptime_sec == last_sec) return;
    last_sec = uptime_sec;
    struct rtc_time t;
    rtc_read(&t);
    draw_clock(&t);
    if (cur_tab == TAB_INFO) info_update(&t);
}

static void ui_key(int k) {
    if (k >= KEY_F1 && k <= KEY_F4) { ui_switch(k - KEY_F1); return; }
    if (cur_tab == TAB_SHELL) { shell_key(k); return; }
    if (k == 27) { ui_switch(TAB_SHELL); return; }
    if (k == '\t') { ui_switch((cur_tab + 1) % TAB_COUNT); return; }

    if (cur_tab == TAB_SETTINGS) {
        if (k == KEY_UP)   { set_sel = (set_sel + SET_COUNT - 1) % SET_COUNT; settings_draw(); }
        else if (k == KEY_DOWN) { set_sel = (set_sel + 1) % SET_COUNT; settings_draw(); }
        else if (k == KEY_LEFT) settings_change(-1);
        else if (k == KEY_RIGHT || k == '\n') settings_change(1);
        return;
    }
    if (cur_tab == TAB_GAMES && k == '\n') {
        launch_snake();
        games_draw();                          /* refresh the best score */
        return;
    }
    if (k == KEY_LEFT)  ui_switch((cur_tab + TAB_COUNT - 1) % TAB_COUNT);
    if (k == KEY_RIGHT) ui_switch((cur_tab + 1) % TAB_COUNT);
}

void kernel_main(uint32_t magic, uint32_t *mbi) {
    serial_init();
    pit_init();
    if (magic == 0x2BADB002 && (mbi[0] & 1))   /* flags bit 0: mem_* valid */
        mem_upper_kb = mbi[2];

    splash();
    ui_init();

    for (;;) {
        ui_tick();
        int k = getkey_nb();
        if (k < 0) { __asm__ volatile("pause"); continue; }
        ui_key(k);
    }
}
