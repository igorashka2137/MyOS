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
#define OS_VERSION "0.6"

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

/* ---------- RAM filesystem (flat, in memory, lost on reboot) ---------- */
/* FS-BEGIN (pure C, no I/O: extracted and unit-tested on the host) */
#define FS_FILES 16
#define FS_NAME  16
#define FS_SIZE  2048

struct fs_file {
    bool used;
    char name[FS_NAME];
    uint16_t size;
    char data[FS_SIZE];
};
static struct fs_file fs[FS_FILES];

enum { FS_OK = 0, FS_ERR_FULL = -1, FS_ERR_EXISTS = -2, FS_ERR_NAME = -3, FS_ERR_NOFILE = -4 };

static bool fs_valid_name(const char *n) {
    size_t len = strlen(n);
    if (len == 0 || len >= FS_NAME) return false;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static int fs_find(const char *name) {
    for (int i = 0; i < FS_FILES; i++)
        if (fs[i].used && !strcmp(fs[i].name, name)) return i;
    return -1;
}

/* Returns the new file's index, or a negative FS_ERR_* code. */
static int fs_create(const char *name) {
    if (!fs_valid_name(name)) return FS_ERR_NAME;
    if (fs_find(name) >= 0) return FS_ERR_EXISTS;
    for (int i = 0; i < FS_FILES; i++)
        if (!fs[i].used) {
            fs[i].used = true;
            strcopy(fs[i].name, name, FS_NAME);
            fs[i].size = 0;
            return i;
        }
    return FS_ERR_FULL;
}

static void fs_delete(int i) {
    fs[i].used = false;
    fs[i].size = 0;
    fs[i].name[0] = 0;
}

static int fs_rename(int i, const char *newname) {
    if (!fs_valid_name(newname)) return FS_ERR_NAME;
    int j = fs_find(newname);
    if (j >= 0 && j != i) return FS_ERR_EXISTS;
    strcopy(fs[i].name, newname, FS_NAME);
    return FS_OK;
}

/* Replaces the contents; returns bytes stored (truncated to FS_SIZE). */
static size_t fs_set(int i, const char *data, size_t len) {
    if (len > FS_SIZE) len = FS_SIZE;
    for (size_t k = 0; k < len; k++) fs[i].data[k] = data[k];
    fs[i].size = (uint16_t)len;
    return len;
}

/* Appends; returns bytes actually appended (0 when the file is full). */
static size_t fs_append(int i, const char *data, size_t len) {
    size_t room = (size_t)FS_SIZE - fs[i].size;
    if (len > room) len = room;
    for (size_t k = 0; k < len; k++) fs[i].data[fs[i].size + k] = data[k];
    fs[i].size = (uint16_t)(fs[i].size + len);
    return len;
}

/* Copies file src to a new file; returns the new index or an FS_ERR_* code. */
static int fs_copy(int src, const char *dstname) {
    int d = fs_create(dstname);
    if (d < 0) return d;
    fs_set(d, fs[src].data, fs[src].size);
    return d;
}

static int fs_count(void) {
    int n = 0;
    for (int i = 0; i < FS_FILES; i++) if (fs[i].used) n++;
    return n;
}

static uint32_t fs_bytes(void) {
    uint32_t n = 0;
    for (int i = 0; i < FS_FILES; i++) if (fs[i].used) n += fs[i].size;
    return n;
}

/* Index of the n-th used file (0-based), or -1. Used by the Files tab. */
static int fs_nth(int n) {
    for (int i = 0; i < FS_FILES; i++)
        if (fs[i].used && n-- == 0) return i;
    return -1;
}

/* Creates a file with initial text; ignores errors (used for seed files). */
static void fs_seed(const char *name, const char *text) {
    int i = fs_create(name);
    if (i >= 0) fs_set(i, text, strlen(text));
}
/* FS-END */

/* ---------- text editor core (buffer operations, no drawing) ---------- */
/* ED-BEGIN (pure C: extracted and unit-tested on the host) */
static char ed_buf[FS_SIZE];
static size_t ed_len, ed_cur;                  /* text length, cursor offset */
static bool ed_dirty;

static size_t ed_line_start(size_t pos) {
    while (pos > 0 && ed_buf[pos - 1] != '\n') pos--;
    return pos;
}
static size_t ed_line_end(size_t pos) {
    while (pos < ed_len && ed_buf[pos] != '\n') pos++;
    return pos;
}
static size_t ed_cur_line(void) {              /* 0-based */
    size_t n = 0;
    for (size_t i = 0; i < ed_cur; i++) if (ed_buf[i] == '\n') n++;
    return n;
}
static size_t ed_cur_col(void) { return ed_cur - ed_line_start(ed_cur); }

static bool ed_insert(char c) {
    if (ed_len >= FS_SIZE) return false;
    for (size_t i = ed_len; i > ed_cur; i--) ed_buf[i] = ed_buf[i - 1];
    ed_buf[ed_cur++] = c;
    ed_len++;
    ed_dirty = true;
    return true;
}
static void ed_backspace(void) {
    if (!ed_cur) return;
    for (size_t i = ed_cur - 1; i + 1 < ed_len; i++) ed_buf[i] = ed_buf[i + 1];
    ed_cur--; ed_len--;
    ed_dirty = true;
}
static void ed_delete(void) {
    if (ed_cur >= ed_len) return;
    for (size_t i = ed_cur; i + 1 < ed_len; i++) ed_buf[i] = ed_buf[i + 1];
    ed_len--;
    ed_dirty = true;
}
static void ed_left(void)  { if (ed_cur) ed_cur--; }
static void ed_right(void) { if (ed_cur < ed_len) ed_cur++; }
static void ed_home(void)  { ed_cur = ed_line_start(ed_cur); }
static void ed_end(void)   { ed_cur = ed_line_end(ed_cur); }
static void ed_up(void) {
    size_t ls = ed_line_start(ed_cur);
    if (ls == 0) return;                       /* already on the first line */
    size_t col = ed_cur - ls;
    size_t ps = ed_line_start(ls - 1);
    size_t plen = ls - 1 - ps;
    ed_cur = ps + (col < plen ? col : plen);
}
static void ed_down(void) {
    size_t le = ed_line_end(ed_cur);
    if (le >= ed_len) return;                  /* already on the last line */
    size_t col = ed_cur - ed_line_start(ed_cur);
    size_t ns = le + 1;
    size_t nlen = ed_line_end(ns) - ns;
    ed_cur = ns + (col < nlen ? col : nlen);
}
static void ed_load(const char *data, size_t len) {
    if (len > FS_SIZE) len = FS_SIZE;
    for (size_t i = 0; i < len; i++) ed_buf[i] = data[i];
    ed_len = len; ed_cur = 0; ed_dirty = false;
}
/* ED-END */

/* Editor session: which file is open, scroll position, status message. */
static char ed_name[FS_NAME];
static bool ed_loaded;
static size_t ed_top, ed_left_col;
static char ed_msg[48];

static void ed_set_msg(const char *m) { strcopy(ed_msg, m, sizeof ed_msg); }

/* Opens (creating if needed) a file in the editor. Returns 0 or an FS_ERR_*. */
static int ed_open(const char *name) {
    bool same = ed_loaded && !strcmp(ed_name, name);
    if (same && ed_dirty) { ed_set_msg("Back to your unsaved edits"); return 0; }

    int i = fs_find(name);
    bool created = false;
    if (i < 0) {
        i = fs_create(name);
        if (i < 0) return i;
        created = true;
    }
    if (ed_loaded && ed_dirty) {               /* switching away from edits */
        ed_set_msg("Discarded unsaved changes to ");
        catstr(ed_msg, ed_name, sizeof ed_msg);
    } else {
        ed_set_msg(created ? "New file" : "Opened");
    }
    ed_load(fs[i].data, fs[i].size);
    strcopy(ed_name, name, FS_NAME);
    ed_loaded = true;
    ed_top = 0; ed_left_col = 0;
    return 0;
}

static void ed_save(void) {
    if (!ed_loaded) return;
    int i = fs_find(ed_name);
    if (i < 0) {                               /* deleted meanwhile: recreate */
        i = fs_create(ed_name);
        if (i < 0) { ed_set_msg("Save failed: filesystem full"); return; }
    }
    fs_set(i, ed_buf, ed_len);
    ed_dirty = false;
    ed_set_msg("Saved");
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

/* ---------- PC speaker (PIT channel 2) ---------- */
static bool set_sound = true;

static void speaker_on(uint32_t hz) {
    if (hz < 20) hz = 20;
    if (hz > 20000) hz = 20000;
    uint32_t div = PIT_HZ / hz;
    outb(0x43, 0xB6);                          /* channel 2, lo/hi byte, square wave */
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)(div >> 8));
    outb(0x61, (uint8_t)(inb(0x61) | 3));      /* gate the PIT to the speaker */
}
static void speaker_off(void) { outb(0x61, (uint8_t)(inb(0x61) & 0xFC)); }

static void beep(uint32_t hz, uint32_t ms) {
    if (!set_sound) return;
    speaker_on(hz);
    sleep_ms(ms);
    speaker_off();
}

/* ---------- keyboard (polled, scancode set 1, US layout) ---------- */
#define KEY_UP    0x100
#define KEY_DOWN  0x101
#define KEY_LEFT  0x102
#define KEY_RIGHT 0x103
#define KEY_F1    0x110     /* F1..F6 = 0x110..0x115 */
#define KEY_F6    0x115
#define KEY_HOME  0x120
#define KEY_END   0x121
#define KEY_DEL   0x122
#define KEY_PGUP  0x123
#define KEY_PGDN  0x124
#define KEY_CTRL(c) (0x200 | (c))   /* Ctrl + lowercase letter */

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
static bool ctrl;
static bool e0;                                /* saw 0xE0 extended prefix */

/* Non-blocking: returns a character, a KEY_* code, or -1 if nothing. */
static int getkey_nb(void) {
    if (!(inb(0x64) & 1)) return -1;
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) { e0 = true; return -1; }
    if (e0) {
        e0 = false;
        if (sc == 0x1D) { ctrl = true;  return -1; }   /* right Ctrl */
        if (sc == 0x9D) { ctrl = false; return -1; }
        if (sc & 0x80) return -1;              /* extended key release */
        switch (sc) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x53: return KEY_DEL;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        default:   return -1;
        }
    }

    if (sc == 0x1D) { ctrl = true;  return -1; }   /* left Ctrl */
    if (sc == 0x9D) { ctrl = false; return -1; }

    if (sc == 0x2A || sc == 0x36) { shift = true;  return -1; }
    if (sc == 0xAA || sc == 0xB6) { shift = false; return -1; }
    if (sc == 0x3A) { caps = !caps; return -1; }   /* Caps Lock press */
    if (sc & 0x80) return -1;                      /* key release */
    if (sc >= 0x3B && sc <= 0x40) return KEY_F1 + (sc - 0x3B);   /* F1..F6 */

    char lo = map_lo[sc];
    if (ctrl) return (lo >= 'a' && lo <= 'z') ? KEY_CTRL(lo) : -1;
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
enum { TAB_SHELL, TAB_INFO, TAB_SETTINGS, TAB_GAMES, TAB_FILES, TAB_EDITOR, TAB_COUNT };

static int cur_tab = TAB_SHELL;
static int pending_tab = -1;                   /* tab to switch to after a command */
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
static void ui_apply_cursor(void) {            /* Shell and Editor show a cursor */
    if (cur_tab == TAB_SHELL) { cursor_show(); update_cursor(); }
    else if (cur_tab == TAB_EDITOR && ed_loaded) cursor_show();   /* ed_draw positions it */
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
    beep(523, 90); beep(659, 90); beep(784, 180);   /* boot chime: C E G */
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

/* ---------- tetris ---------- */
/* TET-BEGIN (game rules only, no I/O: extracted and fuzz-tested on the host) */
#define TW 10
#define TH 20

static uint8_t tboard[TH][TW];                 /* 0 = empty, else piece type + 1 */
/* 4x4 bitmaps, bit = y*4 + x:  I O T S Z J L */
static const uint16_t tshape[7] = { 0x00F0, 0x0660, 0x0072, 0x0036, 0x0063, 0x0071, 0x0074 };
static int tbag[7], tbag_i = 7;                /* 7-bag randomizer */
static int t_type, t_next;
static uint16_t t_mask;                        /* current piece, rotated */
static int t_px, t_py;                         /* top-left of its 4x4 box */
static uint32_t t_score, t_lines, t_level;
static bool t_over;

static uint16_t t_rot(uint16_t m) {            /* rotate 90 degrees clockwise */
    uint16_t r = 0;
    for (int i = 0; i < 16; i++)
        if (m & (1u << i)) {
            int x = i & 3, y = i >> 2;
            r |= (uint16_t)(1u << ((3 - y) + 4 * x));
        }
    return r;
}

static bool t_fits(uint16_t m, int px, int py) {
    for (int i = 0; i < 16; i++)
        if (m & (1u << i)) {
            int x = px + (i & 3), y = py + (i >> 2);
            if (x < 0 || x >= TW || y >= TH) return false;
            if (y >= 0 && tboard[y][x]) return false;      /* y < 0: above the board */
        }
    return true;
}

static int t_take_bag(void) {
    if (tbag_i >= 7) {
        for (int i = 0; i < 7; i++) tbag[i] = i;
        for (int i = 6; i > 0; i--) {
            int j = (int)(rnd() % (uint32_t)(i + 1));
            int t = tbag[i]; tbag[i] = tbag[j]; tbag[j] = t;
        }
        tbag_i = 0;
    }
    return tbag[tbag_i++];
}

static void t_spawn(void) {
    t_type = t_next;
    t_next = t_take_bag();
    t_mask = tshape[t_type];
    t_px = 3; t_py = -1;
    if (!t_fits(t_mask, t_px, t_py)) t_over = true;
}

static void t_new_game(void) {
    for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) tboard[y][x] = 0;
    t_score = 0; t_lines = 0; t_level = 1; t_over = false;
    tbag_i = 7;
    t_next = t_take_bag();
    t_spawn();
}

static bool t_move(int dx) {
    if (!t_fits(t_mask, t_px + dx, t_py)) return false;
    t_px += dx;
    return true;
}

static bool t_rotate_piece(void) {             /* with simple wall kicks */
    static const int kicks[5] = { 0, -1, 1, -2, 2 };
    uint16_t r = t_rot(t_mask);
    for (int k = 0; k < 5; k++)
        if (t_fits(r, t_px + kicks[k], t_py)) {
            t_mask = r; t_px += kicks[k];
            return true;
        }
    return false;
}

static int t_clear_lines(void) {
    int cleared = 0;
    for (int y = TH - 1; y >= 0; ) {
        bool full = true;
        for (int x = 0; x < TW; x++) if (!tboard[y][x]) { full = false; break; }
        if (!full) { y--; continue; }
        for (int r = y; r > 0; r--)
            for (int x = 0; x < TW; x++) tboard[r][x] = tboard[r - 1][x];
        for (int x = 0; x < TW; x++) tboard[0][x] = 0;
        cleared++;                             /* same y again: rows shifted down */
    }
    return cleared;
}

/* Fixes the piece on the board, scores any lines and spawns the next piece. */
static void t_lock_piece(void) {
    static const uint32_t pts[5] = { 0, 100, 300, 500, 800 };
    for (int i = 0; i < 16; i++)
        if (t_mask & (1u << i)) {
            int x = t_px + (i & 3), y = t_py + (i >> 2);
            if (y < 0) t_over = true;          /* locked above the visible board */
            else tboard[y][x] = (uint8_t)(t_type + 1);
        }
    int n = t_clear_lines();
    t_lines += (uint32_t)n;
    t_score += pts[n] * t_level;
    t_level = 1 + t_lines / 10;
    if (!t_over) t_spawn();
}

/* One gravity step. Returns true if the piece locked instead of moving. */
static bool t_step_down(void) {
    if (t_fits(t_mask, t_px, t_py + 1)) { t_py++; return false; }
    t_lock_piece();
    return true;
}

static void t_hard_drop(void) {
    uint32_t n = 0;
    while (t_fits(t_mask, t_px, t_py + 1)) { t_py++; n++; }
    t_score += 2 * n;
    t_lock_piece();
}
/* TET-END */

static uint32_t tetris_best;

#define TB_X 26                                /* board: 2 screen columns per cell */
#define TB_Y 2
static const uint8_t tcol[7] = { 0x0B, 0x0E, 0x0D, 0x0A, 0x0C, 0x09, 0x06 };

static void tet_cell(int x, int y, uint8_t a) {
    vga_put(TB_X + 2 * x,     TB_Y + y, (char)0xDB, a);
    vga_put(TB_X + 2 * x + 1, TB_Y + y, (char)0xDB, a);
}

static void tet_frame(void) {
    vga_text(0, 0, "TETRIS", 0x0F);
    for (int y = 0; y < TH; y++) {
        vga_put(TB_X - 1, TB_Y + y, (char)0xDB, 0x08);
        vga_put(TB_X + 2 * TW, TB_Y + y, (char)0xDB, 0x08);
    }
    for (int x = -1; x <= 2 * TW; x++) vga_put(TB_X + x, TB_Y + TH, (char)0xDB, 0x08);
    vga_text(2, 4,  "Left/Right  move", 0x07);
    vga_text(2, 5,  "Up          rotate", 0x07);
    vga_text(2, 6,  "Down        soft drop", 0x07);
    vga_text(2, 7,  "Space       hard drop", 0x07);
    vga_text(2, 8,  "P           pause", 0x07);
    vga_text(2, 9,  "Q / Esc     quit", 0x07);
    vga_text(50, 3, "NEXT", 0x03);
    vga_text(50, 10, "SCORE", 0x03);
    vga_text(50, 13, "LINES", 0x03);
    vga_text(50, 16, "LEVEL", 0x03);
}

static void tet_draw(bool paused) {
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            uint8_t v = tboard[y][x];
            if (v) tet_cell(x, y, tcol[v - 1]);
            else {
                vga_put(TB_X + 2 * x, TB_Y + y, (char)0xFA, 0x08);
                vga_put(TB_X + 2 * x + 1, TB_Y + y, ' ', 0x07);
            }
        }
    if (!t_over)
        for (int i = 0; i < 16; i++)
            if (t_mask & (1u << i)) {
                int x = t_px + (i & 3), y = t_py + (i >> 2);
                if (y >= 0 && y < TH && x >= 0 && x < TW) tet_cell(x, y, tcol[t_type]);
            }

    vga_clear_area(50, 4, 10, 4, 0x07);        /* next-piece preview */
    for (int i = 0; i < 16; i++)
        if (tshape[t_next] & (1u << i)) {
            vga_put(51 + 2 * (i & 3),     4 + (i >> 2), (char)0xDB, tcol[t_next]);
            vga_put(51 + 2 * (i & 3) + 1, 4 + (i >> 2), (char)0xDB, tcol[t_next]);
        }

    vga_clear_area(50, 11, 10, 1, 0x07); vga_num(50, 11, t_score, 0x0F);
    vga_clear_area(50, 14, 10, 1, 0x07); vga_num(50, 14, t_lines, 0x0F);
    vga_clear_area(50, 17, 10, 1, 0x07); vga_num(50, 17, t_level, 0x0F);

    if (paused) vga_text(TB_X + 7, TB_Y + 9, " PAUSED ", 0x4F);
}

/* Takes over the whole screen. Returns the score; caller restores the UI. */
static uint32_t tetris(void) {
    rng_state = rdtsc_lo() | 1u;
    cursor_hide();
    for (size_t i = 0; i < VGA_W * VGA_H; i++) VGA_MEM[i] = 0x0720;
    tet_frame();
    t_new_game();
    tet_draw(false);

    bool quit = false, paused = false;
    uint32_t acc = 0;
    uint16_t prev = pit_read();
    while (!quit && !t_over) {
        uint32_t d = pit_elapsed(&prev);
        if (!paused) acc += d;
        bool redraw = false;

        int k = getkey_nb();
        if (k >= 'A' && k <= 'Z') k += 32;
        if (k == 'q' || k == 27) quit = true;
        else if (k == 'p') { paused = !paused; redraw = true; }
        else if (k >= 0 && !paused) {
            if (k == KEY_LEFT || k == 'a')       redraw |= t_move(-1);
            else if (k == KEY_RIGHT || k == 'd') redraw |= t_move(1);
            else if (k == KEY_UP || k == 'w')    redraw |= t_rotate_piece();
            else if (k == KEY_DOWN || k == 's') {
                if (!t_step_down()) t_score++;   /* +1 per soft-dropped row */
                acc = 0; redraw = true;
            } else if (k == ' ') { t_hard_drop(); acc = 0; redraw = true; }
        }

        uint32_t delay_ms = t_level > 10 ? 100u : 800u - (t_level - 1) * 70u;
        if (!paused && !t_over && acc >= delay_ms * 1193u) {
            acc = 0;
            t_step_down();
            redraw = true;
        }
        if (redraw) tet_draw(paused);
    }

    if (!quit) {
        tet_draw(false);
        vga_text(TB_X + 5, TB_Y + 8,  "  GAME OVER  ", 0x4F);
        vga_text(TB_X + 5, TB_Y + 10, " press a key  ", 0x07);
        sleep_ms(600);
        while (getkey_nb() >= 0) {}            /* drop keys pressed meanwhile */
        getkey();
    }
    return t_score;
}

static uint32_t launch_tetris(void) {
    snap_save();
    uint32_t sc = tetris();
    if (sc > tetris_best) tetris_best = sc;
    snap_restore();
    pit_prev = pit_read();
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

/* ---------- file commands ---------- */
/* If cmd is `name` or `name <args>`, returns a pointer to the args (possibly
 * an empty string); otherwise NULL. */
static const char *cmd_args(const char *cmd, const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++) if (cmd[i] != name[i]) return NULL;
    if (cmd[n] == 0) return cmd + n;
    if (cmd[n] != ' ') return NULL;
    return skip_spaces(cmd + n);
}

/* Copies the next space-separated word into out and advances *pp. */
static bool next_word(const char **pp, char *out, size_t max) {
    const char *p = skip_spaces(*pp);
    size_t n = 0;
    while (*p && *p != ' ') { if (n + 1 < max) out[n++] = *p; p++; }
    out[n] = 0;
    *pp = p;
    return n > 0;
}

static bool no_more(const char *a) { return !*skip_spaces(a); }   /* no extra words */

static void fs_err(int e, const char *name) {
    switch (e) {
    case FS_ERR_FULL:
        puts("Filesystem full (max "); put_dec(FS_FILES); puts(" files)\n"); break;
    case FS_ERR_EXISTS:
        puts(name); puts(": already exists\n"); break;
    case FS_ERR_NAME:
        puts("Bad file name (letters, digits . _ -  max "); put_dec(FS_NAME - 1);
        puts(" chars)\n"); break;
    default:
        puts(name); puts(": no such file\n"); break;
    }
}

static void cmd_ls(void) {
    int n = fs_count();
    if (!n) { puts("(no files)\n"); return; }
    for (int k = 0; k < n; k++) {
        int i = fs_nth(k);
        puts(fs[i].name);
        for (size_t c = strlen(fs[i].name); c < 18; c++) putc(' ');
        put_dec(fs[i].size); puts(" B\n");
    }
    put_dec((uint32_t)n); puts(n == 1 ? " file, " : " files, ");
    put_dec(fs_bytes()); puts(" bytes\n");
}

static void cmd_cat(int i) {
    for (uint16_t k = 0; k < fs[i].size; k++) {
        char c = fs[i].data[k];
        putc((c < 32 && c != '\n') ? '.' : c);
    }
    if (fs[i].size && fs[i].data[fs[i].size - 1] != '\n') putc('\n');
}

/* write/append <file> <text>: adds a newline after the text. */
static void cmd_write(const char *args, bool append) {
    char name[LINE_MAX];
    if (!next_word(&args, name, sizeof name)) {
        puts(append ? "Usage: append <file> <text>\n" : "Usage: write <file> <text>\n");
        return;
    }
    const char *text = skip_spaces(args);
    int i = fs_find(name);
    if (i < 0) { i = fs_create(name); if (i < 0) { fs_err(i, name); return; } }
    size_t len = strlen(text), got;
    if (append) { got = fs_append(i, text, len); got += fs_append(i, "\n", 1); }
    else        { got = fs_set(i, text, len);    got += fs_append(i, "\n", 1); }
    if (got < len + 1) puts("(file full: text was truncated)\n");
}

/* "Ode to Joy", first phrase: {frequency Hz, length ms}. */
static const uint16_t tune_notes[][2] = {
    {659,250},{659,250},{698,250},{784,250},{784,250},{698,250},{659,250},{587,250},
    {523,250},{523,250},{587,250},{659,250},{659,375},{587,125},{587,500},
};

static void cmd_tune(void) {
    if (!set_sound) { puts("Sound is off (turn it on in Settings, F3)\n"); return; }
    puts("Playing... (press any key to stop)\n");
    for (size_t i = 0; i < sizeof tune_notes / sizeof tune_notes[0]; i++) {
        beep(tune_notes[i][0], tune_notes[i][1]);
        sleep_ms(25);
        if (getkey_nb() >= 0) break;
    }
}

static void cmd_beep(const char *args) {
    int32_t hz = 800, ms = 200;
    if (*args && !parse_num(&args, &hz)) { puts("Usage: beep [hz] [ms]\n"); return; }
    if (*skip_spaces(args) && !parse_num(&args, &ms)) { puts("Usage: beep [hz] [ms]\n"); return; }
    if (hz < 20 || hz > 20000 || ms < 1 || ms > 5000) {
        puts("Frequency 20-20000 Hz, length 1-5000 ms\n"); return;
    }
    if (!set_sound) { puts("Sound is off (turn it on in Settings, F3)\n"); return; }
    beep((uint32_t)hz, (uint32_t)ms);
}

/* Handles the file-related commands; returns false if cmd is not one. */
static bool run_files(const char *cmd) {
    const char *a;
    char w1[LINE_MAX], w2[LINE_MAX];
    int i, e;

    if (!strcmp(cmd, "ls") || !strcmp(cmd, "dir")) { cmd_ls(); return true; }
    if (!strcmp(cmd, "tune")) { cmd_tune(); return true; }
    if ((a = cmd_args(cmd, "beep"))) { cmd_beep(a); return true; }
    if (!strcmp(cmd, "df")) {
        puts("Files: "); put_dec((uint32_t)fs_count()); putc('/'); put_dec(FS_FILES);
        puts("   Used: "); put_dec(fs_bytes());
        puts(" B of "); put_dec((uint32_t)FS_FILES * FS_SIZE); puts(" B (RAM, lost on reboot)\n");
        return true;
    }
    if ((a = cmd_args(cmd, "edit"))) {          /* opens the Editor tab afterwards */
        if (!next_word(&a, w1, sizeof w1)) {
            if (ed_loaded) pending_tab = TAB_EDITOR;
            else puts("Usage: edit <file>\n");
        } else if (!no_more(a)) puts("Usage: edit <file>\n");
        else if ((e = ed_open(w1)) < 0) fs_err(e, w1);
        else pending_tab = TAB_EDITOR;
        return true;
    }
    if ((a = cmd_args(cmd, "cat"))) {
        if (!next_word(&a, w1, sizeof w1) || !no_more(a)) puts("Usage: cat <file>\n");
        else if ((i = fs_find(w1)) < 0) fs_err(FS_ERR_NOFILE, w1);
        else cmd_cat(i);
        return true;
    }
    if ((a = cmd_args(cmd, "touch"))) {
        if (!next_word(&a, w1, sizeof w1) || !no_more(a)) puts("Usage: touch <file>\n");
        else if (fs_find(w1) < 0 && (e = fs_create(w1)) < 0) fs_err(e, w1);
        return true;
    }
    if ((a = cmd_args(cmd, "rm"))) {
        if (!next_word(&a, w1, sizeof w1) || !no_more(a)) puts("Usage: rm <file>\n");
        else if ((i = fs_find(w1)) < 0) fs_err(FS_ERR_NOFILE, w1);
        else fs_delete(i);
        return true;
    }
    if ((a = cmd_args(cmd, "mv"))) {
        if (!next_word(&a, w1, sizeof w1) || !next_word(&a, w2, sizeof w2) || !no_more(a))
            puts("Usage: mv <old> <new>\n");
        else if ((i = fs_find(w1)) < 0) fs_err(FS_ERR_NOFILE, w1);
        else if ((e = fs_rename(i, w2)) < 0) fs_err(e, w2);
        return true;
    }
    if ((a = cmd_args(cmd, "cp"))) {
        if (!next_word(&a, w1, sizeof w1) || !next_word(&a, w2, sizeof w2) || !no_more(a))
            puts("Usage: cp <from> <to>\n");
        else if ((i = fs_find(w1)) < 0) fs_err(FS_ERR_NOFILE, w1);
        else if ((e = fs_copy(i, w2)) < 0) fs_err(e, w2);
        return true;
    }
    if ((a = cmd_args(cmd, "write")))  { cmd_write(a, false); return true; }
    if ((a = cmd_args(cmd, "append"))) { cmd_write(a, true);  return true; }
    return false;
}

static void run(const char *cmd) {
    if (!*cmd) return;
    if (!strcmp(cmd, "help")) {
        puts("help clear echo <text> rev <text> about ver mem cpu time history\n");
        puts("hex <n>  dec <n>  calc <a> <op> <b>  color <0-15>  bg <0-15>\n");
        puts("Files:  ls cat <f> touch <f> rm <f> mv <a> <b> cp <a> <b> df\n");
        puts("        write <f> <text>  append <f> <text>  edit <f>\n");
        puts("Games:  snake tetris      Sound: beep [hz] [ms]  tune\n");
        puts("reboot shutdown      F1-F6 switch tabs, Up/Down = history\n");
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
    } else if (!strcmp(cmd, "tetris")) {
        uint32_t sc = launch_tetris();
        puts("Tetris finished. Score: "); put_dec(sc);
        puts("  (best "); put_dec(tetris_best); puts(")\n");
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
    } else if (!run_files(cmd)) {
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
    "F1 Shell", "F2 Info", "F3 Settings", "F4 Games", "F5 Files", "F6 Editor"
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
}

static const char *const tab_hints[TAB_COUNT] = {
    " F1-F6: tabs  |  Up/Down: history  |  try 'help', 'ls' or 'edit'",
    " F1-F6 or Tab: switch tabs  |  Esc: back to the Shell",
    " Up/Down: select  |  Left/Right/Enter: change  |  Tab: next tab",
    " Up/Down: select  |  Enter: play  |  Tab: next tab",
    " Enter: edit  |  N: new  |  R: rename  |  D: delete  |  Tab: next",
    " Ctrl+S: save  |  Esc: back to Files  |  F1-F6: switch tabs",
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
    const int vx = 55;                         /* value column, right of the labels */
    vga_clear_area(vx, 3, 22, 7, 0x07);
    int x;
    x = vga_num(vx, 3, t->y, 0x0F);
    vga_put(x, 3, '-', 0x0F); vga_num2(x + 1, 3, t->mo, 0x0F);
    vga_put(x + 3, 3, '-', 0x0F); vga_num2(x + 4, 3, t->d, 0x0F);
    vga_hms(vx, 4, t->h, t->m, t->s, 0x0F);
    vga_text(vx + 9, 4, "(UTC in QEMU)", 0x08);
    vga_hms(vx, 5, uptime_sec / 3600, (uptime_sec / 60) % 60, uptime_sec % 60, 0x0F);
    int nf = fs_count();
    x = vga_num(vx, 6, (uint32_t)nf, 0x0F);
    vga_text(x, 6, nf == 1 ? " file" : " files", 0x0F);
    x = vga_num(vx, 7, (uint32_t)hist_count, 0x0F);
    vga_text(x, 7, hist_count == 1 ? " entry" : " entries", 0x0F);
    vga_num(vx, 8, snake_best, 0x0F);
    vga_num(vx, 9, tetris_best, 0x0F);
}

static void info_draw(void) {
    page_clear();
    draw_box(1, 2, 38, 9, 0x0B, "System");
    draw_box(41, 2, 38, 9, 0x0B, "Clock");
    draw_box(1, 12, 78, 11, 0x0B, "Keys");

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
    vga_text(43, 6, "Files", 0x03);
    vga_text(43, 7, "History", 0x03);
    vga_text(43, 8, "Best snake", 0x03);
    vga_text(43, 9, "Best tetris", 0x03);

    static const char *const keys[][2] = {
        { "F1",  "Shell: type commands (try 'help'), Up/Down recalls history" },
        { "F2",  "Info: this page, refreshed every second" },
        { "F3",  "Settings: colors, clock, serial mirror, sound, snake speed" },
        { "F4",  "Games: Snake and Tetris" },
        { "F5",  "Files: browse, create (N), rename (R), delete (D), edit (Enter)" },
        { "F6",  "Editor: Ctrl+S saves, Esc goes back to Files" },
        { "Tab", "Next tab (outside the Shell and the Editor)" },
        { "Esc", "Back to the Shell (from Info, Settings, Games and Files)" },
    };
    for (int i = 0; i < 8; i++) {
        vga_text(3, 13 + i, keys[i][0], 0x0E);
        vga_text(10, 13 + i, keys[i][1], 0x07);
    }

    struct rtc_time t;
    rtc_read(&t);
    info_update(&t);
}

/* ---------- Settings tab ---------- */
#define SET_COUNT 6

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
        "Mirror output to serial", "Sound (PC speaker)", "Snake speed"
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
        case 4:  name = set_sound ? "On" : "Off"; break;
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
    case 4: set_sound = !set_sound; beep(880, 60); break;   /* chirps when turned on */
    default: set_speed = (set_speed + dir + 3) % 3; break;
    }
    settings_draw();
    draw_status();
}

/* ---------- Games tab ---------- */
#define GAME_COUNT 2
static int games_sel;

static void games_draw(void) {
    static const char *const names[GAME_COUNT] = { "Snake", "Tetris" };
    static const char *const help[GAME_COUNT][3] = {
        { "Steer with the arrow keys or WASD and eat the * to grow.",
          "Don't hit the walls or yourself. Q or Esc quits the game.",
          "Speed can be changed in Settings (F3)." },
        { "Stack falling pieces into full rows to clear them.",
          "Left/Right move, Up rotates, Down drops faster, Space drops at once.",
          "P pauses, Q or Esc quits. It speeds up every 10 lines." },
    };
    page_clear();
    draw_box(1, 2, 78, 16, 0x0B, "Games");
    for (int i = 0; i < GAME_COUNT; i++) {
        uint8_t a = (i == games_sel) ? 0x1F : 0x07;
        vga_clear_area(3, 4 + i, 74, 1, a);
        vga_text(5, 4 + i, names[i], a);
        vga_text(62, 4 + i, "best", a);
        vga_num(67, 4 + i, i == 0 ? snake_best : tetris_best, a);
    }
    for (int i = 0; i < 3; i++) vga_text(5, 8 + i, help[games_sel][i], 0x07);
    vga_text(5, 12, "Scores are kept until you reboot.", 0x08);
    vga_text(5, 14, "Up/Down to choose, Enter to play.", 0x0E);
}

static void ui_switch(int t);

/* ---------- Files tab ---------- */
enum { FM_BROWSE, FM_NEW, FM_RENAME, FM_DELETE };

static int files_sel;                          /* index in the list of used files */
static int files_mode = FM_BROWSE;
static char files_in[FS_NAME];                 /* name being typed */
static size_t files_in_n;
static char files_msg[56];

static void files_set_msg(const char *m) { strcopy(files_msg, m, sizeof files_msg); }

static const char *fs_err_text(int e) {
    switch (e) {
    case FS_ERR_FULL:   return "Filesystem full";
    case FS_ERR_EXISTS: return "A file with that name already exists";
    case FS_ERR_NAME:   return "Bad name (letters, digits . _ -  max 15 chars)";
    default:            return "No such file";
    }
}

static bool name_char(int k) {
    return (k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') ||
           (k >= '0' && k <= '9') || k == '.' || k == '_' || k == '-';
}

static void files_clamp(void) {
    int n = fs_count();
    if (files_sel >= n) files_sel = n - 1;
    if (files_sel < 0) files_sel = 0;
}

static void files_select_name(const char *name) {
    int n = fs_count();
    for (int k = 0; k < n; k++)
        if (!strcmp(fs[fs_nth(k)].name, name)) { files_sel = k; return; }
}

static void files_draw(void) {
    page_clear();
    files_clamp();
    int n = fs_count();
    int sel = n ? fs_nth(files_sel) : -1;

    draw_box(1, 2, 38, 18, 0x0B, "Files");
    draw_box(40, 2, 39, 18, 0x0B, sel >= 0 ? fs[sel].name : "Preview");

    if (!n) {
        vga_text(3, 4, "(no files yet)", 0x08);
        vga_text(3, 6, "Press N to create one.", 0x07);
    }
    for (int k = 0; k < n; k++) {
        int i = fs_nth(k), y = 3 + k;
        uint8_t a = (k == files_sel) ? 0x1F : 0x07;
        vga_clear_area(2, y, 36, 1, a);
        vga_text(3, y, fs[i].name, a);
        if (ed_loaded && !strcmp(ed_name, fs[i].name))
            vga_text(20, y, ed_dirty ? "edit*" : "edit", (k == files_sel) ? 0x1E : 0x0E);
        int nd = 1;
        for (uint32_t v = fs[i].size; v >= 10; v /= 10) nd++;
        vga_num(34 - nd, y, fs[i].size, a);
        vga_text(35, y, "B", a);
    }

    if (sel >= 0) {                            /* preview of the selected file */
        int row = 0, col = 0;
        for (uint16_t p = 0; p < fs[sel].size && row < 16; p++) {
            char c = fs[sel].data[p];
            if (c == '\n') { row++; col = 0; continue; }
            if (col < 37) vga_put(41 + col, 3 + row, (c < 32 || c > 126) ? '.' : c, 0x07);
            col++;
        }
    }

    int x = vga_num(2, 20, (uint32_t)n, 0x08);
    vga_text(x, 20, n == 1 ? " file, " : " files, ", 0x08);
    x = vga_num(x + (n == 1 ? 7 : 8), 20, fs_bytes(), 0x08);
    vga_text(x, 20, " bytes used (RAM only: lost on reboot)", 0x08);

    if (files_msg[0]) vga_text(2, 21, files_msg, 0x0E);

    if (files_mode == FM_NEW || files_mode == FM_RENAME) {
        vga_clear_area(0, 22, VGA_W, 1, 0x30);
        vga_text(1, 22, files_mode == FM_NEW ? "New file name: " : "Rename to: ", 0x30);
        int px = files_mode == FM_NEW ? 16 : 12;
        vga_text(px, 22, files_in, 0x3F);
        vga_put(px + (int)files_in_n, 22, '_', 0x3F);
        vga_text(50, 22, "Enter: ok   Esc: cancel", 0x30);
    } else if (files_mode == FM_DELETE && sel >= 0) {
        vga_clear_area(0, 22, VGA_W, 1, 0x40);
        vga_text(1, 22, "Delete ", 0x4F);
        vga_text(8, 22, fs[sel].name, 0x4F);
        vga_text(9 + (int)strlen(fs[sel].name), 22, "?  y = yes, any other key = no", 0x4F);
    }
}

/* Returns true when the key was used; false lets the generic tab keys run. */
static bool files_key(int k) {
    int n = fs_count();

    if (files_mode == FM_BROWSE) {
        files_set_msg("");
        if (k == KEY_UP)   { if (files_sel > 0) files_sel--; files_draw(); return true; }
        if (k == KEY_DOWN) { if (files_sel + 1 < n) files_sel++; files_draw(); return true; }
        if (k == 'n' || k == 'N') {
            files_mode = FM_NEW; files_in_n = 0; files_in[0] = 0;
            files_draw(); return true;
        }
        if (!n) return false;
        int i = fs_nth(files_sel);
        if (k == '\n' || k == 'e' || k == 'E') {
            int e = ed_open(fs[i].name);
            if (e < 0) { files_set_msg(fs_err_text(e)); files_draw(); }
            else ui_switch(TAB_EDITOR);        /* it redraws the screen itself */
            return true;
        }
        if (k == 'r' || k == 'R') {
            files_mode = FM_RENAME;
            strcopy(files_in, fs[i].name, FS_NAME);
            files_in_n = strlen(files_in);
            files_draw(); return true;
        }
        if (k == 'd' || k == 'D' || k == KEY_DEL) {
            files_mode = FM_DELETE; files_draw(); return true;
        }
        return false;
    }

    if (files_mode == FM_DELETE) {
        if (k == 'y' || k == 'Y') {
            int i = fs_nth(files_sel);
            if (i >= 0) { fs_delete(i); files_set_msg("Deleted"); }
        } else files_set_msg("Cancelled");
        files_mode = FM_BROWSE;
        files_draw(); return true;
    }

    /* FM_NEW / FM_RENAME: line input */
    if (k == 27) {
        files_mode = FM_BROWSE; files_set_msg("Cancelled");
    } else if (k == '\b') {
        if (files_in_n) files_in[--files_in_n] = 0;
    } else if (k == '\n') {
        int e;
        if (files_mode == FM_NEW) {
            e = fs_create(files_in);
            if (e >= 0) files_set_msg("Created");
        } else {
            int i = fs_nth(files_sel);
            char old[FS_NAME];
            strcopy(old, fs[i].name, FS_NAME);
            e = fs_rename(i, files_in);
            if (e >= 0) {
                if (ed_loaded && !strcmp(ed_name, old)) strcopy(ed_name, files_in, FS_NAME);
                files_set_msg("Renamed");
            }
        }
        if (e < 0) files_set_msg(fs_err_text(e));      /* stay in the prompt */
        else { files_mode = FM_BROWSE; files_select_name(files_in); }
    } else if (name_char(k) && files_in_n + 1 < FS_NAME) {
        files_in[files_in_n++] = (char)k;
        files_in[files_in_n] = 0;
        files_set_msg("");
    }
    files_draw();
    return true;
}

/* ---------- Editor tab ---------- */
#define ED_ROWS   21                           /* text rows 2..22 */
#define ED_GUTTER 5                            /* line-number column width */
#define ED_COLS   (VGA_W - ED_GUTTER)

/* Moves the blinking hardware cursor without touching the shell's cur_x/cur_y. */
static void hw_cursor(int x, int y) {
    uint16_t pos = (uint16_t)(y * VGA_W + x);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}

static void ed_draw(void) {
    page_clear();
    if (!ed_loaded) {
        vga_center(10, "No file is open.", 0x0F);
        vga_center(12, "Open one from the Files tab (F5), or type: edit <name>", 0x07);
        cursor_hide();
        return;
    }

    /* keep the cursor visible: vertical and horizontal scrolling */
    size_t line = ed_cur_line(), col = ed_cur_col();
    if (line < ed_top) ed_top = line;
    if (line >= ed_top + ED_ROWS) ed_top = line - ED_ROWS + 1;
    if (col < ed_left_col) ed_left_col = col;
    if (col >= ed_left_col + ED_COLS) ed_left_col = col - ED_COLS + 1;

    /* header bar */
    vga_clear_area(0, 1, VGA_W, 1, 0x30);
    vga_text(1, 1, ed_name, 0x3F);
    if (ed_dirty) vga_text(2 + (int)strlen(ed_name), 1, "[modified]", 0x3E);
    vga_text(40, 1, "Ln", 0x30);   vga_num(43, 1, (uint32_t)line + 1, 0x3F);
    vga_text(52, 1, "Col", 0x30);  vga_num(56, 1, (uint32_t)col + 1, 0x3F);
    int x = vga_num(64, 1, (uint32_t)ed_len, 0x3F);
    vga_text(x, 1, "/", 0x30);
    x = vga_num(x + 1, 1, FS_SIZE, 0x30);
    vga_text(x, 1, " B", 0x30);

    /* text area */
    size_t pos = 0;
    for (size_t l = 0; l < ed_top; l++) pos = ed_line_end(pos) + 1;
    for (int r = 0; r < ED_ROWS; r++) {
        int y = 2 + r;
        if (pos > ed_len) {                    /* past the end of the text */
            vga_put(0, y, '~', 0x08);
            continue;
        }
        uint32_t no = (uint32_t)(ed_top + (size_t)r + 1);
        int nd = 1;
        for (uint32_t v = no; v >= 10; v /= 10) nd++;
        vga_num(4 - nd, y, no, 0x08);
        size_t le = ed_line_end(pos);
        for (int c = 0; c < ED_COLS; c++) {
            size_t p = pos + ed_left_col + (size_t)c;
            if (p >= le) break;
            char ch = ed_buf[p];
            vga_put(ED_GUTTER + c, y, (ch < 32 || ch > 126) ? '.' : ch, 0x0F);
        }
        pos = le + 1;
    }

    /* bottom bar: message, or key hints */
    vga_clear_area(0, 23, VGA_W, 1, 0x30);
    if (ed_msg[0]) vga_text(1, 23, ed_msg, 0x3E);
    else vga_text(1, 23, "Ctrl+S save   Esc files   Tab = 4 spaces   Home/End  PgUp/PgDn", 0x30);

    hw_cursor(ED_GUTTER + (int)(col - ed_left_col), 2 + (int)(line - ed_top));
}

static void editor_key(int k) {
    ed_msg[0] = 0;
    if (ed_loaded) {
        switch (k) {
        case KEY_CTRL('s'): ed_save(); break;
        case KEY_UP:    ed_up(); break;
        case KEY_DOWN:  ed_down(); break;
        case KEY_LEFT:  ed_left(); break;
        case KEY_RIGHT: ed_right(); break;
        case KEY_HOME:  ed_home(); break;
        case KEY_END:   ed_end(); break;
        case KEY_PGUP:  for (int i = 0; i < ED_ROWS - 1; i++) ed_up(); break;
        case KEY_PGDN:  for (int i = 0; i < ED_ROWS - 1; i++) ed_down(); break;
        case KEY_DEL:   ed_delete(); break;
        case '\b':      ed_backspace(); break;
        case '\n':
            if (!ed_insert('\n')) ed_set_msg("File is full (2048 bytes)");
            break;
        case '\t':
            for (int i = 0; i < 4; i++)
                if (!ed_insert(' ')) { ed_set_msg("File is full (2048 bytes)"); break; }
            break;
        default:
            if (k >= 32 && k < 127 && !ed_insert((char)k))
                ed_set_msg("File is full (2048 bytes)");
            break;
        }
    }
    ed_draw();
}

/* ---------- tab switching and the main loop ---------- */
static void ui_show_page(void) {
    switch (cur_tab) {
    case TAB_SHELL:    shell_enter(); break;
    case TAB_INFO:     info_draw(); break;
    case TAB_SETTINGS: settings_draw(); break;
    case TAB_FILES:    files_draw(); break;
    case TAB_EDITOR:   ed_draw(); break;
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
    puts("F1-F6 switch tabs. Type 'help' for commands.\n\n");
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
    if (k >= KEY_F1 && k <= KEY_F6) { ui_switch(k - KEY_F1); return; }
    if (cur_tab == TAB_SHELL) {
        shell_key(k);
        if (pending_tab >= 0) {                /* e.g. the 'edit' command */
            int t = pending_tab;
            pending_tab = -1;
            ui_switch(t);
        }
        return;
    }
    if (cur_tab == TAB_EDITOR) {
        if (k == 27) ui_switch(TAB_FILES);
        else editor_key(k);
        return;
    }
    if (cur_tab == TAB_FILES && files_key(k)) return;
    if (k == 27) { ui_switch(TAB_SHELL); return; }
    if (k == '\t') { ui_switch((cur_tab + 1) % TAB_COUNT); return; }

    if (cur_tab == TAB_SETTINGS) {
        if (k == KEY_UP)   { set_sel = (set_sel + SET_COUNT - 1) % SET_COUNT; settings_draw(); }
        else if (k == KEY_DOWN) { set_sel = (set_sel + 1) % SET_COUNT; settings_draw(); }
        else if (k == KEY_LEFT) settings_change(-1);
        else if (k == KEY_RIGHT || k == '\n') settings_change(1);
        return;
    }
    if (cur_tab == TAB_GAMES) {
        if (k == KEY_UP)   { games_sel = (games_sel + GAME_COUNT - 1) % GAME_COUNT; games_draw(); return; }
        if (k == KEY_DOWN) { games_sel = (games_sel + 1) % GAME_COUNT; games_draw(); return; }
        if (k == '\n') {
            if (games_sel == 0) launch_snake(); else launch_tetris();
            games_draw();                      /* refresh the best score */
            return;
        }
    }
    if (k == KEY_LEFT)  ui_switch((cur_tab + TAB_COUNT - 1) % TAB_COUNT);
    if (k == KEY_RIGHT) ui_switch((cur_tab + 1) % TAB_COUNT);
}

void kernel_main(uint32_t magic, uint32_t *mbi) {
    serial_init();
    pit_init();
    if (magic == 0x2BADB002 && (mbi[0] & 1))   /* flags bit 0: mem_* valid */
        mem_upper_kb = mbi[2];

    fs_seed("readme.txt",
            "Welcome to MyOS!\n"
            "Files live in RAM and vanish on reboot.\n"
            "Try: ls, cat, write, append, edit, mv, cp, rm, df\n");
    fs_seed("notes.txt", "A scratch file. Open it in the Editor (F6).\n");

    splash();
    ui_init();

    for (;;) {
        ui_tick();
        int k = getkey_nb();
        if (k < 0) { __asm__ volatile("pause"); continue; }
        ui_key(k);
    }
}
