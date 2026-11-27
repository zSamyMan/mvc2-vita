/*
 * config_menu.c  The PS Vita's settings menu, before the game: env.txt's settings chosen on two screens drawn like
 * the later ports' option screens. GAME SETUP: ASPECT RATIO (and ORIGINAL's BORDERS beside it), SECRET FACTOR, RIGHT
 * ANALOG STICK, PRESET (the clocks), DEBUG OPTIONS. CONTROL TYPE: CONTROL_SCHEME's VITA, PS2 or GRIP over its picture, and "don't show this again".
 *
 * Shown at the start unless env.txt has CONFIG_MENU=OFF; with it, only while SELECT is held as the game starts. Its
 * pieces: the Dreamcast disc's option screen textures (DM0CTEX.BIN, by DM0CPOL.BIN's headers: the marble behind, the
 * pills), its 24-dot roman font (S24RM04.BIN) for the words, and the port's own pictures (app0:assets: the schemes'
 * pictures and the check box, ports/tools/vita_assets.py). The title's oval and the buttons' glyphs are drawn.
 *
 * The menu runs here, on the game's thread, before dc_init; the render thread draws it (rd_vgl_menu) from the
 * state below, which only this thread changes.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr.h>

#include "naomi_host.h"
#include "rd_vgl.h"
#include "vita_sys.h"

void input_scheme(const char* scheme, int right_stick);     /* input_sdl.c */

/* ---- the settings ---- */

enum { O_ASPECT, O_SECRET, O_STICK, O_PRESET, O_DEBUG, OPTIONS };
enum { SCHEME_VITA, SCHEME_PS2, SCHEME_GRIP, SCHEMES };

static const char* const aspect_names[] = { "WIDE", "ORIGINAL", "STRETCHED" };
static const char* const scheme_names[] = { "VITA", "PS2", "GRIP" };
static const char* const preset_names[] = { "STOCK", "BALANCED", "PERFORMANCE" };
static const char* const border_names[] = { "NO BORDERS", "ARTWORK 1", "ARTWORK 2" };
static const char* const border_env[] = { "NONE", "ARTWORK1", "ARTWORK2" };
static const char* const border_help[] = { "Black at the sides of the 4:3 picture.",
                                           "The first artwork at the sides of the 4:3 picture.",
                                           "The second artwork at the sides of the 4:3 picture." };

static const struct {
    const char* label;
    int count;
    const char* values[3];
    const char* help[3];
} options[OPTIONS] = {
    { "ASPECT RATIO", 3, { "WIDE", "ORIGINAL", "STRETCHED" },
      { "The picture fills the whole PS Vita screen; the stages are widened out to its sides.",
        "The arcade's 4:3 picture in the middle of the screen; the box beside it sets what fills its sides.",
        "The 4:3 picture stretched across the whole screen." } },
    { "SECRET FACTOR", 2, { "LOCKED", "UNLOCKED" },
      { "The hidden characters and extras are won in play, as on the Dreamcast.",
        "Every hidden character, colour and extra is open from the start." } },
    { "RIGHT ANALOG STICK", 2, { "OFF", "ON" },
      { "The right stick does nothing, in every control type.",
        "The right stick does its control type's button combinations (see the next screen)." } },
    { "PRESET", 3, { "STOCK", "BALANCED", "PERFORMANCE" },
      { "CPU 333 MHz, GPU 166 MHz: the PS Vita's own clocks, the least heat and battery. The heaviest hyper combos "
        "may slow down.",
        "CPU 333 MHz, GPU 222 MHz: the default, a steady 60 frames a second.",
        "CPU 444 MHz, GPU 222 MHz: more heat and battery used." } },
    { "DEBUG OPTIONS", 2, { "OFF", "ON" },
      { "Every testing tool off. Takes effect the next time the game starts.",
        "The log, the pictures and the debug menu (L + DOWN + SELECT). Takes effect the next time the game starts." } },
};

static const char* const scheme_help[SCHEMES] = {
    "The PS Vita's own buttons: four attacks on the face buttons, the assists on L and R.",
    "As on the PlayStation 2: R and the rear panel's right tag your partners in, its left calls assist 2.",
    "For grips with rear buttons: the rear panel does the punch and kick pairs, the right stick tags partners in.",
};

/* the menu's state: changed on the game's thread, read by the render thread's drawing */
static volatile struct {
    int page;                   /* 0 GAME SETUP, 1 CONTROL TYPE */
    int row;                    /* page 0: an option or NEXT (OPTIONS); page 1: P1_* */
    int side;                   /* page 0's ASPECT RATIO row: its BORDERS box chosen (ORIGINAL only) */
    int value[OPTIONS];
    int borders;                /* ORIGINAL's sides: 0 black, 1 the artwork */
    int scheme, hide;
    unsigned frame;
} m;

enum { P1_TYPE, P1_HIDE, P1_BACK, P1_START, P1_ROWS };

static void from_env(void) {
    const char* v;
    int i;
    m.value[O_ASPECT] = 0;
    if ((v = getenv("ASPECT_RATIO")) != NULL) {
        for (i = 0; i < 3; i++) {
            if (strcasecmp(v, aspect_names[i]) == 0) {
                m.value[O_ASPECT] = i;
            }
        }
    }
    v = getenv("SECRET_FACTOR");
    m.value[O_SECRET] = v != NULL ? strcasecmp(v, "UNLOCKED") == 0 : host_unlock;
    v = getenv("RIGHT_STICK");
    m.value[O_STICK] = v == NULL || strcasecmp(v, "OFF") != 0;
    m.value[O_PRESET] = 1;
    if ((v = getenv("PRESET")) != NULL) {
        for (i = 0; i < 3; i++) {
            if (strcasecmp(v, preset_names[i]) == 0) {
                m.value[O_PRESET] = i;
            }
        }
    }
    v = getenv("BORDERS");
    m.borders = 1;     /* ARTWORK1, also the "ARTWORK" of before */
    if (v != NULL) {
        m.borders = strcasecmp(v, "NONE") == 0 ? 0 : strcasecmp(v, "ARTWORK2") == 0 ? 2 : 1;
    }
    m.value[O_DEBUG] = vita_debug != 0;
    m.scheme = SCHEME_VITA;
    if ((v = getenv("CONTROL_SCHEME")) != NULL) {
        for (i = 0; i < SCHEMES; i++) {
            if (strcasecmp(v, scheme_names[i]) == 0) {
                m.scheme = i;
            }
        }
    }
    v = getenv("CONFIG_MENU");
    m.hide = v != NULL && strcasecmp(v, "OFF") == 0;
}

/* env.txt rewritten: the menu's lines first, then any other line it had (the test runs' own) as it was */
#define MANAGED 8
static const char* const managed[MANAGED] = { "ASPECT_RATIO", "BORDERS", "CONTROL_SCHEME", "RIGHT_STICK",
                                              "SECRET_FACTOR", "PRESET", "DEBUG_OPTIONS", "CONFIG_MENU" };

static void to_env(void) {
    char vals[MANAGED][16];
    static char kept[32][160];
    int nkept = 0, i;
    char line[160];
    FILE* f = fopen(VITA_DATA_DIR "env.txt", "r");
    while (f != NULL && fgets(line, sizeof line, f) != NULL && nkept < 32) {
        size_t n = strcspn(line, "=\r\n");
        int mine = 0;
        line[strcspn(line, "\r\n")] = 0;
        for (i = 0; i < MANAGED; i++) {
            mine |= strlen(managed[i]) == n && strncmp(line, managed[i], n) == 0;
        }
        mine |= n == 6 && strncmp(line, "CLOCKS", 6) == 0;     /* PRESET's before it, dropped */
        if (!mine && line[0] != 0) {
            snprintf(kept[nkept++], sizeof kept[0], "%s", line);
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    snprintf(vals[0], sizeof vals[0], "%s", aspect_names[m.value[O_ASPECT]]);
    snprintf(vals[1], sizeof vals[1], "%s", border_env[m.borders]);
    snprintf(vals[2], sizeof vals[2], "%s", scheme_names[m.scheme]);
    snprintf(vals[3], sizeof vals[3], "%s", m.value[O_STICK] ? "ON" : "OFF");
    snprintf(vals[4], sizeof vals[4], "%s", m.value[O_SECRET] ? "UNLOCKED" : "LOCKED");
    snprintf(vals[5], sizeof vals[5], "%s", preset_names[m.value[O_PRESET]]);
    snprintf(vals[6], sizeof vals[6], "%s", m.value[O_DEBUG] ? "1" : "0");
    snprintf(vals[7], sizeof vals[7], "%s", m.hide ? "OFF" : "ON");
    for (i = 0; i < MANAGED; i++) {
        setenv(managed[i], vals[i], 1);
    }
    f = fopen(VITA_DATA_DIR "env.txt", "w");
    if (f == NULL) {
        host_log("menu: env.txt could not be written");
        return;
    }
    for (i = 0; i < MANAGED; i++) {
        fprintf(f, "%s=%s\n", managed[i], vals[i]);
    }
    for (i = 0; i < nkept; i++) {
        fprintf(f, "%s\n", kept[i]);
    }
    fclose(f);
    host_log("menu: env.txt written: ASPECT_RATIO=%s BORDERS=%s CONTROL_SCHEME=%s RIGHT_STICK=%s SECRET_FACTOR=%s "
             "PRESET=%s DEBUG_OPTIONS=%s CONFIG_MENU=%s", vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], vals[6],
             vals[7]);
}

/* ---- its pictures: read here, made into textures by the render thread at its first frame ---- */

typedef struct {
    uint8_t* px;                /* RGBA, rows top down; freed once a texture */
    int w, h;
    unsigned tex;
} PIC;

enum { T_MARBLE, T_PILLS, T_FONT, T_ICONS, T_CTL0, T_CTL1, T_CTL2, PICS };
static PIC pics[PICS];

static uint8_t* read_whole(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    uint8_t* d = NULL;
    long size;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        d = malloc((size_t)size);
        if (d != NULL && fread(d, 1, (size_t)size, f) != (size_t)size) {
            free(d);
            d = NULL;
        }
        *n = (size_t)size;
    }
    fclose(f);
    return d;
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* a 16-bit texel: 0 ARGB 1555, 1 RGB 565, 2 ARGB 4444 */
static void texel(uint16_t v, int fmt, uint8_t* o) {
    if (fmt == 0) {
        o[0] = (uint8_t)((v >> 10 & 31) * 255 / 31), o[1] = (uint8_t)((v >> 5 & 31) * 255 / 31);
        o[2] = (uint8_t)((v & 31) * 255 / 31), o[3] = v & 0x8000 ? 255 : 0;
    } else if (fmt == 1) {
        o[0] = (uint8_t)((v >> 11 & 31) * 255 / 31), o[1] = (uint8_t)((v >> 5 & 63) * 255 / 63);
        o[2] = (uint8_t)((v & 31) * 255 / 31), o[3] = 255;
    } else {
        o[0] = (uint8_t)((v >> 8 & 15) * 17), o[1] = (uint8_t)((v >> 4 & 15) * 17), o[2] = (uint8_t)((v & 15) * 17);
        o[3] = (uint8_t)((v >> 12 & 15) * 17);
    }
}

/* DM0CTEX.BIN's texture idx (a square twiddled one), by DM0CPOL.BIN's header for it; the file's rows run bottom up.
   key_black: its black made clear (for one drawn on black) */
static int dm_texture(const uint8_t* pol, size_t pn, const uint8_t* tex, size_t tn, int idx, int key_black, PIC* out) {
    uint32_t built, thd, type, addr, off;
    const uint8_t* e;
    int w, h, kind, fmt, x, y;
    if (pol == NULL || tex == NULL || pn < 32) {
        return 0;
    }
    built = rd32(pol) - 16;
    thd = rd32(pol + 8) - built;
    if (thd + (uint32_t)(idx + 1) * 16 > pn) {
        return 0;
    }
    e = pol + thd + idx * 16;
    w = (int16_t)(e[0] | e[1] << 8), h = (int16_t)(e[2] | e[3] << 8);
    type = rd32(e + 4), addr = rd32(e + 8);
    kind = type >> 8 & 255, fmt = type & 255;
    off = addr - 0x0cc00000u;
    if (w != h || w <= 0 || w > 1024 || (w & (w - 1)) != 0 || kind != 1 || fmt > 2 || off + (uint32_t)w * h * 2 > tn) {
        host_log("menu: DM0C texture %d not one read here (%dx%d, kind %d, format %d)", idx, w, h, kind, fmt);
        return 0;
    }
    out->px = malloc((size_t)w * h * 4);
    if (out->px == NULL) {
        return 0;
    }
    out->w = w, out->h = h;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t i = 0;
            int b;
            const uint8_t* s;
            uint8_t* o = out->px + ((size_t)(h - 1 - y) * w + x) * 4;
            for (b = 0; (1 << b) < w; b++) {
                i |= (uint32_t)(y >> b & 1) << (2 * b) | (uint32_t)(x >> b & 1) << (2 * b + 1);
            }
            s = tex + off + i * 2;
            texel((uint16_t)(s[0] | s[1] << 8), fmt, o);
            if (key_black && o[0] + o[1] + o[2] < 24) {
                o[3] = 0;
            }
        }
    }
    return 1;
}

/* the font: S24RM04.BIN's glyphs (24 rows of 6 bytes, four 2-bit shades a byte, after 16 bytes of their own, in
   JIS order) for ASCII 32 to 126, 10 to a row of 25-pixel cells, white with their shade as alpha; each one's inked
   columns kept for its width */
#define CELL 25
#define GLYPH 24
static uint8_t ink_x0[95], ink_x1[95];

static uint16_t ascii_jis(int c) {
    static const char punct[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    static const uint16_t punct_jis[] = { 0x212A, 0x2149, 0x2174, 0x2170, 0x2173, 0x2175, 0x2147, 0x214A, 0x214B,
                                          0x2176, 0x215C, 0x2124, 0x215D, 0x2125, 0x213F, 0x2127, 0x2128, 0x2163,
                                          0x2161, 0x2164, 0x2129, 0x2177, 0x214E, 0x2140, 0x214F, 0x2130, 0x2132,
                                          0x212E, 0x2150, 0x2143, 0x2151, 0x2141 };
    const char* p;
    if (c >= '0' && c <= '9') {
        return (uint16_t)(0x2330 + c - '0');
    }
    if (c >= 'A' && c <= 'Z') {
        return (uint16_t)(0x2341 + c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
        return (uint16_t)(0x2361 + c - 'a');
    }
    p = strchr(punct, c);
    return p != NULL && c != 0 ? punct_jis[p - punct] : 0x2121;
}

static int font_make(const uint8_t* d, size_t n) {
    const int size = GLYPH * 6 + 16;
    int c;
    PIC* p = &pics[T_FONT];
    if (d == NULL) {
        return 0;
    }
    p->w = p->h = 256;
    p->px = calloc(256 * 256, 4);
    if (p->px == NULL) {
        return 0;
    }
    for (c = 32; c < 127; c++) {
        uint16_t jis = ascii_jis(c);
        size_t at = (size_t)(((jis >> 8) - 0x21) * 94 + (jis & 255) - 0x21) * size + 16;
        int cx = (c - 32) % 10 * CELL, cy = (c - 32) / 10 * CELL, x, y, lo = GLYPH, hi = 0;
        if (at + GLYPH * 6 > n) {
            continue;
        }
        for (y = 0; y < GLYPH; y++) {
            for (x = 0; x < GLYPH; x++) {
                int v = d[at + y * 6 + x / 4] >> (6 - 2 * (x & 3)) & 3;
                uint8_t* o = p->px + ((size_t)(cy + y) * 256 + cx + x) * 4;
                o[0] = o[1] = o[2] = 255;
                o[3] = (uint8_t)(v * 85);
                if (v != 0) {
                    lo = x < lo ? x : lo;
                    hi = x + 1 > hi ? x + 1 : hi;
                }
            }
        }
        ink_x0[c - 32] = (uint8_t)(lo < hi ? lo : 0);
        ink_x1[c - 32] = (uint8_t)(lo < hi ? hi : 0);
    }
    return 1;
}

static int asset_rgba(const char* name, int w, int h, PIC* p) {
    char path[96];
    size_t n = 0;
    snprintf(path, sizeof path, "app0:assets/%s.rgba", name);
    p->px = read_whole(path, &n);
    if (p->px != NULL && n != (size_t)w * h * 4) {
        free(p->px);
        p->px = NULL;
    }
    if (p->px == NULL) {
        host_log("menu: %s not read", path);
        return 0;
    }
    p->w = w, p->h = h;
    return 1;
}

static void pics_load(const char* disc) {
    char path[1400];
    size_t pn = 0, tn = 0, fn = 0;
    int buckets = dc_disc_buckets(disc);
    uint8_t *pol, *tex, *font;
    dc_disc_path(path, sizeof path, disc, "DM0CPOL.BIN", buckets);
    pol = read_whole(path, &pn);
    dc_disc_path(path, sizeof path, disc, "DM0CTEX.BIN", buckets);
    tex = read_whole(path, &tn);
    dm_texture(pol, pn, tex, tn, 5, 0, &pics[T_MARBLE]);
    dm_texture(pol, pn, tex, tn, 14, 0, &pics[T_PILLS]);
    free(pol);
    free(tex);
    dc_disc_path(path, sizeof path, disc, "S24RM04.BIN", buckets);
    font = read_whole(path, &fn);
    if (!font_make(font, fn)) {
        host_log("menu: the font (%s) not read", path);
    }
    free(font);
    asset_rgba("menu_icons", 64, 32, &pics[T_ICONS]);
    asset_rgba("ctl_vita", 960, 544, &pics[T_CTL0 + SCHEME_VITA]);
    asset_rgba("ctl_ps2", 960, 544, &pics[T_CTL0 + SCHEME_PS2]);
    asset_rgba("ctl_grip", 960, 544, &pics[T_CTL0 + SCHEME_GRIP]);
}

static void pics_free(void) {
    int i;
    for (i = 0; i < PICS; i++) {
        free(pics[i].px);
        pics[i].px = NULL;
        pics[i].tex = 0;
    }
}

/* ---- the drawing, on the render thread ---- */

#define RGBA(r, g, b, a) ((unsigned)(r) | (unsigned)(g) << 8 | (unsigned)(b) << 16 | (unsigned)(a) << 24)

static float rot_c = 1.0f, rot_s = 0.0f, rot_x, rot_y;     /* a turn about rot_x, rot_y for what follows */

static void turn(float degrees, float x, float y) {
    float r = degrees * 3.14159265f / 180.0f;
    rot_c = __builtin_cosf(r), rot_s = __builtin_sinf(r), rot_x = x, rot_y = y;
}

static void put_vtx(RD2D_VTX* v, float x, float y, float u, float t, unsigned c) {
    float dx = x - rot_x, dy = y - rot_y;
    v->x = rot_x + dx * rot_c - dy * rot_s;
    v->y = rot_y + dx * rot_s + dy * rot_c;
    v->u = u, v->v = t, v->rgba = c;
}

/* a quad into v (6 vertices): the texture's pixels sx0..sx1, sy0..sy1 of a w by h one on the screen's x0..x1,
   y0..y1, coloured top to bottom */
static int quad_at(RD2D_VTX* v, int pic, float x0, float y0, float x1, float y1, float sx0, float sy0, float sx1,
                   float sy1, unsigned top, unsigned bottom) {
    float w = pic >= 0 ? (float)pics[pic].w : 1.0f, h = pic >= 0 ? (float)pics[pic].h : 1.0f;
    float u0 = sx0 / w, u1 = sx1 / w, t0 = sy0 / h, t1 = sy1 / h;
    put_vtx(&v[0], x0, y0, u0, t0, top);
    put_vtx(&v[1], x1, y0, u1, t0, top);
    put_vtx(&v[2], x0, y1, u0, t1, bottom);
    put_vtx(&v[3], x0, y1, u0, t1, bottom);
    put_vtx(&v[4], x1, y0, u1, t0, top);
    put_vtx(&v[5], x1, y1, u1, t1, bottom);
    return 6;
}

static unsigned tex_of(int pic) {
    return pic >= 0 ? pics[pic].tex : 0;
}

static void quad(int pic, float x0, float y0, float x1, float y1, float sx0, float sy0, float sx1, float sy1,
                 unsigned top, unsigned bottom) {
    RD2D_VTX v[6];
    if (pic >= 0 && pics[pic].tex == 0) {
        return;
    }
    rd_vgl_2d_draw(tex_of(pic), v, quad_at(v, pic, x0, y0, x1, y1, sx0, sy0, sx1, sy1, top, bottom));
}

static void fill(float x0, float y0, float x1, float y1, unsigned top, unsigned bottom) {
    quad(-1, x0, y0, x1, y1, 0, 0, 1, 1, top, bottom);
}

static void frame_rect(float x0, float y0, float x1, float y1, float t, unsigned c) {
    fill(x0, y0, x1, y0 + t, c, c);
    fill(x0, y1 - t, x1, y1, c, c);
    fill(x0, y0, x0 + t, y1, c, c);
    fill(x1 - t, y0, x1, y1, c, c);
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, unsigned c) {
    RD2D_VTX v[3];
    put_vtx(&v[0], x0, y0, 0, 0, c);
    put_vtx(&v[1], x1, y1, 0, 0, c);
    put_vtx(&v[2], x2, y2, 0, 0, c);
    rd_vgl_2d_draw(0, v, 3);
}

/* a pill from the pills' texture (its rect sx, sy, sw, sh), its round ends kept round and its middle stretched */
typedef struct {
    float x, y, w, h;
} RECT;
static const RECT PILL_SLATE = { 102, 68, 94, 33 }, PILL_LIME = { 2, 187, 240, 29 }, GLOW_GREEN = { 9, 112, 102, 18 };
static const RECT ICON_BOX = { 0, 0, 30, 30 }, ICON_TICK = { 32, 0, 25, 22 };

static void pill(const RECT* r, float x0, float y0, float x1, float y1, unsigned c) {
    float cap = r->h * 0.5f, dcap = (y1 - y0) * 0.5f;
    float sx0 = r->x + 0.5f, sy0 = r->y + 0.5f, sx1 = r->x + r->w - 0.5f, sy1 = r->y + r->h - 0.5f;
    RD2D_VTX v[18];
    int n = 0;
    if (pics[T_PILLS].tex == 0) {
        fill(x0, y0, x1, y1, c, c);
        return;
    }
    n += quad_at(v + n, T_PILLS, x0, y0, x0 + dcap, y1, sx0, sy0, r->x + cap, sy1, c, c);
    n += quad_at(v + n, T_PILLS, x0 + dcap, y0, x1 - dcap, y1, r->x + cap, sy0, r->x + r->w - cap, sy1, c, c);
    n += quad_at(v + n, T_PILLS, x1 - dcap, y0, x1, y1, r->x + r->w - cap, sy0, sx1, sy1, c, c);
    rd_vgl_2d_draw(pics[T_PILLS].tex, v, n);
}

static void icon(const RECT* r, float x, float y, float scale, unsigned c) {
    quad(T_ICONS, x, y, x + r->w * scale, y + r->h * scale, r->x, r->y, r->x + r->w, r->y + r->h, c, c);
}

/* the words */
static float text_width(const char* s, float scale) {
    float w = 0;
    for (; *s != 0; s++) {
        int c = (unsigned char)*s;
        if (c < 33 || c > 126 || ink_x1[c - 32] == 0) {
            w += 7.0f * scale;
        } else {
            w += (float)(ink_x1[c - 32] - ink_x0[c - 32] + 2) * scale;
        }
    }
    return w > 0 ? w - 2.0f * scale : 0;
}

/* one pass of s at x, y (its top), every glyph moved by dx, dy */
static void text_pass(const char* s, float x, float y, float scale, float dx, float dy, unsigned top, unsigned bottom) {
    static RD2D_VTX v[1536];
    int n = 0;
    for (; *s != 0 && n + 6 <= (int)(sizeof v / sizeof v[0]); s++) {
        int c = (unsigned char)*s;
        float cx, cy, w;
        if (c < 33 || c > 126 || ink_x1[c - 32] == 0) {
            x += 7.0f * scale;
            continue;
        }
        cx = (float)((c - 32) % 10 * CELL), cy = (float)((c - 32) / 10 * CELL);
        w = (float)(ink_x1[c - 32] - ink_x0[c - 32]);
        n += quad_at(v + n, T_FONT, x + dx, y + dy, x + dx + w * scale, y + dy + GLYPH * scale, cx + ink_x0[c - 32], cy,
                     cx + ink_x1[c - 32], cy + GLYPH, top, bottom);
        x += (w + 2.0f) * scale;
    }
    rd_vgl_2d_draw(pics[T_FONT].tex, v, n);
}

enum { LEFT, CENTRE, RIGHT };

/* s with an outline of edge pixels round it (none for 0), filled top to bottom; bold doubles its strokes. Its top
   at y, its x by align */
static void text(const char* s, float x, float y, float scale, int align, unsigned top, unsigned bottom, unsigned edge,
                 float e, int bold) {
    static const float ring[8][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
    int i;
    if (pics[T_FONT].tex == 0) {
        return;
    }
    if (align != LEFT) {
        x -= text_width(s, scale) * (align == CENTRE ? 0.5f : 1.0f);
    }
    if (e > 0) {
        for (i = 0; i < 8; i++) {
            text_pass(s, x, y, scale, ring[i][0] * e + (bold ? 0.5f : 0), ring[i][1] * e, edge, edge);
        }
    }
    text_pass(s, x, y, scale, 0, 0, top, bottom);
    if (bold) {
        text_pass(s, x, y, scale, 1.0f, 0, top, bottom);
    }
}

/* s in lines no wider than width, from y down; the lines' count */
static int text_wrapped(const char* s, float x, float y, float width, float scale, float step, unsigned c, unsigned edge,
                        float e) {
    char line[160];
    int lines = 0;
    while (*s != 0) {
        int n = 0, cut = 0;
        while (s[n] != 0 && n < (int)sizeof line - 1) {
            memcpy(line, s, (size_t)n + 1);
            line[n + 1] = 0;
            if (text_width(line, scale) > width) {
                break;
            }
            if (s[n] == ' ') {
                cut = n;
            }
            n++;
        }
        if (s[n] != 0 && cut > 0) {
            n = cut;
        }
        memcpy(line, s, (size_t)n);
        line[n] = 0;
        text(line, x, y + lines * step, scale, LEFT, c, c, edge, e, 0);
        lines++;
        s += n;
        while (*s == ' ') {
            s++;
        }
    }
    return lines;
}

/* a chosen value's yellow arrows: the left one's point at l, the right one's at r, their middle at cy */
static void arrows(float l, float r, float cy) {
    const unsigned yellow = RGBA(255, 220, 40, 255), dark = RGBA(30, 25, 60, 255);
    tri(r - 16, cy - 11, r + 3, cy, r - 16, cy + 11, dark);
    tri(r - 14, cy - 8, r, cy, r - 14, cy + 8, yellow);
    tri(l + 16, cy - 11, l - 3, cy, l + 16, cy + 11, dark);
    tri(l + 14, cy - 8, l, cy, l + 14, cy + 8, yellow);
}

/* an ellipse about cx, cy: its middle's colour out to its edge's */
static void ellipse(float cx, float cy, float rx, float ry, unsigned mid, unsigned edge) {
    enum { SIDES = 48 };
    RD2D_VTX v[SIDES * 3];
    int i;
    for (i = 0; i < SIDES; i++) {
        float a0 = i * 6.2831853f / SIDES, a1 = (i + 1) * 6.2831853f / SIDES;
        put_vtx(&v[i * 3], cx, cy, 0, 0, mid);
        put_vtx(&v[i * 3 + 1], cx + rx * __builtin_cosf(a0), cy + ry * __builtin_sinf(a0), 0, 0, edge);
        put_vtx(&v[i * 3 + 2], cx + rx * __builtin_cosf(a1), cy + ry * __builtin_sinf(a1), 0, 0, edge);
    }
    rd_vgl_2d_draw(0, v, SIDES * 3);
}

/* a button's glyph (START, SELECT): a dark rounded pill with its name, its left at x, its middle at cy; its width */
static float button_glyph(const char* name, float x, float cy) {
    float w = text_width(name, 0.5f) + 18;
    pill(&PILL_SLATE, x, cy - 11, x + w, cy + 11, RGBA(70, 70, 82, 255));
    text(name, x + w * 0.5f, cy - 6, 0.5f, CENTRE, RGBA(255, 255, 255, 255), RGBA(225, 225, 235, 255), 0, 0, 0);
    return w;
}

/* a line of words and button glyphs: "[NAME]" a glyph; its left at x, its middle at cy */
static void hint(const char* const* parts, int n, float x, float cy) {
    const unsigned white = RGBA(255, 255, 255, 255), edge = RGBA(10, 10, 30, 255);
    int i;
    for (i = 0; i < n; i++) {
        if (parts[i][0] == '[') {
            char name[16];
            snprintf(name, sizeof name, "%.*s", (int)strlen(parts[i]) - 2, parts[i] + 1);
            x += button_glyph(name, x, cy) + 6;
        } else {
            text(parts[i], x, cy - 9, 0.72f, LEFT, white, white, edge, 1.5f, 0);
            x += text_width(parts[i], 0.72f) + 6;
        }
    }
}

static float hint_width(const char* const* parts, int n) {
    float w = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (parts[i][0] == '[') {
            char name[16];
            snprintf(name, sizeof name, "%.*s", (int)strlen(parts[i]) - 2, parts[i] + 1);
            w += text_width(name, 0.5f) + 18 + 6;
        } else {
            w += text_width(parts[i], 0.72f) + 6;
        }
    }
    return w - 6;
}

static void background(void) {
    const unsigned top = RGBA(70, 95, 235, 255), bottom = RGBA(35, 48, 165, 255);
    if (pics[T_MARBLE].tex != 0) {
        quad(T_MARBLE, 0, 0, 960, 544, 0, 0, 960 * 256.0f / 400, 544 * 256.0f / 400, top, bottom);
    } else {
        fill(0, 0, 960, 544, top, bottom);
    }
    /* darker at the top and bottom, as the later ports' screens */
    fill(0, 0, 960, 90, RGBA(10, 15, 70, 150), RGBA(10, 15, 70, 0));
    fill(0, 420, 960, 544, RGBA(10, 15, 70, 0), RGBA(10, 15, 70, 170));
}

/* the title in a tilted white oval with a grey-blue rim, as the later ports' */
static void title_oval(const char* l1, const char* l2) {
    const float cx = 150, cy = 90;
    turn(-9.0f, cx, cy);
    ellipse(cx + 3, cy + 5, 132, 79, RGBA(0, 0, 40, 90), RGBA(0, 0, 40, 0));
    ellipse(cx, cy, 130, 77, RGBA(150, 158, 200, 255), RGBA(70, 80, 135, 255));
    ellipse(cx, cy, 125, 72, RGBA(190, 196, 225, 255), RGBA(175, 182, 215, 255));
    ellipse(cx, cy, 118, 66, RGBA(255, 255, 255, 255), RGBA(208, 212, 228, 255));
    text(l1, cx - 8, cy - 40, 1.55f, CENTRE, RGBA(255, 252, 110, 255), RGBA(255, 196, 0, 255), RGBA(25, 18, 60, 255),
         2.5f, 1);
    text(l2, cx + 14, cy + 0, 1.55f, CENTRE, RGBA(255, 252, 110, 255), RGBA(255, 196, 0, 255), RGBA(25, 18, 60, 255),
         2.5f, 1);
    turn(0, 0, 0);
}

static void header(const char* s) {
    text(s, 942, 18, 1.05f, RIGHT, RGBA(235, 255, 90, 255), RGBA(170, 225, 0, 255), RGBA(15, 25, 10, 255), 2.0f, 1);
}

static const unsigned label_top[OPTIONS + 1] = { RGBA(150, 222, 255, 255), RGBA(255, 175, 240, 255),
                                                RGBA(255, 222, 120, 255), RGBA(170, 255, 205, 255),
                                                RGBA(215, 185, 255, 255), RGBA(255, 130, 105, 255) };
static const unsigned label_bottom[OPTIONS + 1] = { RGBA(70, 120, 255, 255), RGBA(230, 60, 200, 255),
                                                   RGBA(255, 120, 30, 255), RGBA(40, 200, 120, 255),
                                                   RGBA(140, 90, 255, 255), RGBA(230, 20, 30, 255) };

/* a GAME SETUP row's places, for its drawing and the touch screen: its pill, its label's left and size, its value's
   right and size, and its arrows' points (the value kept in place whether chosen or not, 12 pixels from each) */
typedef struct {
    float x0, y0, x1, y1, label_x, label_scale, value_x, value_scale, left, right;
    int narrow;                 /* ASPECT RATIO with ORIGINAL: the BORDERS box beside it */
} ROW;

#define BOX_X0 730.0f           /* the BORDERS box */
#define BOX_X1 952.0f

static void row_place(int i, ROW* g) {
    const char* label = i < OPTIONS ? options[i].label : "NEXT";
    float w, room;
    g->y0 = 116 + i * 55.0f, g->y1 = g->y0 + 42;
    g->x0 = 330 - i * 20.0f, g->x1 = i < OPTIONS ? 932 - i * 20.0f : g->x0 + 240;
    g->narrow = i == O_ASPECT && m.value[O_ASPECT] == 1;
    if (g->narrow) {
        g->x0 = 296, g->x1 = BOX_X0 - 9;
    }
    g->label_scale = 1.12f;
    if (i == OPTIONS) {
        g->label_x = (g->x0 + g->x1) * 0.5f;
        return;
    }
    g->value_scale = g->narrow ? 0.85f : 1.0f;
    w = text_width(options[i].values[m.value[i]], g->value_scale);
    g->right = g->x1 - (g->narrow ? 20 : 30);
    g->value_x = g->right - 26;
    g->left = g->value_x - w - 26;
    g->label_x = g->x0 + (g->narrow ? 26 : 34);
    /* the label smaller when it would come within 18 pixels of the left arrow */
    room = g->left - 18 - g->label_x;
    if (text_width(label, g->label_scale) > room) {
        g->label_scale = room / text_width(label, 1.0f);
    }
}

static void page_setup(void) {
    int i;
    const char* help;
    static const char* const save[] = { "Press", "[START]", "to save" };
    const unsigned white = RGBA(255, 255, 255, 255);
    title_oval("GAME", "SETUP");
    header("Choose the settings you wish to use.");
    hint(save, 3, 940 - hint_width(save, 3), 64);
    for (i = 0; i <= OPTIONS; i++) {
        ROW g;
        int chosen = m.row == i && !m.side;
        const char* label = i < OPTIONS ? options[i].label : "NEXT";
        float cy;
        row_place(i, &g);
        cy = (g.y0 + g.y1) * 0.5f;
        pill(chosen ? &PILL_LIME : &PILL_SLATE, g.x0, g.y0, g.x1, g.y1, white);
        text(label, g.label_x, cy - GLYPH * g.label_scale * 0.5f - 1.0f, g.label_scale, i < OPTIONS ? LEFT : CENTRE,
             chosen ? RGBA(255, 245, 90, 255) : label_top[i], chosen ? RGBA(255, 150, 0, 255) : label_bottom[i],
             RGBA(20, 20, 45, 255), 2.0f, 1);
        if (i < OPTIONS) {
            text(options[i].values[m.value[i]], g.value_x, cy - GLYPH * g.value_scale * 0.5f - 1.0f, g.value_scale,
                 RIGHT, white, RGBA(200, 205, 225, 255), RGBA(20, 22, 50, 255), 2.0f, 1);
            if (chosen) {
                arrows(g.left, g.right, cy);
            }
        }
        if (g.narrow) {
            /* the BORDERS box: its value in the middle, as large as fits 12 pixels inside its arrows */
            int on = m.row == O_ASPECT && m.side;
            const char* v = border_names[m.borders];
            float l = BOX_X0 + 10, r = BOX_X1 - 10, room = (r - 14 - 12) - (l + 14 + 12);
            float sc = text_width(v, 0.8f) > room ? room / text_width(v, 1.0f) : 0.8f;
            pill(on ? &PILL_LIME : &PILL_SLATE, BOX_X0, g.y0, BOX_X1, g.y1, white);
            text(v, (BOX_X0 + BOX_X1) * 0.5f, cy - GLYPH * sc * 0.5f - 1.0f, sc, CENTRE, white, RGBA(200, 205, 225, 255),
                 RGBA(20, 22, 50, 255), 2.0f, 1);
            if (on) {
                arrows(l, r, cy);
            }
        }
    }
    fill(22, 452, 938, 530, RGBA(8, 18, 85, 225), RGBA(8, 14, 70, 235));
    frame_rect(22, 452, 938, 530, 2, RGBA(80, 120, 255, 255));
    help = m.row == O_ASPECT && m.side ? border_help[m.borders]
           : m.row < OPTIONS          ? options[m.row].help[m.value[m.row]]
                                      : "Choose your control type.";
    text_wrapped(help, 42, 466, 876, 0.85f, 26, white, 0, 0);
}

/* the scheme's picture's right stick labels, dimmed when RIGHT ANALOG STICK is off */
static void page_controls(void) {
    const unsigned white = RGBA(255, 255, 255, 255);
    int s = m.scheme;
    if (pics[T_CTL0 + s].tex != 0) {
        quad(T_CTL0 + s, 0, 0, 960, 544, 0, 0, 960, 544, white, white);
    }
    if (!m.value[O_STICK]) {
        fill(744, 130, 954, 188, RGBA(15, 22, 90, 215), RGBA(15, 22, 90, 215));
        text("RIGHT STICK OFF", 849, 149, 0.72f, CENTRE, RGBA(200, 205, 230, 255), RGBA(160, 165, 200, 255),
             RGBA(10, 10, 30, 255), 1.5f, 0);
    }
    /* in the picture's screen: the type, its choice and what it does */
    text("CONTROL TYPE", 472, 44, 1.1f, CENTRE, RGBA(255, 250, 100, 255), RGBA(255, 180, 0, 255), RGBA(20, 15, 45, 255),
         2.0f, 1);
    pill(m.row == P1_TYPE ? &PILL_LIME : &PILL_SLATE, 334, 76, 612, 112, white);
    {
        const unsigned yellow = RGBA(255, 220, 40, 255), dark = RGBA(30, 25, 60, 255);
        float cy = 94;
        text(scheme_names[s], 473, cy - 13, 1.1f, CENTRE, white, RGBA(200, 205, 225, 255), RGBA(20, 22, 50, 255), 2.0f,
             1);
        if (m.row == P1_TYPE) {
            tri(372, cy - 11, 353, cy, 372, cy + 11, dark);
            tri(370, cy - 8, 356, cy, 370, cy + 8, yellow);
            tri(574, cy - 11, 593, cy, 574, cy + 11, dark);
            tri(576, cy - 8, 590, cy, 576, cy + 8, yellow);
        }
    }
    text_wrapped(scheme_help[s], 330, 124, 284, 0.72f, 20, white, RGBA(10, 10, 20, 255), 1.0f);
    /* don't show this again */
    if (m.row == P1_HIDE) {
        pill(&GLOW_GREEN, 0, 404, 210, 462, white);
    }
    icon(&ICON_BOX, 10, 418, 1.0f, white);
    if (m.hide) {
        icon(&ICON_TICK, 13, 418, 1.1f, white);
    }
    text("Don't show this", 46, 414, 0.7f, LEFT, white, white, RGBA(10, 10, 30, 255), 1.5f, 0);
    text("again at startup", 46, 432, 0.7f, LEFT, white, white, RGBA(10, 10, 30, 255), 1.5f, 0);
    /* the way back to it */
    {
        static const char* const l1[] = { "Hold", "[SELECT]", "at boot" }, * const l2[] = { "to show this" },
                                 * const l3[] = { "menu again" };
        hint(l1, 3, 744, 400);
        hint(l2, 1, 744, 422);
        hint(l3, 1, 744, 444);
    }
    /* BACK and START GAME */
    pill(m.row == P1_BACK ? &PILL_LIME : &PILL_SLATE, 16, 476, 206, 524, white);
    text("BACK", 111, 487, 1.15f, CENTRE, m.row == P1_BACK ? RGBA(255, 245, 90, 255) : RGBA(170, 190, 255, 255),
         m.row == P1_BACK ? RGBA(255, 150, 0, 255) : RGBA(80, 90, 230, 255), RGBA(20, 20, 45, 255), 2.0f, 1);
    pill(m.row == P1_START ? &PILL_LIME : &PILL_SLATE, 714, 476, 948, 524, white);
    text("START GAME", 831, 489, 1.0f, CENTRE, m.row == P1_START ? RGBA(255, 245, 90, 255) : RGBA(255, 120, 100, 255),
         m.row == P1_START ? RGBA(255, 150, 0, 255) : RGBA(225, 20, 30, 255), RGBA(20, 20, 45, 255), 2.0f, 1);
}

static void menu_draw(void) {
    int i;
    for (i = 0; i < PICS; i++) {
        if (pics[i].px != NULL && pics[i].tex == 0) {
            pics[i].tex = rd_vgl_2d_texture(pics[i].px, pics[i].w, pics[i].h, 1);
        }
    }
    background();
    if (m.page == 0) {
        page_setup();
    } else {
        page_controls();
    }
}

static void menu_done(void) {
    int i;
    for (i = 0; i < PICS; i++) {
        rd_vgl_2d_free(pics[i].tex);
        pics[i].tex = 0;
    }
}

/* ---- the menu's run, on the game's thread ---- */

static uint32_t buttons(void) {
    SceCtrlData pad;
    uint32_t b = 0;
    memset(&pad, 0, sizeof pad);
    if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
        b = pad.buttons;
        /* the left stick as the D-pad */
        b |= pad.ly < 50 ? SCE_CTRL_UP : pad.ly > 205 ? SCE_CTRL_DOWN : 0;
        b |= pad.lx < 50 ? SCE_CTRL_LEFT : pad.lx > 205 ? SCE_CTRL_RIGHT : 0;
    }
    return b;
}

static void step(int dir) {
    if (m.page == 0) {
        if (m.row == O_ASPECT && m.side) {
            m.borders = (m.borders + dir + 3) % 3;
        } else if (m.row < OPTIONS) {
            int n = options[m.row].count;
            m.value[m.row] = (m.value[m.row] + dir + n) % n;
        }
    } else if (m.row == P1_TYPE) {
        m.scheme = (m.scheme + dir + SCHEMES) % SCHEMES;
    } else if (m.row == P1_BACK || m.row == P1_START) {
        m.row = m.row == P1_BACK ? P1_START : P1_BACK;
    }
}

/* a tap on the front touch screen (down and up within 40 frames, moved less than 24 pixels): where it came down, in
   the screen's pixels */
static int tapped(float* tx, float* ty) {
    static int ready, down, sx, sy, frames, far;
    SceTouchData d;
    int n;
    if (!ready) {
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
        ready = 1;
    }
    n = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &d, 1) > 0 ? (int)d.reportNum : 0;
    if (n > 0) {
        int x = d.report[0].x / 2, y = d.report[0].y / 2;
        if (!down) {
            down = 1, sx = x, sy = y, frames = 0, far = 0;
        }
        frames++;
        far |= (x - sx) * (x - sx) + (y - sy) * (y - sy) > 24 * 24;
        return 0;
    }
    if (!down) {
        return 0;
    }
    down = 0;
    *tx = (float)sx, *ty = (float)sy;
    return !far && frames <= 40;
}

static int inside(float x, float y, float x0, float y0, float x1, float y1) {
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

/* a tap's work: what it lands on chosen, or done when already chosen (a value's arrows step it that way, the rest of
   its row forward); START GAME, BACK, NEXT, the check box and "Press START to save" at once. 1: the menu done */
static int touch(float x, float y) {
    if (m.page == 0) {
        static const char* const save[] = { "Press", "[START]", "to save" };
        int i;
        if (inside(x, y, 940 - hint_width(save, 3) - 8, 48, 948, 80)) {
            return 1;
        }
        for (i = 0; i <= OPTIONS; i++) {
            ROW g;
            row_place(i, &g);
            if (g.narrow && inside(x, y, BOX_X0, g.y0 - 6, BOX_X1, g.y1 + 6)) {
                if (m.row == O_ASPECT && m.side) {
                    step(x < (BOX_X0 + BOX_X1) * 0.5f ? -1 : 1);
                }
                m.row = O_ASPECT, m.side = 1;
                return 0;
            }
            if (!inside(x, y, g.x0, g.y0 - 6, g.x1, g.y1 + 6)) {
                continue;
            }
            if (i == OPTIONS) {
                m.page = 1, m.row = P1_TYPE, m.side = 0;
            } else if (m.row == i && !m.side) {
                step(x < g.left + 30 && x > g.left - 30 ? -1 : 1);
            } else {
                m.row = i, m.side = 0;
            }
            return 0;
        }
        return 0;
    }
    if (inside(x, y, 334, 70, 612, 118)) {
        if (m.row == P1_TYPE) {
            step(x < 473 ? -1 : 1);
        }
        m.row = P1_TYPE;
    } else if (inside(x, y, 0, 400, 214, 466)) {
        m.row = P1_HIDE, m.hide = !m.hide;
    } else if (inside(x, y, 16, 470, 206, 530)) {
        m.page = 0, m.row = OPTIONS, m.side = 0;
    } else if (inside(x, y, 714, 470, 948, 530)) {
        return 1;
    }
    return 0;
}

/* the menu until START GAME (or START); its settings then written and set */
static void run(void) {
    uint32_t held = buttons(), prev = held, repeat_at = 0;
    unsigned frames = 0;
    int done = 0;
    m.page = 0, m.row = 0, m.side = 0;
    rd_vgl_menu(menu_draw, menu_done);
    while (!done) {
        uint32_t now = buttons(), press = now & ~prev;
        const uint32_t dirs = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
        sceDisplayWaitVblankStart();
        frames++;
        m.frame = frames;
        /* a direction held repeats after 24 frames, every 7 */
        if ((now & dirs) != 0 && (now & dirs) == (prev & dirs)) {
            if (frames >= repeat_at) {
                press |= now & dirs;
                repeat_at = frames + 7;
            }
        } else {
            repeat_at = frames + 24;
        }
        prev = now;
        {
            float tx, ty;
            if (tapped(&tx, &ty) && touch(tx, ty)) {
                done = 1;
            }
        }
        if (done) {
        } else if (press & SCE_CTRL_START) {
            done = 1;
        } else if (m.page == 0) {
            /* ORIGINAL's BORDERS box beside ASPECT RATIO: chosen between it and the row below */
            const int original = m.value[O_ASPECT] == 1;
            if (press & SCE_CTRL_UP) {
                if (m.side) {
                    m.side = 0;
                } else {
                    m.row = (m.row + OPTIONS) % (OPTIONS + 1);
                    m.side = m.row == O_ASPECT && original;
                }
            } else if (press & SCE_CTRL_DOWN) {
                if (m.row == O_ASPECT && original && !m.side) {
                    m.side = 1;
                } else {
                    m.row = (m.row + 1) % (OPTIONS + 1);
                    m.side = 0;
                }
            } else if (press & SCE_CTRL_LEFT) {
                step(-1);
            } else if (press & SCE_CTRL_RIGHT) {
                step(1);
            } else if (press & SCE_CTRL_CROSS) {
                if (m.row == OPTIONS) {
                    m.page = 1, m.row = P1_TYPE, m.side = 0;
                } else {
                    step(1);
                }
            }
        } else {
            if (press & SCE_CTRL_UP) {
                m.row = m.row == P1_START ? P1_HIDE : m.row > 0 ? m.row - 1 : P1_START;
            } else if (press & SCE_CTRL_DOWN) {
                m.row = m.row == P1_BACK || m.row == P1_START ? P1_TYPE : m.row + 1;
            } else if (press & SCE_CTRL_LEFT) {
                step(-1);
            } else if (press & SCE_CTRL_RIGHT) {
                step(1);
            } else if (press & SCE_CTRL_CIRCLE) {
                m.page = 0, m.row = OPTIONS;
            } else if (press & SCE_CTRL_CROSS) {
                if (m.row == P1_TYPE) {
                    step(1);
                } else if (m.row == P1_HIDE) {
                    m.hide = !m.hide;
                } else if (m.row == P1_BACK) {
                    m.page = 0, m.row = OPTIONS;
                } else {
                    done = 1;
                }
            }
        }
    }
}

void vita_config_menu(const char* disc) {
    static int ran;
    const char* soft = getenv("MVC2_RENDERER");
    int show;
    if (ran++) {
        return;     /* once a run: not again when the game is restarted */
    }
    from_env();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);     /* the left stick read too */
    show = (buttons() & SCE_CTRL_SELECT) != 0 || !m.hide;
    if (getenv("MVC2_FRAMES") != NULL || (soft != NULL && strcmp(soft, "soft") == 0)) {
        show = 0;
    }
    host_log("menu: %s (CONFIG_MENU %s, SELECT %s)", show ? "shown" : "not shown", m.hide ? "OFF" : "ON",
             (buttons() & SCE_CTRL_SELECT) != 0 ? "held" : "not held");
    if (show) {
        pics_load(disc);
        /* the settings set before the menu closes: its close reads ASPECT_RATIO and BORDERS again, so the game's
           first frame has them */
        run();
        to_env();
        vita_clocks_apply();
        rd_vgl_menu(NULL, NULL);
        pics_free();
    }
    input_scheme(scheme_names[m.scheme], m.value[O_STICK]);
}
