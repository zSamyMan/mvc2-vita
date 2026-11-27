/*
 * rd_vgl.c  The PS Vita's GPU back end: the frame's lists drawn by vitaGL, by rd_soft.c's rules
 *
 * Threads: the game fills one RD_FRAME while a render thread (core 1, the only one making GL calls) reads the
 * other. At the hand-over the frame's palette, fog and textures are frozen with it; the render thread gives the
 * frame back as soon as it has read it (its vertices built, its textures uploaded), before its GL calls and the
 * wait for the vertical blank, so the game's next frame and this one's drawing overlap.
 *
 * Depth: 1/w, bigger nearer, as the PowerVR2 compares it, but sprites the game puts one float step apart must
 * stay apart, so each frame's distinct 1/w values are sorted and a vertex's depth is half its value's rank and
 * half its place between the nearest and farthest (for the slope across a model's triangles). Values that are
 * equal stay equal, and the depth tests keep their meaning.
 *
 * Perspective: a vertex's clip w is its w (1/z), so the GPU interpolates u, v and the colours over w as the
 * PowerVR2 does, and a varying of w gives the fog each pixel's 1/w.
 *
 * Shading: four Cg programs (no texture, a texture, a palette's by the nearest texel, a palette's filtered by
 * hand), the TSP's modes, the offset colour, table fog and punch-through by uniforms. Palettes are read on the
 * GPU, so a palette's change costs 4 KB. The translucent list is sorted by triangle, far to near, in the order
 * sent where equal: rd_soft sorts by pixel, which a flat sprite's triangle matches.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>
#include <vitashark.h>

#include "naomi_host.h"
#include "rd.h"
#include "rd_list.h"
#include "rd_vgl.h"

const char* config_get(const char* key);    /* port/sdl/start.c */
extern int vita_debug;                      /* port/vita/vita_sys.c: DEBUG_OPTIONS */

#define SCREEN_W 960
#define SCREEN_H 544
#define MAX_SNAPS 2048
#define MAX_VERTS (3 * 49152)
#define MAX_BATCHES 8192
#define TEX_SLOTS 4096          /* a power of 2 */
#define TEX_IDLE 600            /* frames a texture goes undrawn before it is let go */
#define DATA_DIR "ux0:data/mvc_2/"

static uint64_t now_us(void) {
    return sceKernelGetProcessTimeWide();
}

/* ---- the frames handed over ---- */

typedef struct {
    RD_TEXTURE t;               /* the texture as it was when its frame was handed over */
    const RD_TEXTURE* orig;     /* its slot: the cache's key */
} SNAP;

typedef struct {
    RD_FRAME* f;
    uint32_t palette[1024];
    uint32_t palette_serial;
    RD_FOG fog;
    SNAP snaps[MAX_SNAPS];
    int nsnaps;
    unsigned frame_no;
    uint64_t handed;
    int shot;                   /* a picture of it wanted, rd_soft's of it already taken (MVC2_SHOT_SOFT) */
    int early;                  /* its depths and translucent list's steps begun on the cutting thread as it was handed
                                   over (job 5), while the frame before is drawn: prepare waits for them */
} VFRAME;

static VFRAME* vf[2];
static int fill;                /* the game's: vf[fill] is rd_frame */
static SceUID sem_ready, sem_free, sem_started;
static volatile int requests;
static volatile int started_ok;
static unsigned presented;

/* ---- the render thread's own copy of what it draws ---- */

typedef struct {
    float x, y, z, w;           /* clip */
    float u, v;
    uint8_t base[4];            /* r g b a */
    uint8_t offs[4];
    float clamp[4];             /* u, v kept within: a sprite's own texels (half a texel in), else unbounded */
    float fw;                   /* w (1 over the game's 1/w) for the fog: a level triangle is drawn with w 1 */
} VTX;

typedef struct {
    RD_STATE s;
    int list, first, count;
    uint8_t clamp, offs;        /* some vertex has a sprite's UV bounds; some an offset colour: the programs that do them */
    struct TEX* tex;
} BATCH;

typedef struct TEX {
    const RD_TEXTURE* key;
    uint32_t serial;
    GLuint id;
    int w, h, format, levels;
    uint32_t used;
    GLint wrap_s, wrap_t, filter;
    int hwpal;                  /* a palette's texture made P8 for the GPU's own lookup (MVC2_HWPAL) */
    GLuint ring[6];             /* the GL textures it takes turns in (id is ring[ring_at]), see tex_upload */
    uint32_t ring_used[6];      /* render_no each was last drawn with */
    uint8_t ring_at, ring_n;
    uint32_t up_no;             /* render_no of its last upload */
    uint32_t late_no;           /* render_no it was last left a frame late (tex_get) */
} TEX;

static VTX* verts;
static int nverts;
static BATCH* batches;
static int nbatches;

/* where add_tri_raw puts a triangle: the frame's own arrays, or the cutting thread's for the translucent list's
   second half (appended to the frame's after); a batch's texture is looked up later, on the render thread (GL) */
typedef struct {
    VTX* v;
    int* nv;
    BATCH* b;
    int* nb;
    int vmax, bmax;
    int last_w, last_h;         /* sprite_fit's half texel, by the texture's size */
    float iw, ih;
} VBUF;
static VBUF vb_main;
static uint32_t palette[1024], palette_serial = ~0u, palette_uploaded = ~1u;
static RD_FOG fog;
static int fog_on;
static VTX background[4];
static TEX texes[TEX_SLOTS];
static uint32_t render_no;
static uint16_t* conv;          /* a texture's texels in the GPU's order */

/* depth: each vertex's, from its 1/w's place among the frame's (positive floats sort as their bits). A vertex's
   id is its list's base, then its triangle's * 3 + its corner; the background's three come last */
static float* vdepth;
static int nz, zroom, vbase[RD_LISTS + 1];

/* ---- the statistics, over PERF_US ---- */

/* the threads whose CPU time the log and the overlay show */
#define WATCHED 16
#define OV_THREADS 6     /* the overlay shows the first ones */
static struct {
    char name[8];
    SceUID id;
    uint64_t last_clocks, log_clocks;
    unsigned pct, cpu;
} watched[WATCHED];
static uint64_t watch_log_at;
static int nwatched;

#define PERF_US 2000000
typedef struct {
    uint64_t start;
    unsigned frames, missed, worst_us;
    uint64_t wait_us;           /* the game, waiting for the render thread to give a frame back */
    uint64_t prep_us, tex_us, gl_us, swap_us, frame_us;
    unsigned tris, batches, uploads, upload_kb, binds;
    unsigned up_sub, up_pool, up_made;  /* uploads written into a texture it had, into one from the pool, into a new one */
    uint64_t slow_total, slow[6];
    uint64_t part_us[4], part_worst[4];     /* prepare's: depth table, opaque and punch, translucent cut and sort, add */
    unsigned last_vcount;
    uint64_t last_swap;
} PERF;
static PERF perf;
static volatile uint64_t game_wait_us;
static uint64_t report_us;
/* the slowest single GL step of the window over 20 ms, and what it was */
static uint64_t spike_us;
static const char* spike_what;
static unsigned stall_dumps;

static void gl_step(const char* what, uint64_t t0) {
    uint64_t d = now_us() - t0;
    if (d > 20000 && d > spike_us) {
        spike_us = d;
        spike_what = what;
    }
    /* a GPU stall past the first frames, in the log (its dump and picture, taken here before, held the render
       thread up seconds more on the card: gone) */
    if (d > 200000 && render_no > 600 && stall_dumps < 16) {
        stall_dumps++;
        host_log("vgl: %s stalled %.1f ms at frame %u", what, d / 1000.0, render_no);
    }
}      /* the slowest perf_report of the window (its log lines, the threads' CPU) */
/* MVC2_TRACE=N: the first N frames' times one by one (us), to trace.csv once all are in. The game's side (its
   own work and the sound's) comes by rd_vgl_trace_game; attract mode is deterministic by frame, so runs compare */
typedef struct {
    uint32_t wait, prep, gl, extras, swap, missed, uploads, up_kb, tris, game, sound, game_wait, pieces;
} TRACE_ROW;
static TRACE_ROW* trace;
static unsigned trace_n, trace_at;    /* MVC2_TRACE_AT: the last frame traced (the trace the trace_n before it) */
static volatile int trace_ready;
#define TRACE_SLOT(frame) (((frame) - 1) % trace_n)
#define TRACE_ON(frame) ((frame) >= 1 && (frame) <= trace_at && (frame) + trace_n > trace_at)

void rd_vgl_trace_game(unsigned frame, unsigned game_us, unsigned sound_us) {
    if (trace != NULL && TRACE_ON(frame)) {
        trace[TRACE_SLOT(frame)].game = game_us;
        trace[TRACE_SLOT(frame)].sound = sound_us;
    }
}

static unsigned guard_cuts, cut_count, order_moves, order_checks, order_planes, order_adds;
static uint64_t order_us, order_init_us, tl_us[5], cut_stats[3], cut_us[3];   /* cut_us: index (sort, grid), the deep ones cut, the rest put */   /* deep, shallow, the cuts' candidates looked at */

static void perf_report(int now) {
    uint64_t t = now_us();
    double s;
    unsigned n = perf.frames ? perf.frames : 1;
    if (perf.start == 0) {
        perf.start = t;
        return;
    }
    if (!now && t - perf.start < PERF_US) {
        return;
    }
    s = (t - perf.start) / 1e6;
    host_log("vgl: %.1f fps, %u vblanks missed, worst %.1f ms | game waited %.2f ms | render: prep %.2f tex %.2f "
             "(%u up, %u KB) gl %.2f swap %.2f | %u tris %u batches %u binds a frame, %u guard-cut %u depth-cut %u reordered (%u weighed, %u planes, %u adding, %.2f ms, setup %.2f)",
             perf.frames / s, perf.missed, perf.worst_us / 1000.0, game_wait_us / 1000.0 / n, perf.prep_us / 1000.0 / n,
             perf.tex_us / 1000.0 / n, perf.uploads, perf.upload_kb, perf.gl_us / 1000.0 / n, perf.swap_us / 1000.0 / n,
             perf.tris / n, perf.batches / n, perf.binds / n, guard_cuts / n, cut_count / n, order_moves / n,
             order_checks / n, order_planes / n, order_adds / n, order_us / 1000.0 / n, order_init_us / 1000.0 / n);
    guard_cuts = cut_count = order_moves = order_checks = order_planes = order_adds = 0;
    order_us = order_init_us = 0;
    if (perf.uploads > 0) {
        host_log("vgl: uploads: %u into their own texture, %u into a pooled one, %u into a new one", perf.up_sub,
                 perf.up_pool, perf.up_made);
    }
    host_log("vgl: translucent list's steps a frame: info %.2f, bounds and cuts %.2f, sort %.2f, order %.2f ms (%u tris; "
             "%u deep, %u shallow, %u candidates; index %.2f, pieces %.2f, waiting for the cutting thread %.2f ms)", tl_us[0] / 1000.0 / n, tl_us[1] / 1000.0 / n, tl_us[2] / 1000.0 / n,
             tl_us[3] / 1000.0 / n, (unsigned)(tl_us[4] / n), (unsigned)(cut_stats[0] / n), (unsigned)(cut_stats[1] / n),
             (unsigned)(cut_stats[2] / n), cut_us[0] / 1000.0 / n, cut_us[1] / 1000.0 / n, cut_us[2] / 1000.0 / n);
    memset(tl_us, 0, sizeof tl_us);
    memset(cut_us, 0, sizeof cut_us);
    memset(cut_stats, 0, sizeof cut_stats);
    host_log("vgl: prepare's parts a frame (worst): depth %.2f (%.1f), opaque %.2f (%.1f), translucent cuts %.2f (%.1f), "
             "translucent add %.2f (%.1f)", perf.part_us[0] / 1000.0 / n, perf.part_worst[0] / 1000.0,
             perf.part_us[1] / 1000.0 / n, perf.part_worst[1] / 1000.0, perf.part_us[2] / 1000.0 / n,
             perf.part_worst[2] / 1000.0, perf.part_us[3] / 1000.0 / n, perf.part_worst[3] / 1000.0);
    host_log("vgl: slowest loop %.1f ms: waited for the game %.1f, read %.1f, GL %.1f, extras %.1f, swap %.1f, the rest "
             "%.1f; the last report %.2f ms", perf.slow_total / 1000.0, perf.slow[0] / 1000.0, perf.slow[1] / 1000.0,
             perf.slow[2] / 1000.0, perf.slow[3] / 1000.0, perf.slow[4] / 1000.0, perf.slow[5] / 1000.0,
             report_us / 1000.0);
    report_us = 0;
    if (spike_us != 0) {
        host_log("vgl: a GL step took %.1f ms: %s", spike_us / 1000.0, spike_what);
        spike_us = 0;
    }
    game_wait_us = 0;
    memset(&perf, 0, offsetof(PERF, last_vcount));
    perf.start = t;
}

/* ---- GL state, set only when it changes ---- */

static struct {
    int prog, blend, src, dst, depth_func, depth_mask;
    GLuint tex;
} cur;

/* ---- the threads' CPU time, and the overlay ---- */

static uint64_t watch_at;

void rd_vgl_watch_thread(const char* name, int id) {
    if (nwatched < WATCHED) {
        snprintf(watched[nwatched].name, sizeof watched[nwatched].name, "%s", name);
        watched[nwatched].id = id;
        nwatched++;
    }
}

/* each watched thread's share of the time since the last call, and the core it last ran on */
static void watch_threads(void) {
    uint64_t t = now_us(), span = t - watch_at;
    int i;
    for (i = 0; i < nwatched; i++) {
        SceKernelThreadInfo info;
        uint64_t clocks;
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (sceKernelGetThreadInfo(watched[i].id, &info) < 0) {
            continue;
        }
        memcpy(&clocks, &info.runClocks, sizeof clocks);
        if (watch_at != 0 && span > 0) {
            watched[i].pct = (unsigned)((clocks - watched[i].last_clocks) * 100 / span);
        }
        watched[i].cpu = (unsigned)info.lastExecutedCpuId;
        watched[i].last_clocks = clocks;
    }
    watch_at = t;
}

/* the game's side, from the front end's perf line */
static volatile float game_ms, sound_ms, game_worst_ms;

/* the threads' CPU shares since the last call, into the log: on the file thread, as sceKernelGetThreadInfo on a
   thread of another core can take long (it held the renderer's report up to 140 ms) */
static void log_threads(void) {
    uint64_t t = now_us();
    {
        char line[160];
        int k, at = 0;
        uint64_t span = t - watch_log_at;
        for (k = 0; k < nwatched && at < (int)sizeof line - 32; k++) {
            SceKernelThreadInfo info;
            uint64_t clocks;
            memset(&info, 0, sizeof info);
            info.size = sizeof info;
            if (sceKernelGetThreadInfo(watched[k].id, &info) < 0) {
                continue;
            }
            memcpy(&clocks, &info.runClocks, sizeof clocks);
            at += snprintf(line + at, sizeof line - at, " %s %u%% (core %d)", watched[k].name,
                           watch_log_at && span ? (unsigned)((clocks - watched[k].log_clocks) * 100 / span) : 0,
                           info.lastExecutedCpuId);
            watched[k].log_clocks = clocks;
        }
        watch_log_at = t;
        host_log("vgl: threads' CPU:%s", line);
        {
            struct mallinfo mi = mallinfo();
            SceKernelFreeMemorySizeInfo fm;
            fm.size = sizeof fm;
            sceKernelGetFreeMemorySize(&fm);
            host_log("vgl: heap %u KB in use of %u KB taken (%u KB free in it, %u KB in blocks of their own); system "
                     "free: user %d KB, CDRAM %d KB; vitaGL free: RAM %u KB, VRAM %u KB", (unsigned)mi.uordblks >> 10,
                     (unsigned)mi.arena >> 10, (unsigned)mi.fordblks >> 10, (unsigned)mi.hblkhd >> 10, fm.size_user >> 10,
                     fm.size_cdram >> 10, (unsigned)(vglMemFree(VGL_MEM_RAM) >> 10), (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10));
        }
    }
}

void rd_vgl_game_stats(float game, float sound, float worst) {
    game_ms = game, sound_ms = sound, game_worst_ms = worst;
}

/* MVC2_OVERLAY=1 or SELECT + UP: the figures in the left pillar, 116 by 220 */
#define OV_W 116
#define OV_H 232
static int overlay_on;
static GLuint overlay_tex;
static uint32_t overlay_px[OV_W * OV_H];
static struct {
    unsigned frames, missed, worst_us;
    uint64_t start, prep, gl, swap;
} ov;

void menu_text(uint32_t* buf, int w, int h, int x, int y, const char* s, uint32_t color, int scale);
#include "vita_debug_menu.h"

static void overlay_update(void) {
    char line[32];
    int i, y = 3;
    uint64_t t = now_us();
    double s = (t - ov.start) / 1e6;
    unsigned n = ov.frames ? ov.frames : 1;
#define OV_LINE(colour, ...) do { snprintf(line, sizeof line, __VA_ARGS__); menu_text(overlay_px, OV_W, OV_H, 3, y, line, colour, 1); y += 10; } while (0)
    for (i = 0; i < OV_W * OV_H; i++) {
        overlay_px[i] = 0xB0000000u;
    }
    OV_LINE(ov.missed ? 0xFF4040FFu : 0xFF40FF40u, "FPS %.1f", ov.frames / s);
    OV_LINE(ov.missed ? 0xFF4040FFu : 0xFFFFFFFFu, "MISSED %u", ov.missed);
    OV_LINE(0xFFFFFFFFu, "WORST %.1f", ov.worst_us / 1000.0);
    y += 4;
    OV_LINE(0xFF80FFFFu, "GAME %.1f", game_ms);
    OV_LINE(0xFF80FFFFu, " PEAK %.1f", game_worst_ms);
    OV_LINE(0xFF80FFFFu, "SOUND %.1f", sound_ms);
    y += 4;
    OV_LINE(0xFFFFC080u, "PREP %.2f", ov.prep / 1000.0 / n);
    OV_LINE(0xFFFFC080u, "GL %.2f", ov.gl / 1000.0 / n);
    OV_LINE(0xFFFFC080u, "SWAP %.2f", ov.swap / 1000.0 / n);
    OV_LINE(0xFFFFC080u, "TRIS %d", nverts / 3);
    OV_LINE(0xFFFFC080u, "BATCH %d", nbatches);
    y += 4;
    OV_LINE(0xFFFFFFFFu, "CPU%%  CORE");
    for (i = 0; i < nwatched && i < OV_THREADS; i++) {
        OV_LINE(watched[i].pct > 80 ? 0xFF4040FFu : 0xFFFFFFFFu, "%-5.5s%3u %u", watched[i].name, watched[i].pct,
                watched[i].cpu);
    }
#undef OV_LINE
    glActiveTexture(GL_TEXTURE0);
    /* two textures in turn, the other's last draw 30 frames back: one written in place while the GPU may still read
       it waits for the GPU (the debug menu's panel lost the render thread its overlap that way) */
    {
        static GLuint ring[2];
        static int at, made[2];
        if (ring[0] == 0) {
            glGenTextures(2, ring);
            for (i = 0; i < 2; i++) {
                glBindTexture(GL_TEXTURE_2D, ring[i]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            }
        } else {
            at ^= 1;
        }
        glBindTexture(GL_TEXTURE_2D, ring[at]);
        if (!made[at]) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, OV_W, OV_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, overlay_px);
            made[at] = 1;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, OV_W, OV_H, GL_RGBA, GL_UNSIGNED_BYTE, overlay_px);
        }
        overlay_tex = ring[at];
    }
    cur.tex = overlay_tex;
    memset(&ov, 0, sizeof ov);
    ov.start = t;
}

/* the debug menu's (vita_debug_menu.c) switch over the overlay */
void rd_vgl_overlay_toggle(void) {
    overlay_on = !overlay_on;
}

int rd_vgl_overlay_shown(void) {
    return overlay_on;
}

/* the debug menu's panel: its lines drawn into a texture, shown in the middle at 1.5 times. Drawn again only when its
   text changed, and then into the other of two textures, at most once in DM_GAP frames: a texture the GPU may still be
   reading (vitaGL keeps up to 4 frames in flight) is never written. Written every frame into one texture, each write
   waited for the GPU to finish the frame before: the render thread lost its overlap (GL 3 to 8.5 ms, 46 to 50 fps
   on stage 4 while the menu was open) */
#define DM_W 304
#define DM_H 172
#define DM_GAP 6
static GLuint dmenu_ring[2], dmenu_tex;
static int dmenu_at;
static unsigned dmenu_wrote;    /* render_no of the last write */
static uint32_t dmenu_px[DM_W * DM_H];

static int dmenu_update(void) {
    static char last[16][DBG_LINE_MAX];
    static int last_n = -1, last_pick = -1;
    char lines[16][DBG_LINE_MAX];
    int n, pick, i, k;
    dbg_menu_text(lines, 16, &n, &pick);
    glActiveTexture(GL_TEXTURE0);
    if (dmenu_tex != 0 && (( n == last_n && pick == last_pick && memcmp(lines, last, sizeof lines) == 0) ||
                           render_no - dmenu_wrote < DM_GAP)) {
        cur.tex = 0;            /* bound by the caller */
        return 1;               /* the same text (or one written too lately): the texture as it is */
    }
    memcpy(last, lines, sizeof lines);
    last_n = n, last_pick = pick;
    for (i = 0; i < DM_W * DM_H; i++) {
        dmenu_px[i] = 0xD0200000u;
    }
    for (k = 0; k < n; k++) {
        int y = 4 + k * 11 + (k >= 3 ? 4 : 0);
        uint32_t col = k == 0 ? 0xFF40FFFFu : k < 3 ? 0xFFC0C0C0u : k == pick ? 0xFF40FF40u : 0xFFFFFFFFu;
        if (k == pick) {
            for (i = (y - 1) * DM_W; i < (y + 9) * DM_W && i < DM_W * DM_H; i++) {
                dmenu_px[i] = 0xF0503000u;
            }
        }
        menu_text(dmenu_px, DM_W, DM_H, 6, y, lines[k], col, 1);
    }
    if (dmenu_ring[0] == 0) {
        glGenTextures(2, dmenu_ring);
        for (k = 0; k < 2; k++) {
            glBindTexture(GL_TEXTURE_2D, dmenu_ring[k]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, DM_W, DM_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, dmenu_px);
        }
        dmenu_at = 0;
    } else {
        dmenu_at ^= 1;          /* the one not drawn with for DM_GAP frames */
        glBindTexture(GL_TEXTURE_2D, dmenu_ring[dmenu_at]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DM_W, DM_H, GL_RGBA, GL_UNSIGNED_BYTE, dmenu_px);
    }
    dmenu_tex = dmenu_ring[dmenu_at];
    dmenu_wrote = render_no;
    cur.tex = dmenu_tex;
    return 1;
}

/* ---- textures ---- */

static TEX* tex_find(const RD_TEXTURE* key) {
    uint32_t h = (uint32_t)(uintptr_t)key * 2654435761u >> 20, i;
    TEX* free_slot = NULL;
    for (i = 0; i < TEX_SLOTS; i++) {
        TEX* t = &texes[(h + i) & (TEX_SLOTS - 1)];
        if (t->key == key) {
            return t;
        }
        if (t->key == NULL) {
            return free_slot != NULL ? free_slot : t;
        }
        if (t->key == (const RD_TEXTURE*)1 && free_slot == NULL) {
            free_slot = t;      /* a let-go one's slot, kept so later keys are still found */
        }
    }
    return free_slot;
}

static int is_pal(int format) {
    return format == RD_PAL4 || format == RD_PAL8;
}

/* a level's texels in the GPU's order: ARGB1555 to RGBA5551, ARGB4444 to RGBA4444, RGB565 as it is, palette
   indices as bytes */
static const void* convert(int format, const void* src, int n) {
    const uint16_t* s = src;
    int i;
    switch (format) {
    case RD_ARGB1555:
        i = 0;
#ifdef __ARM_NEON
        /* NEON: eight texels a step, each rotated left by one (a shift left, the top bit inserted at the bottom) */
        for (; i + 8 <= n; i += 8) {
            uint16x8_t v = vld1q_u16(s + i);
            vst1q_u16(conv + i, vsriq_n_u16(vshlq_n_u16(v, 1), v, 15));
        }
#endif
        for (; i < n; i++) {
            conv[i] = (uint16_t)(s[i] << 1 | s[i] >> 15);
        }
        return conv;
    case RD_ARGB4444:
        i = 0;
#ifdef __ARM_NEON
        for (; i + 8 <= n; i += 8) {
            uint16x8_t v = vld1q_u16(s + i);
            vst1q_u16(conv + i, vsriq_n_u16(vshlq_n_u16(v, 4), v, 12));
        }
#endif
        for (; i < n; i++) {
            conv[i] = (uint16_t)(s[i] << 4 | s[i] >> 12);
        }
        return conv;
    default:
        return src;
    }
}

/*
 * MVC2_HWPAL=1: a palette's texture looked up by the GPU itself: its indices uploaded as L8 (a copy), its
 * descriptor then made P8, and before each draw pointed at its bank in the frame's palette (ABGR words in
 * GPU-mapped memory, a ring of PAL_RING copies so a copy the GPU may still read is not written). The texture
 * unit then filters the looked-up colours: one read where the shader did eight (the filtered program's four
 * indices and four colours).
 */
#define PAL_RING 8
static int hwpal = -1;
static uint32_t* pal_ring;      /* PAL_RING times 1024 words, 64-byte aligned */
static int pal_slot;

static void hwpal_upload(void) {
    int i;
    uint32_t* d;
    if (pal_ring == NULL) {
        uint8_t* m = vglAlloc(PAL_RING * 4096 + 64, VGL_MEM_RAM);
        if (m == NULL) {
            host_log("vgl: no GPU memory for the palettes: MVC2_HWPAL off");
            hwpal = 0;
            return;
        }
        pal_ring = (uint32_t*)(((uintptr_t)m + 63) & ~(uintptr_t)63);
    }
    pal_slot = (pal_slot + 1) % PAL_RING;
    d = pal_ring + pal_slot * 1024;
    for (i = 0; i < 1024; i++) {
        uint32_t c = palette[i];    /* ARGB to ABGR */
        d[i] = (c & 0xFF00FF00u) | (c >> 16 & 0xFFu) | (c & 0xFFu) << 16;
    }
}

/*
 * The uploads' time in a frame: a burst of sprite tiles changed at once (Abyss's last fall, thousands of tiles that
 * frame) held one frame up 15 ms. Past TEX_FRAME_US of uploads in a frame, a texture already on the GPU whose source
 * changed at the same size and format keeps its last texels for that frame and is sent the next (a tile a frame
 * late, in such a burst only); one never sent is always sent. MVC2_TEX_FRAME_US: the budget, 0 none.
 */
static uint64_t frame_up_us;
static unsigned frame_up_no;
static unsigned tex_frame_us = 1;

/*
 * GL textures given back (a slot's texture of a size it no longer has, a slot let go) kept by their size and format
 * for another slot of that size, once the GPU is done with them (6 frames): a new texture is GPU memory taken and
 * filled whole by vitaGL, a sprite tile changing size or a new one (a hyper's finish: a hundred a frame) cost about
 * 3 ms a frame of them; one from here is written in place
 */
#define POOL_MOST 1536
static struct {
    GLuint id;
    uint16_t w, h;
    uint8_t format, levels;
    uint32_t since;             /* render_no it was last drawn with */
} tpool[POOL_MOST];
static int npool;

static void pool_put(GLuint id, int w, int h, int format, int levels, uint32_t since) {
    if (id == 0) {
        return;
    }
    if (npool == POOL_MOST) {
        glDeleteTextures(1, &tpool[0].id);  /* the oldest given back */
        memmove(tpool, tpool + 1, sizeof tpool[0] * (POOL_MOST - 1));
        npool--;
    }
    tpool[npool].id = id, tpool[npool].w = (uint16_t)w, tpool[npool].h = (uint16_t)h;
    tpool[npool].format = (uint8_t)format, tpool[npool].levels = (uint8_t)levels, tpool[npool].since = since;
    npool++;
}

static GLuint pool_take(int w, int h, int format, int levels) {
    int i;
    for (i = 0; i < npool; i++) {
        if (tpool[i].w == w && tpool[i].h == h && tpool[i].format == format && tpool[i].levels == levels &&
            render_no - tpool[i].since > 5) {
            GLuint id = tpool[i].id;
            tpool[i] = tpool[--npool];
            return id;
        }
    }
    return 0;
}

static void tex_upload(TEX* e, const RD_TEXTURE* t) {
    GLenum fmt = GL_RGBA, type = GL_UNSIGNED_SHORT_5_5_5_1;
    int k, bytes = 0;
    uint64_t t0 = now_us();
    switch (t->format) {
    case RD_RGB565: fmt = GL_RGB, type = GL_UNSIGNED_SHORT_5_6_5; break;
    case RD_ARGB4444: fmt = GL_RGBA, type = GL_UNSIGNED_SHORT_4_4_4_4; break;
    case RD_PAL4:
    case RD_PAL8: fmt = GL_LUMINANCE, type = GL_UNSIGNED_BYTE; break;
    default: break;
    }
    /*
     * A sprite's tile changes its texels nearly every frame. glTexImage2D on a texture the GPU may still read made
     * vitaGL take new memory and free the old a few frames on, some 60 times a frame in a fight, and now and then
     * its pools held the render thread up (0.1 to 2 s, in the clear). So a texture changed again at the same size
     * and format takes turns in up to TEX_RING GL textures, one not drawn with for more than vitaGL's 4 frames
     * written in place with glTexSubImage2D: no memory taken or freed. MVC2_TEXRING=0: as before.
     */
    static int ring_on = -1;
    int sub = 0;
    if (ring_on < 0) {
        ring_on = getenv("MVC2_TEXRING") == NULL || atoi(getenv("MVC2_TEXRING")) != 0;
    }
    if (ring_on && e->ring_n > 0 && e->w == t->width && e->h == t->height && e->format == t->format &&
        e->levels == t->levels) {
        int j, pick = -1;
        e->ring_used[e->ring_at] = e->used;
        for (j = 1; j <= e->ring_n && pick < 0; j++) {
            int c = (e->ring_at + j) % e->ring_n;
            if (c != e->ring_at && render_no - e->ring_used[c] > 5) {
                pick = c;
            }
        }
        if (pick >= 0) {
            e->ring_at = (uint8_t)pick, sub = 1;
        } else if (e->ring_n < (int)(sizeof e->ring / sizeof e->ring[0])) {
            GLuint id = e->hwpal ? 0 : pool_take(t->width, t->height, t->format, t->levels);
            e->ring_at = e->ring_n++;
            if (id != 0) {
                e->ring[e->ring_at] = id, sub = 2;
            } else {
                glGenTextures(1, &e->ring[e->ring_at]);
            }
        }
        e->id = e->ring[e->ring_at];
    } else {
        /* another size (or none yet): the slot's textures to the pool (by the size they have), one of the new size
           taken from it or made */
        GLuint id;
        int j;
        for (j = 0; j < e->ring_n; j++) {
            if (e->hwpal) {
                glDeleteTextures(1, &e->ring[j]);
            } else {
                pool_put(e->ring[j], e->w, e->h, e->format, e->levels, e->ring_used[j] > e->used ? e->ring_used[j] : e->used);
            }
        }
        id = pool_take(t->width, t->height, t->format, t->levels);
        if (id != 0) {
            e->ring[0] = id, sub = 2;
        } else {
            glGenTextures(1, &e->ring[0]);
        }
        memset(e->ring_used, 0, sizeof e->ring_used);
        e->ring_n = 1, e->ring_at = 0, e->id = e->ring[0];
    }
    if (sub == 1) {
        perf.up_sub++;
    } else if (sub == 2) {
        perf.up_pool++;
    } else {
        perf.up_made++;
    }
    e->wrap_s = e->wrap_t = e->filter = -1;
    glBindTexture(GL_TEXTURE_2D, e->id);
    cur.tex = e->id;
    for (k = 0; k <= t->levels && k < 11; k++) {
        int w = t->width >> k, h = t->height >> k;
        const void* src = k == 0 ? t->texels : t->mip[k];
        w = w < 1 ? 1 : w;
        h = h < 1 ? 1 : h;
        if (src == NULL) {
            break;
        }
        {
            uint64_t g0 = now_us();
            if (sub) {
                glTexSubImage2D(GL_TEXTURE_2D, k, 0, 0, w, h, fmt, type, convert(t->format, src, w * h));
            } else {
                glTexImage2D(GL_TEXTURE_2D, k, fmt, w, h, 0, fmt, type, convert(t->format, src, w * h));
            }
            gl_step("a texture's upload", g0);
        }
        bytes += w * h * (is_pal(t->format) ? 1 : 2);
    }
    if (frame_up_no != render_no) {
        frame_up_no = render_no, frame_up_us = 0;
    }
    frame_up_us += now_us() - t0;
    e->hwpal = 0;
    if (hwpal > 0 && is_pal(t->format)) {
        SceGxmTexture* g = vglGetGxmTexture(GL_TEXTURE_2D);
        int r = g != NULL ? sceGxmTextureSetFormat(g, SCE_GXM_TEXTURE_FORMAT_P8_ABGR) : -1;
        static unsigned made, failed;
        if (r == 0) {
            e->hwpal = 1;
            made++;
        } else if (failed++ < 4) {
            host_log("vgl: a palette's texture not made P8 (%08X, descriptor %p)", r, (void*)g);
        }
        if (made == 1 || made == 100 || made == 1000) {
            host_log("vgl: %u palette textures made P8 (%u not)", made, failed);
        }
    }
    e->serial = t->serial;
    e->up_no = render_no;
    e->w = t->width, e->h = t->height, e->format = t->format, e->levels = k - 1;
    e->filter = -1;             /* glTexImage2D may reset the sampler: set again at the next draw */
    e->wrap_s = e->wrap_t = -1;
    perf.uploads++;
    perf.upload_kb += (unsigned)(bytes >> 10);
    perf.tex_us += now_us() - t0;
}

/* the texture as its frame froze it, uploaded when its texels changed */
static TEX* tex_get(const SNAP* sn) {
    TEX* e = tex_find(sn->orig);
    if (e == NULL || sn->t.texels == NULL || sn->t.width <= 0 || sn->t.height <= 0) {
        return NULL;
    }
    if (e->key != sn->orig) {
        memset(e, 0, sizeof *e);
        e->key = sn->orig;
        e->serial = ~sn->t.serial;
    }
    if (e->serial != sn->t.serial || e->w != sn->t.width || e->h != sn->t.height || e->format != sn->t.format) {
        if (tex_frame_us == 1) {
            const char* v = getenv("MVC2_TEX_FRAME_US");
            tex_frame_us = v != NULL ? (unsigned)atoi(v) : 0u;   /* none: a tile left a frame late showed another
                                                                    sprite's squares (Abyss's entrance, its slots
                                                                    passed from sprite to sprite each frame) */
        }
        /* only a tile that changed in the last two frames (a sprite's animation: its last texels are its frame
           before) and that was not late the frame before. A tile taken over by another sprite (still for a while,
           then changed) left late showed the sprite it held before, squares of the wrong sprite; one left late
           frame after frame stayed wrong */
        if (tex_frame_us != 0 && e->ring_n > 0 && frame_up_no == render_no && frame_up_us > tex_frame_us &&
            e->w == sn->t.width && e->h == sn->t.height && e->format == sn->t.format && e->levels == sn->t.levels &&
            render_no - e->up_no <= 2 && e->late_no != render_no - 1) {
            e->used = render_no;    /* the frame's budget spent: its last texels this frame, sent the next */
            e->late_no = render_no;
            return e;
        }
        tex_upload(e, &sn->t);
    }
    e->used = render_no;
    return e;
}

/* a sixteenth of the table a frame, not all of it now and then (that was a missed frame) */
static void tex_sweep(void) {
    static int at;
    int i, end = at + TEX_SLOTS / 16;
    for (i = at; i < end; i++) {
        TEX* t = &texes[i];
        if (t->key != NULL && t->key != (const RD_TEXTURE*)1 && render_no - t->used > TEX_IDLE) {
            int j;
            for (j = 0; j < t->ring_n; j++) {
                if (t->hwpal) {
                    glDeleteTextures(1, &t->ring[j]);
                } else {
                    pool_put(t->ring[j], t->w, t->h, t->format, t->levels, t->used);
                }
            }
            memset(t, 0, sizeof *t);
            t->key = (const RD_TEXTURE*)1;
        }
    }
    at = end % TEX_SLOTS;
}

/* ---- depth ---- */

static int z_ok(float z) {
    return z > 0.0f && z < 3.0e38f;
}

/* every vertex's depth, by list (vbase), then the background's three */
static int depth_room(int n) {
    if (n > zroom) {
        free(vdepth);
        zroom = n + n / 2;
        vdepth = malloc((size_t)zroom * 4);
        if (vdepth == NULL) {
            zroom = 0;
        }
    }
    return zroom != 0;
}

/* d = 1/w times a power of two (depth_scale), the largest just under 1: exact (two layers a float's step apart stay
   apart), and linear in 1/w as rd_soft compares it, so a triangle's d interpolates across it as its 1/w does and
   overlapping slanted layers meet where they should (a mapping by rank bent them: the title's water showed its
   triangles). vdepth holds 1/w (0 where it is not drawn); the power of two is applied as a vertex is made (exactly
   the same: a multiply by a power of two does not round), not in a pass of its own */
static float depth_scale = 1.0f;

/* the triangles g0..g1-1 counted through the lists (the background's three corners last), their 1/w in vdepth; their
   largest 1/w */
static float depth_part(const RD_FRAME* f, int g0, int g1) {
    float zmax = 0.0f;
    int l, g = 0;
    for (l = 0; l <= RD_LISTS && g < g1; l++) {
        int nt = l < RD_LISTS ? f->ntris[l] : 1, i = g0 > g ? g0 - g : 0;
        float* out = &vdepth[vbase[l]];
        for (; i < nt && g + i < g1; i++) {
            const RD_VTX* v3 = l < RD_LISTS ? f->tris[l][i].v : f->background;
            if (l < RD_LISTS && i + 6 < nt) {
                __builtin_prefetch(&f->tris[l][i + 6]);     /* the frame read cold: a line at a time without */
            }
            int k;
            for (k = 0; k < 3; k++) {
                float z = v3[k].z;
                if (z_ok(z)) {
                    zmax = z > zmax ? z : zmax;
                    out[i * 3 + k] = z;
                } else {
                    out[i * 3 + k] = 0.0f;
                }
            }
        }
        g += nt;
    }
    return zmax;
}

static int cut_go_ready(void);
static float depth_work_max;
static void cut_run(int kind, const RD_FRAME* f, int from, int to);
static void cut_wait(void);

static void depth_table_split(const RD_FRAME* f, int split);
static void depth_table(const RD_FRAME* f) {
    depth_table_split(f, 1);
}

/* split: its second half on the cutting thread (not when it runs there itself) */
static void depth_table_split(const RD_FRAME* f, int split) {
    int l, n = 0, g = 0;
    float zmax;
    for (l = 0; l < RD_LISTS; l++) {
        vbase[l] = n;
        n += f->ntris[l] * 3;
        g += f->ntris[l];
    }
    vbase[RD_LISTS] = n;
    n += 3;
    g += 1;
    nz = 0;
    if (!depth_room(n)) {
        return;
    }
    if (split && cut_go_ready() && g >= 1024) {
        /* the second half on the cutting thread */
        cut_run(3, f, g / 2, g);
        zmax = depth_part(f, 0, g / 2);
        cut_wait();
        zmax = depth_work_max > zmax ? depth_work_max : zmax;
    } else {
        zmax = depth_part(f, 0, g);
    }
    {
        int e;
        frexpf(zmax > 0.0f ? zmax : 1.0f, &e);
        depth_scale = ldexpf(1.0f, -e);
    }
    nz = n;
}

/* ---- the frame's vertices and batches ---- */

static float view_x0, view_w;   /* the game's 640 by 480 on the screen */
/* ASPECT_RATIO (env.txt): ORIGINAL the game's 4:3 between pillars; STRETCHED its 640 across the screen; WIDE the
   whole screen at the 4:3's scale, the game's x from game_x0 for game_w (-103.5 to 743.5): the 640 in the middle,
   what the game draws past its sides seen in the rest */
enum { ASPECT_ORIGINAL, ASPECT_STRETCHED, ASPECT_WIDE };
static int aspect_mode = ASPECT_WIDE;
static float game_x0 = 0.0f, game_w = RD_WIDTH;
static float ndc_kx = 2.0f / RD_WIDTH, ndc_bx = -1.0f;    /* x in the game's pixels to the viewport's -1..1 */

/* d goes to the depth buffer as it is (glDepthRange -1 to 1, a float buffer). A level triangle (a sprite: one 1/w at
   its three corners) has nothing to correct for perspective: drawn with w 1, d is not multiplied then divided by w,
   which could round two layers the game puts a float's step apart into one */
static int depth_mode;  /* MVC2_DEPTHMODE: 0 d as it is (depth range -1 to 1); 1 2 d - 1 (range 0 to 1), w kept */

static void put_vtx(VTX* o, const RD_VTX* v, const RD_VTX* colours, const RD_STATE* s, float d, const float* cl,
                    int level) {
    float w = 1.0f / v->z, pw = level && depth_mode == 0 ? 1.0f : w;
    d *= depth_scale;
    uint32_t b = colours->base, k = s->offset ? colours->offset : 0;
    o->x = (v->x * ndc_kx + ndc_bx) * pw;
    o->y = (1.0f - v->y * (2.0f / RD_HEIGHT)) * pw;
    o->z = (depth_mode == 1 ? 2.0f * d - 1.0f : depth_mode == 2 ? 0.5f + 0.5f * d : d) * pw;
    o->w = pw;
    o->fw = w;
    o->u = v->u;
    o->v = v->v;
    o->base[0] = (uint8_t)(b >> 16), o->base[1] = (uint8_t)(b >> 8), o->base[2] = (uint8_t)b;
    o->base[3] = s->use_alpha ? (uint8_t)(b >> 24) : 255;
    o->offs[0] = (uint8_t)(k >> 16), o->offs[1] = (uint8_t)(k >> 8), o->offs[2] = (uint8_t)k, o->offs[3] = 0;
    memcpy(o->clamp, cl, sizeof o->clamp);
}

/* rd_soft's setup_tri: what it leaves undrawn */
static int tri_drawn(const RD_STATE* s, const RD_TRI* t) {
    const RD_VTX *a = &t->v[0], *b = &t->v[1], *c = &t->v[2];
    double area = ((double)b->x - a->x) * ((double)c->y - a->y) - ((double)b->y - a->y) * ((double)c->x - a->x);
    if (area == 0.0 || !(area == area) || (s->cull != 0 && fabs(area) < RD_CULL_VAL) || (s->cull == 2 && area < 0) ||
        (s->cull == 3 && area > 0)) {
        return 0;
    }
    return z_ok(a->z) && z_ok(b->z) && z_ok(c->z);
}

static SNAP* snap_of(const RD_STATE* s) {
    return (SNAP*)s->tex;       /* the hand-over pointed it at its snapshot */
}

typedef struct {
    RD_VTX v;
    float d;
} PV;

/* a to b by the corners' weights over w (ka, kb: each corner's 1/w times its share, over the point's) */
static inline uint32_t lerp_colour(uint32_t a, uint32_t b, float ka, float kb) {
    uint32_t out = 0;
    int sh;
    if (a == b) {
        return a;
    }
    for (sh = 0; sh < 32; sh += 8) {
        int c = (int)((float)(a >> sh & 255) * ka + (float)(b >> sh & 255) * kb + 0.5f);
        out |= (uint32_t)(c < 0 ? 0 : c > 255 ? 255 : c) << sh;
    }
    return out;
}

/* the point t of the way from a to b on the screen: x, y, 1/w and the depth straight, the rest over w (one
   division for all of it) */
static PV lerp_pv(const PV* a, const PV* b, float t) {
    PV o;
    float z = a->v.z + (b->v.z - a->v.z) * t, iz = 1.0f / z;
    float ka = a->v.z * (1.0f - t) * iz, kb = b->v.z * t * iz;
    o.v.x = a->v.x + (b->v.x - a->v.x) * t;
    o.v.y = a->v.y + (b->v.y - a->v.y) * t;
    o.v.z = z;
    o.v.u = a->v.u * ka + b->v.u * kb;
    o.v.v = a->v.v * ka + b->v.v * kb;
    o.v.base = lerp_colour(a->v.base, b->v.base, ka, kb);
    o.v.offset = lerp_colour(a->v.offset, b->v.offset, ka, kb);
    o.d = a->d + (b->d - a->d) * t;
    return o;
}

/* the point of a to b where 1/w is z */
static PV cut_at(const PV* a, const PV* b, float z) {
    return lerp_pv(a, b, (z - a->v.z) / (b->v.z - a->v.z));
}

/*
 * A triangle reaching far past the screen (a corner cut at the near plane can be millions of pixels off) cut to a
 * guard band of four times the game's screen each way, for the float's precision: the GXM does not clip, and its tiler pays for such a triangle's
 * reach. What is cut away is past the screen; what stays is interpolated as rd_soft would (over w).
 */
#define GUARD_X0 (-RD_WIDTH * 4.0f)
#define GUARD_X1 (RD_WIDTH * 5.0f)
#define GUARD_Y0 (-RD_HEIGHT * 4.0f)
#define GUARD_Y1 (RD_HEIGHT * 5.0f)


static int clip_edge(const PV* in, int n, PV* out, int axis, float edge, int keep_above) {
    int k, m = 0;
    for (k = 0; k < n && m < 14; k++) {
        const PV* a = &in[k];
        const PV* b = &in[(k + 1) % n];
        float va = axis ? a->v.y : a->v.x, vb = axis ? b->v.y : b->v.x;
        int ina = keep_above ? va >= edge : va <= edge, inb = keep_above ? vb >= edge : vb <= edge;
        if (ina) {
            out[m++] = *a;
        }
        if (ina != inb) {
            out[m++] = lerp_pv(a, b, (edge - va) / (vb - va));
        }
    }
    return m;
}

static void add_tri_raw(VBUF* o, const RD_FRAME* f, int list, const RD_TRI* t, const float* d);

static int add_tri_guarded(VBUF* o, const RD_FRAME* f, int list, const RD_TRI* t, const float* d) {
    PV a[16], b[16];
    RD_TRI piece;
    float pd[3];
    int n = 3, k;
    for (k = 0; k < 3; k++) {
        const RD_VTX* v = &t->v[k];
        if (v->x < GUARD_X0 || v->x > GUARD_X1 || v->y < GUARD_Y0 || v->y > GUARD_Y1) {
            break;
        }
    }
    if (k == 3) {
        return 0;
    }
    for (k = 0; k < 3; k++) {
        a[k].v = t->v[k];
        a[k].d = d[k];
        if (f->states[t->state].flat) {
            a[k].v.base = t->v[2].base, a[k].v.offset = t->v[2].offset;
        }
    }
    n = clip_edge(a, n, b, 0, GUARD_X0, 1);
    n = clip_edge(b, n, a, 0, GUARD_X1, 0);
    n = clip_edge(a, n, b, 1, GUARD_Y0, 1);
    n = clip_edge(b, n, a, 1, GUARD_Y1, 0);
    guard_cuts++;
    piece = *t;
    for (k = 1; k + 1 < n; k++) {
        piece.v[0] = a[0].v, piece.v[1] = a[k].v, piece.v[2] = a[k + 1].v;
        pd[0] = a[0].d, pd[1] = a[k].d, pd[2] = a[k + 1].d;
        add_tri_raw(o, f, list, &piece, pd);
    }
    return 1;
}

static void add_tri_v(VBUF* o, const RD_FRAME* f, int list, const RD_TRI* t, const float* d) {
    if (!add_tri_guarded(o, f, list, t, d)) {
        add_tri_raw(o, f, list, t, d);
    }
}

/*
 * Sprites drawn larger than the game's 640 by 480 (here 1.133 times): two tiles the game puts side by side, one
 * ending at 100.4 and the next starting at 100.6, cover the same pixels there (by their centres), and none is left
 * between; scaled, the fifth of a pixel between them can hold a pixel's centre, a line through the sprite. And a
 * pixel at a tile's edge, its centre past the tile's last texel's, reads the texel beside it in the texture: another
 * tile's. As rd_gpu.c (the PC's) does: a sprite's corners (textured, its 1/w level) on a sixteenth of a game pixel
 * then on the whole pixel whose edge rd_soft's coverage gives it, u and v moved with them along the sprite's own
 * plane; and its texels read only between its own u and v, half a texel in. MVC2_SEAMS=0: neither.
 */
static const float no_clamp[4] = { -16384.0f, -16384.0f, 16384.0f, 16384.0f };   /* within a half float: a varying may be one */
static int seams_fix = -1;

/* floor of v (|v| well under 2^31) by the FPU's conversion, not libm's floorf: a call each, sixteen a sprite's
   triangle, had tripled the opaque list's time on the menu */
static inline int floor_i(float v) {
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}

/* the game pixel whose edge rd_soft's coverage gives a corner at x: on a sixteenth first (neighbours a float's
   rounding apart then agree), then the nearest whole one */
static inline float pixel_edge(float x) {
    if (!(x > -65536.0f && x < 65536.0f)) {
        return x;
    }
    return (float)((floor_i(x * 16.0f + 0.5f) + 8) >> 4);
}

static inline float min3f(float a, float b, float c) {
    float m = a < b ? a : b;
    return m < c ? m : c;
}

static inline float max3f(float a, float b, float c) {
    float m = a > b ? a : b;
    return m > c ? m : c;
}

/* 2 when t is a sprite put on whole game pixels in out, 1 a sprite already on them (out not written), 0 not a
   sprite; cl its texels' bounds */
static int sprite_fit(VBUF* o, const RD_STATE* s, const RD_TRI* t, RD_TRI* out, float* cl) {
    const RD_VTX *a = &t->v[0], *b = &t->v[1], *c = &t->v[2];
    float area, dux = 0, duy = 0, dvx = 0, dvy = 0;
    int k, slopes = 0, moved = 0;
    if (s->tex == NULL || a->z != b->z || b->z != c->z || s->tex->width <= 0 || s->tex->height <= 0) {
        return 0;
    }
    for (k = 0; k < 3; k++) {
        const RD_VTX* o = &t->v[k];
        RD_VTX* v = &out->v[k];
        float x = pixel_edge(o->x), y = pixel_edge(o->y);
        if (x != o->x || y != o->y) {
            float mx = x - o->x, my = y - o->y;
            if (!moved) {
                *out = *t;
                moved = 1;
            }
            if (!slopes) {
                area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
                if (area == 0.0f) {
                    return 0;
                }
                area = 1.0f / area;
                dux = ((b->u - a->u) * (c->y - a->y) - (c->u - a->u) * (b->y - a->y)) * area;
                duy = ((c->u - a->u) * (b->x - a->x) - (b->u - a->u) * (c->x - a->x)) * area;
                dvx = ((b->v - a->v) * (c->y - a->y) - (c->v - a->v) * (b->y - a->y)) * area;
                dvy = ((c->v - a->v) * (b->x - a->x) - (b->v - a->v) * (c->x - a->x)) * area;
                slopes = 1;
            }
            v->u += dux * mx + duy * my;
            v->v += dvx * mx + dvy * my;
            v->x = x, v->y = y;
        }
    }
    if (s->tex->width != o->last_w || s->tex->height != o->last_h) {   /* by size: a snapshot's slot changes */
        o->last_w = s->tex->width, o->last_h = s->tex->height;
        o->iw = 0.5f / (float)o->last_w, o->ih = 0.5f / (float)o->last_h;
    }
    cl[0] = min3f(a->u, b->u, c->u) + o->iw, cl[1] = min3f(a->v, b->v, c->v) + o->ih;
    cl[2] = max3f(a->u, b->u, c->u) - o->iw, cl[3] = max3f(a->v, b->v, c->v) - o->ih;
    if (cl[0] > cl[2]) {
        cl[0] = cl[2] = (cl[0] + cl[2]) * 0.5f;
    }
    if (cl[1] > cl[3]) {
        cl[1] = cl[3] = (cl[1] + cl[3]) * 0.5f;
    }
    return moved ? 2 : 1;
}

static void add_tri_raw(VBUF* o, const RD_FRAME* f, int list, const RD_TRI* t, const float* d) {
    const RD_STATE* s = &f->states[t->state];
    BATCH* b = *o->nb > 0 ? &o->b[*o->nb - 1] : NULL;
    const float* cl = no_clamp;
    float fit_cl[4];
    RD_TRI fitted;
    int k, level, nv = *o->nv;
    if (nv + 3 > o->vmax) {
        return;
    }
    if (seams_fix) {
        int fit = sprite_fit(o, s, t, &fitted, fit_cl);
        if (fit) {
            cl = fit_cl;
            t = fit == 2 ? &fitted : t;
        }
    }
    level = t->v[0].z == t->v[1].z && t->v[1].z == t->v[2].z;
    if (b == NULL || b->list != list || !rd_same_state(&b->s, s)) {
        if (*o->nb == o->bmax) {
            return;
        }
        b = &o->b[(*o->nb)++];
        b->s = *s;
        b->list = list;
        b->first = nv;
        b->count = 0;
        b->clamp = b->offs = 0;
        b->tex = NULL;          /* tex_resolve, on the render thread */
    }
    b->clamp |= cl != no_clamp;
    if (s->offset) {
        const RD_TRI* ct = s->flat ? NULL : t;
        b->offs |= ct != NULL ? (t->v[0].offset | t->v[1].offset | t->v[2].offset) != 0 : t->v[2].offset != 0;
    }
    for (k = 0; k < 3; k++) {
        put_vtx(&o->v[nv + k], &t->v[k], s->flat ? &t->v[2] : &t->v[k], s, d[k], cl, level);
    }
    *o->nv = nv + 3;
    b->count += 3;
}

static void add_tri(const RD_FRAME* f, int list, int index) {
    add_tri_v(&vb_main, f, list, &f->tris[list][index], &vdepth[vbase[list] + index * 3]);
}

/* the batches' textures, uploaded where their texels changed: GL, so here on the render thread */
static void tex_resolve(int from) {
    int i;
    for (i = from; i < nbatches; i++) {
        BATCH* b = &batches[i];
        b->tex = b->s.tex != NULL ? tex_get(snap_of(&b->s)) : NULL;
    }
}

/*
 * The translucent list. rd_soft sorts it by pixel; here by triangle, far to near, in the order sent where equal,
 * after a deep triangle (a plane seen at a slant, a cloud layer reaching the near plane) is cut along the 1/w of
 * each shallower one that lies within its depths and over its pixels: each piece then lies on one side of each such
 * layer, and its middle's 1/w sorts it where rd_soft's pixels have it. The cuts follow lines of equal 1/w on the
 * screen (1/w is linear there), and a cut's new corner takes u, v and the colours over w, as rd_soft interpolates.
 */
typedef struct {
    const RD_TRI* t;    /* the frame's triangle, or a cut piece's in cut_pool */
    const float* d;     /* its corners' depths */
    float key;          /* the middle's 1/w */
    float zlo, zhi;     /* its corners' least and most 1/w */
    int adds;           /* its state adds (dst ONE): its order among others that add changes nothing */
} PIECE;

/* the pieces deep triangles are cut into, a fixed pool so that the pieces' pointers stay good */
#define CUT_POOL 16384
typedef struct {
    RD_TRI t;
    float d[3];
} CUT;

/* the pieces a thread makes: the render thread's (pcx[0]) and, for the second half of a frame's deep triangles,
   the cutting thread's on the third core (pcx[1]); a thread's pieces are appended in the triangles' order, so the
   two halves joined are the pieces one thread would have made */
typedef struct {
    PIECE* p;
    int n, room;
    CUT* pool;
    int npool;
    int cand[4096];             /* find_cuts' candidates */
    unsigned stamp;             /* its marks in cand_seen: odd the render thread's, even the cutting thread's */
    unsigned candidates, cut;   /* for the log */
} PCTX;
static PCTX pcx[2];

typedef struct {
    float zlo, zhi, zmid;
    float x0, y0, x1, y1;
    int deep, shallow, drawn;
} TINFO;

static PIECE* pieces;           /* the frame's, pcx[0]'s once trans_list has made them */
static int npieces;
static uint8_t state_adds[RD_MAX_STATES];   /* by the frame's state: dst ONE, src not the frame's colour */
static TINFO* tinfo;
static int tinfo_room;
static uint32_t *pkey, *pkey2;
static int *pidx, *pidx2, pkey_room;

static PIECE* new_piece(PCTX* x) {
    if (x->n == x->room) {
        int room = x->room ? x->room * 2 : 4096;
        PIECE* p = realloc(x->p, sizeof *p * (size_t)room);
        if (p == NULL) {
            return NULL;
        }
        x->p = p;
        x->room = room;
    }
    return &x->p[x->n++];
}

static void emit_poly(PCTX* x, const PV* p, int n, uint16_t state) {
    int k;
    float zsum = 0.0f;
    for (k = 0; k < n; k++) {
        zsum += p[k].v.z;
    }
    for (k = 1; k + 1 < n; k++) {
        PIECE* q;
        CUT* c;
        if (x->pool == NULL && (x->pool = malloc(sizeof(CUT) * CUT_POOL)) == NULL) {
            return;
        }
        if (x->npool == CUT_POOL || (q = new_piece(x)) == NULL) {
            return;
        }
        c = &x->pool[x->npool++];
        c->t.state = state;
        c->t.share = 0;
        c->t.stays = 1;
        c->t.v[0] = p[0].v, c->t.v[1] = p[k].v, c->t.v[2] = p[k + 1].v;
        c->d[0] = p[0].d, c->d[1] = p[k].d, c->d[2] = p[k + 1].d;
        q->t = &c->t;
        q->d = c->d;
        q->key = zsum / (float)n;
        q->adds = state_adds[state];
        q->zlo = q->zhi = c->t.v[0].z;
        q->zlo = c->t.v[1].z < q->zlo ? c->t.v[1].z : q->zlo, q->zhi = c->t.v[1].z > q->zhi ? c->t.v[1].z : q->zhi;
        q->zlo = c->t.v[2].z < q->zlo ? c->t.v[2].z : q->zlo, q->zhi = c->t.v[2].z > q->zhi ? c->t.v[2].z : q->zhi;
    }
}

#define MAX_CUTS 12

/* the shallow triangles' indices, by their middles' 1/w: built once a frame, searched for each deep one */
static int* shallow_by_z;
static int nshallow, shallow_room;

static int shallow_cmp(const void* a, const void* b) {
    float x = tinfo[*(const int*)a].zmid, y = tinfo[*(const int*)b].zmid;
    return x < y ? -1 : x > y;
}

/*
 * The shallow triangles by place too: the game's screen in GRID_X by GRID_Y cells, each listing (in the order of
 * shallow_by_z, so by their middles' 1/w) those whose bounds reach it. Two bounds that overlap share the cell of the
 * corner (the larger x0, the larger y0) of their overlap, so a deep triangle's cuts are among its cells' lists; a
 * shallow one in several of them comes up again with the same 1/w and is passed over then as the cut before. Their
 * candidates merged by 1/w give the cuts the whole list gave, without looking at the screen's other side.
 * MVC2_CUTGRID=0: the whole list.
 */
#define GRID_X 10
#define GRID_Y 8
static int cut_grid = -1;
static int grid_start[GRID_X * GRID_Y + 1], *grid_list, grid_room, grid_ok;   /* grid_ok: built for this frame */

/* the shallow ones by place in shallow_by_z, packed: middle 1/w, bounds (read in order, no pointer chasing) */
typedef struct {
    float x0, y0, x1, y1;
} BOX;
static float* sh_z;
static BOX* sh_box;
static float* grid_z;           /* grid_list's entries' middle 1/w, beside it */
static int sh_room;

static inline int grid_cx(float x) {
    int c = x > 0.0f ? (int)(x * (GRID_X / (float)RD_WIDTH)) : 0;
    return c < GRID_X ? c : GRID_X - 1;
}

static inline int grid_cy(float y) {
    int c = y > 0.0f ? (int)(y * (GRID_Y / (float)RD_HEIGHT)) : 0;
    return c < GRID_Y ? c : GRID_Y - 1;
}

/* the cells' lists, two passes (count, then fill) over shallow_by_z */
static void grid_build(void) {
    int j, total = 0, c;
    memset(grid_start, 0, sizeof grid_start);
    for (j = 0; j < nshallow; j++) {
        const BOX* o = &sh_box[j];
        int x0 = grid_cx(o->x0), x1 = grid_cx(o->x1), y0 = grid_cy(o->y0), y1 = grid_cy(o->y1), x, y;
        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                grid_start[y * GRID_X + x + 1]++;
            }
        }
        total += (x1 - x0 + 1) * (y1 - y0 + 1);
    }
    if (total > grid_room) {
        free(grid_list);
        free(grid_z);
        grid_room = total + total / 2;
        grid_list = malloc(sizeof(int) * (size_t)grid_room);
        grid_z = malloc(sizeof(float) * (size_t)grid_room);
        if (grid_list == NULL || grid_z == NULL) {
            free(grid_list), free(grid_z);
            grid_list = NULL, grid_z = NULL;
            grid_room = 0;
            return;
        }
    }
    for (c = 0; c < GRID_X * GRID_Y; c++) {
        grid_start[c + 1] += grid_start[c];
    }
    {
        static int fill_at[GRID_X * GRID_Y];
        memcpy(fill_at, grid_start, sizeof fill_at);
        for (j = 0; j < nshallow; j++) {
            const BOX* o = &sh_box[j];
            int x0 = grid_cx(o->x0), x1 = grid_cx(o->x1), y0 = grid_cy(o->y0), y1 = grid_cy(o->y1), x, y;
            for (y = y0; y <= y1; y++) {
                for (x = x0; x <= x1; x++) {
                    grid_z[fill_at[y * GRID_X + x]] = sh_z[j];
                    grid_list[fill_at[y * GRID_X + x]++] = j;   /* the place in shallow_by_z: its order by 1/w */
                }
            }
        }
    }
}

/* insertion sort: the candidates are few and nearly in order (each cell's list is) */
static void sort_ints(int* v, int n) {
    int i, j;
    for (i = 1; i < n; i++) {
        int x = v[i];
        for (j = i; j > 0 && v[j - 1] > x; j--) {
            v[j] = v[j - 1];
        }
        v[j] = x;
    }
}
static unsigned* cand_seen;     /* by place in shallow_by_z: the deep triangle that last took it (no repeats) */
static int cand_seen_room;


/* the first place in z[from, to) whose value is past v (above: or at it, when at is 0) */
static inline int first_past(const float* z, int from, int to, float v, int at) {
    while (from < to) {
        int mid = (from + to) >> 1;
        if (at ? z[mid] < v : z[mid] <= v) {
            from = mid + 1;
        } else {
            to = mid;
        }
    }
    return from;
}

static inline int box_apart(const BOX* o, const TINFO* me) {
    return o->x1 < me->x0 || o->x0 > me->x1 || o->y1 < me->y0 || o->y0 > me->y1;
}

/* the cuts of deep triangle i: shallow middles past its zlo and before its zhi, over its pixels, a hundredth
   apart at least, the first MAX_CUTS of them */
static int find_cuts(PCTX* pc, int i, float* cuts) {
    const TINFO* me = &tinfo[i];
    int ncuts = 0, j, lo = first_past(sh_z, 0, nshallow, me->zlo, 0);
    if (grid_ok) {
        /* the shallow ones within its depths scanned straight when they are few beside its cells (a big
           triangle over many cells, or a narrow band of depths); else its cells' */
        int hi = first_past(sh_z, lo, nshallow, me->zhi, 1);
        int x0 = grid_cx(me->x0), x1 = grid_cx(me->x1), y0 = grid_cy(me->y0), y1 = grid_cy(me->y1), x, y;
        if (hi - lo > (x1 - x0 + 1) * (y1 - y0 + 1) * 8) {
            int* cand = pc->cand;
            unsigned stamp = (pc->stamp += 2);
            int ncand = 0;
            for (y = y0; y <= y1; y++) {
                for (x = x0; x <= x1; x++) {
                    int c = y * GRID_X + x, e = grid_start[c + 1];
                    int k = first_past(grid_z, grid_start[c], e, me->zlo, 0);
                    for (; k < e && grid_z[k] < me->zhi; k++) {
                        int jj = grid_list[k];
                        pc->candidates++;
                        if (cand_seen[jj] == stamp || box_apart(&sh_box[jj], me)) {
                            continue;
                        }
                        cand_seen[jj] = stamp;
                        if (ncand == (int)(sizeof pc->cand / sizeof pc->cand[0])) {
                            goto whole;     /* too many: the whole list instead */
                        }
                        cand[ncand++] = jj;
                    }
                }
            }
            sort_ints(cand, ncand);
            for (j = 0; j < ncand && ncuts < MAX_CUTS; j++) {
                float z = sh_z[cand[j]];
                if (ncuts > 0 && z < cuts[ncuts - 1] * 1.01f) {
                    continue;
                }
                cuts[ncuts++] = z;
            }
            return ncuts;
        }
    }
whole:
    ncuts = 0;
    for (j = lo; j < nshallow && ncuts < MAX_CUTS; j++) {
        float z = sh_z[j];
        if (!(z < me->zhi)) {
            break;
        }
        pc->candidates++;
        if (box_apart(&sh_box[j], me) || (ncuts > 0 && z < cuts[ncuts - 1] * 1.01f)) {
            continue;
        }
        cuts[ncuts++] = z;
    }
    return ncuts;
}

static void cut_deep(PCTX* x, const RD_FRAME* f, int i) {
    const RD_TRI* t = &f->tris[RD_TRANS][i];
    float cuts[MAX_CUTS];
    PV poly[8];
    int ncuts, j, np = 3, c;
    ncuts = find_cuts(x, i, cuts);
    for (j = 0; j < 3; j++) {
        poly[j].v = t->v[j];
        poly[j].d = vdepth[vbase[RD_TRANS] + i * 3 + j];
        if (f->states[t->state].flat) {
            poly[j].v.base = t->v[2].base, poly[j].v.offset = t->v[2].offset;
        }
    }
    if (ncuts == 0) {
        emit_poly(x, poly, 3, t->state);
        return;
    }
    /* slab by slab, far to near: the part at or behind the cut goes out, the rest goes on */
    for (c = 0; c <= ncuts && np >= 3; c++) {
        PV below[8], above[8];
        int nb = 0, na = 0, k;
        if (c == ncuts || (c > 0 && cuts[c] == cuts[c - 1])) {
            if (c == ncuts) {
                emit_poly(x, poly, np, t->state);
            }
            continue;
        }
        for (k = 0; k < np; k++) {
            const PV* a = &poly[k];
            const PV* b = &poly[(k + 1) % np];
            int ina = a->v.z <= cuts[c], inb = b->v.z <= cuts[c];
            if (ina) {
                below[nb++] = *a;
            } else {
                above[na++] = *a;
            }
            if (ina != inb && nb < 8 && na < 8) {
                PV m = cut_at(a, b, cuts[c]);
                below[nb++] = m;
                above[na++] = m;
            }
        }
        if (nb >= 3) {
            emit_poly(x, below, nb, t->state);
        }
        memcpy(poly, above, sizeof(PV) * (size_t)na);
        np = na;
    }
}

static int keys_room(int n) {
    if (n <= pkey_room) {
        return 1;
    }
    free(pkey), free(pkey2), free(pidx), free(pidx2);
    pkey_room = n + n / 2;
    pkey = malloc((size_t)pkey_room * 4), pkey2 = malloc((size_t)pkey_room * 4);
    pidx = malloc((size_t)pkey_room * 4), pidx2 = malloc((size_t)pkey_room * 4);
    if (pkey == NULL || pkey2 == NULL || pidx == NULL || pidx2 == NULL) {
        pkey_room = 0;
        return 0;
    }
    return 1;
}


static void order_fix(const RD_FRAME* f);

/* triangles from..to-1 of the translucent list as pieces: a deep one cut, the rest as they are */
/* the cuts' candidates one half of the list may look at in a frame (MVC2_CUT_MOST): past them its deep triangles are
   drawn whole, sorted by their middles as the stages' are. A screen of effects (Abyss's last fall, a hyper's finish)
   gave 3800 a frame on average and up to 10.8 ms of cutting, the frame over its 16.7 ms; the others look at a few
   hundred */
static unsigned cut_most;
/* the budget only while the frames are heavy: armed for CUT_HOLD frames by a frame that took over CUT_HEAVY_US to
   prepare (at the default 333 MHz the fights' heaviest take 11 ms; Abyss's fall, 15 at 444 MHz and so about 19 here,
   for many frames), and again by each frame in which it had to stop cutting (the screen still that full); a frame
   prepared lighter thanks to it keeps it on */
#define CUT_HEAVY_US 12000u
/* or by two frames running whose preparing and the frame before's drawing (its uploads and GL) passed CUT_BUSY_US
   together: Abyss's last form under a triple hyper's finish prepared in 11.2 ms, but with 1.5 ms of uploads and 5 ms
   of GL the render thread took 17.7, and the frames fell to 30 for a second. A frame that had to stop cutting keeps it
   on only while it is still near the frame's end (CUT_KEEP_US): kept on by that alone, or at 10.5 ms, one heavy
   frame on the clock tower (11 to 13 ms there; its gears' light and shade, translucent, 800 to 1250 candidates every
   frame) held it for good, its light flickering as the pieces cut and whole took turns */
#define CUT_BUSY_US 15000u
#define CUT_KEEP_US 14000u
#define CUT_PERIOD_US 17500u        /* a period past a frame's 16.7 ms: frames late */
#define CUT_PERIOD_KEEP_US 16000u
#define CUT_HOLD 60
static int cut_hold, cut_heavy_run;
static uint64_t last_draw_us;   /* draw_frame's last, from the render loop */
static volatile int cut_capped;

static void make_pieces(PCTX* x, const RD_FRAME* f, int from, int to) {
    int i;
    unsigned most;
    if (cut_most == 0) {
        cut_most = getenv("MVC2_CUT_MOST") != NULL && atoi(getenv("MVC2_CUT_MOST")) > 0 ? (unsigned)atoi(getenv("MVC2_CUT_MOST")) : 500u;
    }
    /* every cut while the frames are light (a hyper's finish, its streaks cut against the fighters, behind them as
       they should be); the budget only after a heavy frame, the one way to keep that 60 */
    most = cut_hold > 0 ? cut_most : 0xffffffffu;
    for (i = from; i < to; i++) {
        if (!tinfo[i].drawn) {
            continue;
        }
        if (tinfo[i].deep && shallow_room && x->candidates >= most) {
            cut_capped = 1;
        }
        if (tinfo[i].deep && shallow_room && x->candidates < most) {
            int before = x->n;
            cut_deep(x, f, i);
            x->cut += x->n - before > 1;
        } else {
            PIECE* q = new_piece(x);
            if (q == NULL) {
                break;
            }
            q->t = &f->tris[RD_TRANS][i];
            q->d = &vdepth[vbase[RD_TRANS] + i * 3];
            /* a deep one left whole by the budget: sorted by its far end, so a backdrop plane reaching toward the
               camera (a hyper's finish, its streaks behind the fighters) is drawn behind the sprites, not through them */
            q->key = tinfo[i].deep ? tinfo[i].zlo : tinfo[i].zmid;
            q->zlo = tinfo[i].zlo, q->zhi = tinfo[i].zhi;
            q->adds = state_adds[q->t->state];
        }
    }
}

/* MVC2_CUTMT=0: the cuts all on the render thread */
static SceUID cut_go = -1, cut_done = -1;
static struct {
    int kind;                   /* 0 pieces from triangles from..to-1, 1 the sorted pieces from..to-1 added */
    const RD_FRAME* f;
    int from, to;
} cut_job;

/* the cutting thread's own vertices and batches, for the translucent list's second half */
#define WORK_VERTS (3 * 24576)
#define WORK_BATCHES 4096
static VTX* work_v;
static BATCH* work_b;
static int work_nv, work_nb;
static VBUF vb_work;

static void add_pieces(VBUF* o, const RD_FRAME* f, int from, int to);
static void trans_pre(const RD_FRAME* f);
static void trans_post(const RD_FRAME* f);
/* 1 while trans_post runs on the cutting thread (job 4): its cuts then all there, none handed back to itself */
static volatile int tl_on_cut;
/* MVC2_TRANS_AWAY (on unless 0): the translucent list's steps, all of them (pre, cuts, sort, order), on the cutting
   thread while the render thread adds the opaque list; off: only pre there, the rest after on the render thread */
static int trans_away = -1;

static int cut_main(SceSize args, void* argp) {
    (void)args, (void)argp;
    for (;;) {
        sceKernelWaitSema(cut_go, 1, NULL);
        switch (cut_job.kind) {
        case 0:
            pcx[1].n = 0, pcx[1].npool = 0;
            make_pieces(&pcx[1], cut_job.f, cut_job.from, cut_job.to);
            break;
        case 1:
            work_nv = work_nb = 0;
            add_pieces(&vb_work, cut_job.f, cut_job.from, cut_job.to);
            break;
        case 2:
            trans_pre(cut_job.f);
            break;
        case 3:
            depth_work_max = depth_part(cut_job.f, cut_job.from, cut_job.to);
            break;
        case 4:
            trans_pre(cut_job.f);
            tl_on_cut = 1;
            trans_post(cut_job.f);
            tl_on_cut = 0;
            break;
        case 5:
            /* a frame just handed over, while the render thread draws the one before: its depths and its translucent
               list's steps (a hyper's finish, the screen nearly all translucent: the render thread had waited 5 ms
               for these, idle, after drawing) */
            depth_table_split(cut_job.f, 0);
            trans_pre(cut_job.f);
            tl_on_cut = 1;
            trans_post(cut_job.f);
            tl_on_cut = 0;
            break;
        }
        sceKernelSignalSema(cut_done, 1);
    }
    return 0;
}

static int cut_go_ready(void) {
    return cut_go >= 0;
}

static void cut_run(int kind, const RD_FRAME* f, int from, int to) {
    cut_job.kind = kind, cut_job.f = f, cut_job.from = from, cut_job.to = to;
    sceKernelSignalSema(cut_go, 1);
}

static void cut_wait(void) {
    sceKernelWaitSema(cut_done, 1, NULL);
}

static void cut_thread_start(void) {
    SceUID th;
    if (getenv("MVC2_CUTMT") != NULL && atoi(getenv("MVC2_CUTMT")) == 0) {
        return;
    }
    work_v = malloc(sizeof(VTX) * WORK_VERTS);
    work_b = malloc(sizeof(BATCH) * WORK_BATCHES);
    if (work_v == NULL || work_b == NULL) {
        host_log("vgl: no memory for the cutting thread: the cuts on the render thread");
        return;
    }
    vb_work = (VBUF){ work_v, &work_nv, work_b, &work_nb, WORK_VERTS, WORK_BATCHES, 0, 0, 0.0f, 0.0f };
    cut_go = sceKernelCreateSema("vgl_cut_go", 0, 0, 1, NULL);
    cut_done = sceKernelCreateSema("vgl_cut_done", 0, 0, 1, NULL);
    pcx[0].stamp = 1, pcx[1].stamp = 2;
    th = sceKernelCreateThread("vgl_cut", cut_main, 160, 64 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        host_log("vgl: no cutting thread (%08X): the cuts on the render thread", th);
        cut_go = -1;
        return;
    }
    rd_vgl_watch_thread("CUT", th);
}

/*
 * A watchdog (core 2): when the render thread or the game hands over no frame for 3 s, every watched thread's state
 * (status, what it waits on, priority, core, CPU time) to DATA_DIR "hang.txt", written here at once (the log's own
 * thread may be the one held up), and to the log. Once a run.
 */
static unsigned drawing_frame_wd;
static int watchdog_main(SceSize args, void* argp) {
    unsigned last_render = 0, last_presented = 0, still = 0;
    int dumped = 0;
    (void)args, (void)argp;
    for (;;) {
        sceKernelDelayThread(500000);
        if (render_no == last_render || presented == last_presented) {
            still++;
        } else {
            still = 0;
        }
        last_render = render_no, last_presented = presented;
        if (still >= 6 && !dumped && render_no > 60) {
            static char txt[4096];
            int n = 0, i;
            SceUID fd;
            dumped = 1;
            n += snprintf(txt + n, sizeof txt - n, "held up: render frame %u, game frame %u, drawing %u, cut job %d\n",
                          render_no, presented, drawing_frame_wd, cut_job.kind);
            for (i = 0; i < nwatched && n < (int)sizeof txt - 200; i++) {
                SceKernelThreadInfo info;
                memset(&info, 0, sizeof info);
                info.size = sizeof info;
                if (sceKernelGetThreadInfo(watched[i].id, &info) < 0) {
                    n += snprintf(txt + n, sizeof txt - n, "%-6s %08X: no info\n", watched[i].name, watched[i].id);
                    continue;
                }
                n += snprintf(txt + n, sizeof txt - n,
                              "%-6s %08X %-16s status %X wait type %X on %08X prio %d core %d last %d clocks %llu\n",
                              watched[i].name, watched[i].id, info.name, info.status, info.waitType, info.waitId,
                              info.currentPriority, info.currentCpuId, info.lastExecutedCpuId,
                              (unsigned long long)info.runClocks);
            }
            fd = sceIoOpen(DATA_DIR "hang.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
            if (fd >= 0) {
                sceIoWrite(fd, txt, n);
                sceIoClose(fd);
            }
            host_log("vgl: HELD UP for 3 s:\n%s", txt);
        }
    }
    return 0;
}

static void watchdog_start(void) {
    SceUID th;
    if (!vita_debug) {
        return;
    }
    th = sceKernelCreateThread("vgl_watchdog", watchdog_main, 150, 16 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (th >= 0) {
        sceKernelStartThread(th, 0, NULL);
    }
}

/* trans_pre: what each triangle is (its depths, deep or shallow), the bounds and the shallow ones' index; run on
   the cutting thread while the render thread adds the opaque list. trans_post: the pieces, sorted and set right */
static int tl_ok, tl_ndeep;
static uint64_t tl_pre_us[2];   /* info, then the rest */

static void trans_pre(const RD_FRAME* f) {
    int n = f->ntris[RD_TRANS], i, ndeep = 0;
    uint64_t tt0 = now_us(), tt1;
    tl_ok = 0;
    for (i = 0; i < f->nstates; i++) {
        const RD_STATE* st = &f->states[i];
        state_adds[i] = st->dst == 1 && (st->src == 0 || st->src == 1 || st->src == 4 || st->src == 5);
    }
    if (n > tinfo_room) {
        free(tinfo);
        tinfo_room = n + n / 2;
        tinfo = malloc(sizeof *tinfo * (size_t)tinfo_room);
        if (tinfo == NULL) {
            tinfo_room = 0;
            return;
        }
    }
    for (i = 0; i < n; i++) {
        const RD_TRI* t = &f->tris[RD_TRANS][i];
        TINFO* in = &tinfo[i];   /* (trans_pre)*/
        if (i + 6 < n) {
            __builtin_prefetch(&f->tris[RD_TRANS][i + 6]);
        }
        int k;
        in->drawn = tri_drawn(&f->states[t->state], t);
        if (!in->drawn) {
            continue;
        }
        in->zlo = in->zhi = t->v[0].z;
        for (k = 1; k < 3; k++) {
            const RD_VTX* v = &t->v[k];
            in->zlo = v->z < in->zlo ? v->z : in->zlo, in->zhi = v->z > in->zhi ? v->z : in->zhi;
        }
        in->zmid = (t->v[0].z + t->v[1].z + t->v[2].z) * (1.0f / 3.0f);
        /* deep: 1/w more than doubles across it (a plane seen nearly edge on, reaching toward the near plane); a
           shallow one, whose middle cuts the deep: within a fifth (a sprite, a near-flat layer). Between the two,
           as a stage's meshes are, the middle sorts it well enough, and cutting them all costs far more */
        in->deep = in->zhi > in->zlo * 2.0f;
        in->shallow = in->zhi <= in->zlo * 1.2f;
        ndeep += in->deep;
    }
    tt1 = now_us();
    if (n > shallow_room) {
        free(shallow_by_z);
        shallow_room = n + n / 2;
        shallow_by_z = malloc(sizeof(int) * (size_t)shallow_room);
        if (shallow_by_z == NULL) {
            shallow_room = 0;
        }
    }
    /* the bounds on the screen, only wanted for the cuts */
    for (i = 0; i < n && ndeep > 0; i++) {
        const RD_TRI* t = &f->tris[RD_TRANS][i];
        TINFO* in = &tinfo[i];
        int k;
        if (!in->drawn) {
            continue;
        }
        in->x0 = in->x1 = t->v[0].x;
        in->y0 = in->y1 = t->v[0].y;
        for (k = 1; k < 3; k++) {
            const RD_VTX* v = &t->v[k];
            in->x0 = v->x < in->x0 ? v->x : in->x0, in->x1 = v->x > in->x1 ? v->x : in->x1;
            in->y0 = v->y < in->y0 ? v->y : in->y0, in->y1 = v->y > in->y1 ? v->y : in->y1;
        }
    }
    nshallow = 0;
    for (i = 0; i < n && shallow_room && ndeep > 0; i++) {
        if (tinfo[i].drawn && tinfo[i].shallow) {
            shallow_by_z[nshallow++] = i;
        }
    }
    cut_stats[0] += (unsigned)ndeep, cut_stats[1] += (unsigned)nshallow;
    uint64_t ci0 = now_us();
    if (nshallow > 1) {
        /* by their middles' 1/w: a radix sort of its bits (a positive float sorts as its bits), not qsort's
           calls (a thousand shallow ones cost more than a millisecond) */
        static uint32_t *k1, *k2;
        static int *i2, room;
        int pass;
        if (nshallow > room) {
            free(k1), free(k2), free(i2);
            room = nshallow + nshallow / 2;
            k1 = malloc(sizeof *k1 * (size_t)room), k2 = malloc(sizeof *k2 * (size_t)room);
            i2 = malloc(sizeof *i2 * (size_t)room);
            if (k1 == NULL || k2 == NULL || i2 == NULL) {
                room = 0;
            }
        }
        if (room == 0) {
            qsort(shallow_by_z, (size_t)nshallow, sizeof(int), shallow_cmp);
        } else {
            uint32_t *ka = k1, *kb = k2;
            int *ia = shallow_by_z, *ib = i2;
            for (i = 0; i < nshallow; i++) {
                memcpy(&ka[i], &tinfo[ia[i]].zmid, 4);
            }
            for (pass = 0; pass < 4; pass++) {
                int count[257] = { 0 }, sh = pass * 8;
                uint32_t* tk;
                int* ti;
                for (i = 0; i < nshallow; i++) {
                    count[(ka[i] >> sh & 255) + 1]++;
                }
                if (count[(ka[0] >> sh & 255) + 1] == nshallow) {
                    continue;
                }
                for (i = 0; i < 256; i++) {
                    count[i + 1] += count[i];
                }
                for (i = 0; i < nshallow; i++) {
                    int at = count[ka[i] >> sh & 255]++;
                    kb[at] = ka[i];
                    ib[at] = ia[i];
                }
                tk = ka, ka = kb, kb = tk;
                ti = ia, ia = ib, ib = ti;
            }
            if (ia != shallow_by_z) {
                memcpy(shallow_by_z, ia, sizeof(int) * (size_t)nshallow);
            }
        }
    }
    if (nshallow > sh_room) {
        free(sh_z), free(sh_box);
        sh_room = nshallow + nshallow / 2;
        sh_z = malloc(sizeof(float) * (size_t)sh_room);
        sh_box = malloc(sizeof(BOX) * (size_t)sh_room);
        if (sh_z == NULL || sh_box == NULL) {
            free(sh_z), free(sh_box);
            sh_z = NULL, sh_box = NULL;
            sh_room = 0;
            nshallow = 0;   /* no cuts this frame: the middles sort them */
        }
    }
    for (i = 0; i < nshallow; i++) {
        const TINFO* o = &tinfo[shallow_by_z[i]];
        sh_z[i] = o->zmid;
        sh_box[i].x0 = o->x0, sh_box[i].y0 = o->y0, sh_box[i].x1 = o->x1, sh_box[i].y1 = o->y1;
    }
    if (cut_grid < 0) {
        cut_grid = getenv("MVC2_CUTGRID") == NULL || atoi(getenv("MVC2_CUTGRID")) != 0;
    }
    grid_ok = 0;
    if (cut_grid && ndeep > 0 && nshallow > 32) {   /* a few: the whole list is cheaper */
        if (nshallow > cand_seen_room) {
            free(cand_seen);
            cand_seen_room = nshallow + nshallow / 2;
            cand_seen = calloc((size_t)cand_seen_room, sizeof *cand_seen);
            pcx[0].stamp = 1, pcx[1].stamp = 2;
            if (cand_seen == NULL) {
                cand_seen_room = 0;
            }
        }
        if (cand_seen_room > 0) {
            grid_build();
            grid_ok = grid_room > 0;
        }
    }
    cut_us[0] += now_us() - ci0;
    tl_ndeep = ndeep;
    tl_pre_us[0] += tt1 - tt0, tl_pre_us[1] += now_us() - tt1;
    tl_ok = 1;
}

static void trans_post(const RD_FRAME* f) {
    int n = f->ntris[RD_TRANS], i, pass, ndeep = tl_ndeep;
    uint64_t tp0 = now_us(), tt2, tt3;
    npieces = 0;
    if (!tl_ok) {
        return;
    }
    pcx[0].n = 0, pcx[0].npool = 0;
    if (cut_go >= 0 && !tl_on_cut && ndeep >= 32 && nshallow > 0) {
        /* the second half of the deep ones (and the triangles among them) on the cutting thread */
        int half = 0, m;
        for (m = 0; m < n && half * 2 < ndeep; m++) {
            half += tinfo[m].drawn && tinfo[m].deep;
        }
        cut_job.kind = 0, cut_job.f = f, cut_job.from = m, cut_job.to = n;
        uint64_t m0 = now_us(), m1;
        sceKernelSignalSema(cut_go, 1);
        make_pieces(&pcx[0], f, 0, m);
        m1 = now_us();
        sceKernelWaitSema(cut_done, 1, NULL);
        cut_us[1] += m1 - m0, cut_us[2] += now_us() - m1;
        if (pcx[1].n > 0) {
            if (pcx[0].n + pcx[1].n > pcx[0].room) {
                int room = pcx[0].n + pcx[1].n + 1024;
                PIECE* p = realloc(pcx[0].p, sizeof *p * (size_t)room);
                if (p != NULL) {
                    pcx[0].p = p, pcx[0].room = room;
                }
            }
            if (pcx[0].n + pcx[1].n <= pcx[0].room) {
                memcpy(pcx[0].p + pcx[0].n, pcx[1].p, sizeof(PIECE) * (size_t)pcx[1].n);
                pcx[0].n += pcx[1].n;
            }
        }
        cut_stats[2] += pcx[1].candidates, cut_count += pcx[1].cut;
        pcx[1].candidates = pcx[1].cut = 0;
    } else {
        uint64_t m0 = now_us();
        make_pieces(&pcx[0], f, 0, n);
        cut_us[1] += now_us() - m0;
    }
    cut_stats[2] += pcx[0].candidates, cut_count += pcx[0].cut;
    pcx[0].candidates = pcx[0].cut = 0;
    pieces = pcx[0].p, npieces = pcx[0].n;
    tt2 = now_us();
    if (!keys_room(npieces)) {
        return;
    }
    for (i = 0; i < npieces; i++) {
        memcpy(&pkey[i], &pieces[i].key, 4);    /* a positive float sorts as its bits */
        pidx[i] = i;
    }
    /* stable: far (small 1/w) first, then as sent */
    for (pass = 0; pass < 4; pass++) {
        int count[257] = { 0 }, sh = pass * 8;
        uint32_t* tk;
        int* ti;
        for (i = 0; i < npieces; i++) {
            count[(pkey[i] >> sh & 255) + 1]++;
        }
        if (npieces == 0 || count[(pkey[0] >> sh & 255) + 1] == npieces) {
            continue;
        }
        for (i = 0; i < 256; i++) {
            count[i + 1] += count[i];
        }
        for (i = 0; i < npieces; i++) {
            int at = count[pkey[i] >> sh & 255]++;
            pkey2[at] = pkey[i];
            pidx2[at] = pidx[i];
        }
        tk = pkey, pkey = pkey2, pkey2 = tk;
        ti = pidx, pidx = pidx2, pidx2 = ti;
    }
    tt3 = now_us();
    order_fix(f);
    tl_us[0] += tl_pre_us[0], tl_us[1] += tl_pre_us[1] + (tt2 - tp0), tl_us[2] += tt3 - tt2, tl_us[3] += now_us() - tt3,
        tl_us[4] += (unsigned)n;
    tl_pre_us[0] = tl_pre_us[1] = 0;
}

/*
 * The sorted pieces put right where their middles misled: a slanted piece reaching nearer than another over part of
 * its pixels has a nearer middle while lying behind it where they overlap ("Recommend !": the bubble's lower
 * triangle sorted over the text a hair in front of it). Each piece is weighed against the next ORDER_WINDOW whose
 * depths and bounds overlap its own, by both planes at the middle of the overlap, as rd_soft's pixels would; a
 * nearer one drawn first is moved after the other, unless that puts it after one it lies behind (then the other is
 * moved before it, unless that is as wrong: then they stay). Only the blended pieces are weighed, among
 * themselves: those that add (dst ONE) keep their places, their order among others that add changing nothing, and
 * weighing them too (the select screen's sphere: thousands) cost milliseconds a frame.
 */
#define ORDER_WINDOW 12
/*
 * By place in the sorted order, moved with it. The pairs weighed are mostly apart (stage 4's scenery: eleven
 * thousand a frame, a tenth overlapping), and on the Vita's A9 each float compare waits for the VFP (vmrs): about
 * 70 cycles a pair. So the pairs are first sifted with NEON, eight neighbours at a time with no flags: zlo, zhi as
 * ordered integers (oz*) and the bounds in whole pixels, truncated a pixel out so never tighter than the exact ones
 * (ox*, oy*), each in its own array; PINFO's floats are read only for the pairs that may overlap.
 */
#define ORDER_PAD 16            /* the arrays' room past the last entry: the sift reads 16 on */
typedef struct {
    float x0, y0, x1, y1;
    float px, py, pz, a, b;     /* z = pz + a (x - px) + b (y - py), made when first wanted */
    int piece;
    uint8_t moved, plane, level;    /* level: one 1/w all over (a sprite); plane 1 once made */
} PINFO;
static PINFO* pinfo;
static int16_t *ox0, *oy0, *ox1, *oy1;
static int32_t *ozlo, *ozhi;
static int* pplace;             /* the places in the sorted order the blended pieces hold, in order */
static int pinfo_room;

/* a float's bits as an integer in the same order (negative ones too) */
static inline int32_t zkey(float z) {
    int32_t b;
    memcpy(&b, &z, sizeof b);
    return b >= 0 ? b : (int32_t)(b ^ 0x7fffffff);
}

/* the bounds of the piece at place i, floats and whole pixels, in NEON (no compares) */
static void make_bounds(int i) {
    PINFO* p = &pinfo[i];
    const RD_TRI* t = pieces[p->piece].t;
    float32x2_t a = vld1_f32(&t->v[0].x), b = vld1_f32(&t->v[1].x), c = vld1_f32(&t->v[2].x);
    float32x2_t lo = vmin_f32(vmin_f32(a, b), c), hi = vmax_f32(vmax_f32(a, b), c);
    /* truncation toward zero saturates; a pixel out either way covers floor and ceil */
    int32x2_t ilo = vqsub_s32(vcvt_s32_f32(lo), vdup_n_s32(1)), ihi = vqadd_s32(vcvt_s32_f32(hi), vdup_n_s32(1));
    int16x4_t n = vqmovn_s32(vcombine_s32(ilo, ihi));
    vst1_f32(&p->x0, lo);       /* x0, y0 */
    vst1_f32(&p->x1, hi);       /* x1, y1 */
    ox0[i] = vget_lane_s16(n, 0), oy0[i] = vget_lane_s16(n, 1);
    ox1[i] = vget_lane_s16(n, 2), oy1[i] = vget_lane_s16(n, 3);
}

static void make_plane(PINFO* p) {
    const RD_TRI* t = pieces[p->piece].t;
    double ux = t->v[1].x - t->v[0].x, uy = t->v[1].y - t->v[0].y, uz = t->v[1].z - t->v[0].z;
    double vx = t->v[2].x - t->v[0].x, vy = t->v[2].y - t->v[0].y, vz = t->v[2].z - t->v[0].z;
    double det = ux * vy - uy * vx;
    p->px = t->v[0].x, p->py = t->v[0].y, p->pz = t->v[0].z;
    p->a = det != 0.0 ? (float)((uz * vy - uy * vz) / det) : 0.0f;
    p->b = det != 0.0 ? (float)((ux * vz - uz * vx) / det) : 0.0f;
    p->plane = 1;
    order_planes++;
}

/* 1: the entries at places a and b may overlap (depths and whole-pixel bounds); 0: apart */
static inline int may_overlap(int a, int b) {
    int x0 = ox0[a] > ox0[b] ? ox0[a] : ox0[b], x1 = ox1[a] < ox1[b] ? ox1[a] : ox1[b];
    int y0 = oy0[a] > oy0[b] ? oy0[a] : oy0[b], y1 = oy1[a] < oy1[b] ? oy1[a] : oy1[b];
    return (ozlo[b] <= ozhi[a]) & (ozhi[b] >= ozlo[a]) & (x0 < x1) & (y0 < y1);
}

/* the places i+1..i+16 that may overlap the one at i, as bits (bit 0: i+1), eight at a time */
static unsigned sift(int i) {
    static const uint8_t bit[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x8_t bits = vld1_u8(bit);
    int16x8_t ax0 = vdupq_n_s16(ox0[i]), ay0 = vdupq_n_s16(oy0[i]), ax1 = vdupq_n_s16(ox1[i]), ay1 = vdupq_n_s16(oy1[i]);
    int32x4_t azlo = vdupq_n_s32(ozlo[i]), azhi = vdupq_n_s32(ozhi[i]);
    unsigned mask = 0;
    int g;
    for (g = 0; g < 2; g++) {
        int j = i + 1 + g * 8;
        uint16x8_t cx = vcltq_s16(vmaxq_s16(vld1q_s16(&ox0[j]), ax0), vminq_s16(vld1q_s16(&ox1[j]), ax1));
        uint16x8_t cy = vcltq_s16(vmaxq_s16(vld1q_s16(&oy0[j]), ay0), vminq_s16(vld1q_s16(&oy1[j]), ay1));
        uint32x4_t z0 = vandq_u32(vcleq_s32(vld1q_s32(&ozlo[j]), azhi), vcgeq_s32(vld1q_s32(&ozhi[j]), azlo));
        uint32x4_t z1 = vandq_u32(vcleq_s32(vld1q_s32(&ozlo[j + 4]), azhi), vcgeq_s32(vld1q_s32(&ozhi[j + 4]), azlo));
        uint16x8_t c = vandq_u16(vandq_u16(cx, cy), vcombine_u16(vmovn_u32(z0), vmovn_u32(z1)));
        uint8x8_t m = vand_u8(vmovn_u16(c), bits);
        m = vpadd_u8(m, m), m = vpadd_u8(m, m), m = vpadd_u8(m, m);
        mask |= (unsigned)vget_lane_u8(m, 0) << (g * 8);
    }
    return mask;
}

/* 1: p lies in front of q where they overlap (by more than rounding); 0 behind, level, or apart. The exact test,
   for a pair the sift passed */
static int in_front_exact(PINFO* p, PINFO* q) {
    float x0, y0, x1, y1, cx, cy, zp, zq;
    /* two level ones: the sort by 1/w already has them right (the menu's buttons: thousands of pairs a frame) */
    if (p->level & q->level) {
        return 0;
    }
    order_checks++;
    x0 = p->x0 > q->x0 ? p->x0 : q->x0, x1 = p->x1 < q->x1 ? p->x1 : q->x1;
    y0 = p->y0 > q->y0 ? p->y0 : q->y0, y1 = p->y1 < q->y1 ? p->y1 : q->y1;
    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }
    if (!p->plane) {
        make_plane(p);
    }
    if (!q->plane) {
        make_plane(q);
    }
    cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    zp = p->pz + p->a * (cx - p->px) + p->b * (cy - p->py);
    zq = q->pz + q->a * (cx - q->px) + q->b * (cy - q->py);
    return zp > zq + zq * 1e-5f;
}

/* the entries at places a and b: a in front of b */
static int front_at(int a, int b) {
    return may_overlap(a, b) && in_front_exact(&pinfo[a], &pinfo[b]);
}

/* each array's entries at from..to-1 one place on (on 1) or at from+1..to one place back (on 0) */
#define ORDER_SHIFT(arr, from, to, on) \
    memmove(&(arr)[(from) + (on)], &(arr)[(from) + !(on)], sizeof *(arr) * (size_t)((to) - (from)))
#define ORDER_SHIFT_ALL(from, to, on) do { \
        ORDER_SHIFT(pinfo, from, to, on), ORDER_SHIFT(ox0, from, to, on), ORDER_SHIFT(oy0, from, to, on); \
        ORDER_SHIFT(ox1, from, to, on), ORDER_SHIFT(oy1, from, to, on); \
        ORDER_SHIFT(ozlo, from, to, on), ORDER_SHIFT(ozhi, from, to, on); \
    } while (0)

typedef struct {
    PINFO p;
    int16_t x0, y0, x1, y1;
    int32_t zlo, zhi;
} PKEEP;

static void keep_at(PKEEP* k, int i) {
    k->p = pinfo[i], k->x0 = ox0[i], k->y0 = oy0[i], k->x1 = ox1[i], k->y1 = oy1[i], k->zlo = ozlo[i], k->zhi = ozhi[i];
}

static void put_at(const PKEEP* k, int i) {
    pinfo[i] = k->p, ox0[i] = k->x0, oy0[i] = k->y0, ox1[i] = k->x1, oy1[i] = k->y1, ozlo[i] = k->zlo, ozhi[i] = k->zhi;
}

/* the entries at from..to-1 one place on, the one at to put at from */
static void rotate_in(int from, int to) {
    PKEEP k;
    keep_at(&k, to);
    ORDER_SHIFT_ALL(from, to, 1);
    put_at(&k, from);
}

/* the entry at from put at to, those after it one place back */
static void rotate_out(int from, int to) {
    PKEEP k;
    keep_at(&k, from);
    ORDER_SHIFT_ALL(from, to, 0);
    put_at(&k, to);
}

static void order_free(void) {
    free(pinfo), free(pplace), free(ox0), free(oy0), free(ox1), free(oy1), free(ozlo), free(ozhi);
    pinfo = NULL, pplace = NULL, ox0 = oy0 = ox1 = oy1 = NULL, ozlo = ozhi = NULL;
    pinfo_room = 0;
}

/* among the blended (not adding) pieces only, in the places they hold: those that add keep theirs */
static void order_fix(const RD_FRAME* f) {
    int i, j, k, n = 0, budget = 4096;
    uint64_t t0 = now_us();
    (void)f;
    if (npieces > pinfo_room) {
        size_t room;
        order_free();
        pinfo_room = npieces + npieces / 2;
        room = (size_t)pinfo_room + ORDER_PAD;
        pinfo = malloc(sizeof *pinfo * room), pplace = malloc(sizeof(int) * room);
        ox0 = calloc(room, sizeof *ox0), oy0 = calloc(room, sizeof *oy0);
        ox1 = calloc(room, sizeof *ox1), oy1 = calloc(room, sizeof *oy1);
        ozlo = calloc(room, sizeof *ozlo), ozhi = calloc(room, sizeof *ozhi);
        if (!pinfo || !pplace || !ox0 || !oy0 || !ox1 || !oy1 || !ozlo || !ozhi) {
            order_free();
            return;
        }
    }
    for (i = 0; i < npieces; i++) {
        const PIECE* q = &pieces[pidx[i]];
        if (!q->adds) {
            PINFO* p = &pinfo[n];
            ozlo[n] = zkey(q->zlo), ozhi[n] = zkey(q->zhi);
            p->piece = pidx[i];
            p->moved = p->plane = 0;
            p->level = ozlo[n] == ozhi[n];
            make_bounds(n);
            pplace[n++] = i;
        }
    }
    order_adds += (unsigned)(npieces - n);
    order_init_us += now_us() - t0;
    for (i = 0; i < n - 1 && budget > 0; i++) {
        PINFO* p = &pinfo[i];
        int end = i + ORDER_WINDOW < n - 1 ? i + ORDER_WINDOW : n - 1;
        unsigned mask = sift(i) & ((1u << (end - i)) - 1u);   /* bit b: place i + 1 + b, up to end */
        while (mask != 0) {
            PINFO* q;
            int ok;
            j = i + 1 + __builtin_ctz(mask);
            mask &= mask - 1u;
            q = &pinfo[j];
            if (!in_front_exact(p, q)) {
                continue;
            }
            if (p->moved >= 3 || --budget <= 0) {
                break;
            }
            /* p after q: not past one between that p lies behind */
            for (ok = 1, k = i + 1; k < j && ok; k++) {
                ok = !front_at(k, i);
            }
            if (ok) {
                p->moved++;
                rotate_out(i, j);
                order_moves++;
                i--;            /* the one now at i weighed in its turn */
                break;
            }
            /* or q before p: not before one between that lies behind q */
            for (ok = 1, k = i + 1; k < j && ok; k++) {
                ok = !front_at(j, k);
            }
            if (ok) {
                q->moved++;
                rotate_in(i, j);
                order_moves++;
                i--;
                break;
            }
        }
    }
    for (i = 0; i < n; i++) {
        pidx[pplace[i]] = pinfo[i].piece;
    }
    order_us += now_us() - t0;
}

/* everything the drawing needs, out of the handed-over frame, which can then go back to the game */
/* the sorted translucent pieces from..to-1 added (pidx's order) */
static void add_pieces(VBUF* o, const RD_FRAME* f, int from, int to) {
    int i;
    for (i = from; i < to; i++) {
        const PIECE* q = &pieces[pidx[i]];
        /* sorted, the pieces are read in no order: their triangles fetched a few ahead (each a cache miss) */
        if (i + 8 < to) {
            __builtin_prefetch(&pieces[pidx[i + 8]]);
        }
        if (i + 4 < to) {
            const PIECE* r = &pieces[pidx[i + 4]];
            __builtin_prefetch(r->t);
            __builtin_prefetch((const char*)r->t + 64);
            __builtin_prefetch(r->d);
        }
        add_tri_v(o, f, RD_TRANS, q->t, q->d);
    }
}

static void vring_next(void);

/* ASPECT_RATIO=WIDE: a stage's sky is a plane facing the camera far behind everything (a quad at one 1/w, far below
   any model's), made as wide as the 4:3 screen and a little more: the wider screen showed the background colour past
   its side. Those far level triangles of the opaque list, taken together, stretched across about their middle until
   they reach the wide screen's sides; a far plane, so the stretch shows as no more than a slightly wider sky */
#define BACKDROP_Z 0.0001f      /* 1/w: farther than 10000 */
/* a triangle of the far sky: farther than BACKDROP_Z, its corners at about one depth (to within 3%: a sky plane can
   lean a little, stage 0's by 1% across the screen) */
static int far_level(const RD_TRI* t) {
    float z = t->v[0].z, e = z * 0.03f;
    return z > 0.0f && z < BACKDROP_Z && fabsf(t->v[1].z - z) <= e && fabsf(t->v[2].z - z) <= e;
}
static void wide_backdrop(const RD_FRAME* cf) {
    enum { LAYERS = 8, FAR_MAX = 512 };
    static int logged, last_n;
    static uint16_t far_i[FAR_MAX];
    static uint8_t far_m[FAR_MAX];
    struct { float z, lo, hi, c, k; int n, state; } ly[LAYERS];
    RD_FRAME* f = (RD_FRAME*)cf;    /* the frame is the render thread's now */
    RD_TRI* tris = f->tris[RD_OPAQUE];
    float need_lo = game_x0 - 2.0f, need_hi = game_x0 + game_w + 2.0f;
    int i, j, m, nl = 0, n = 0, any = 0, ntris = f->ntris[RD_OPAQUE];
    if (aspect_mode != ASPECT_WIDE) {
        return;
    }
    /* the far level triangles by their state and depth: each plane its own layer (stage 13's sky and the plain band
       under it, 1% apart, apart); one walk, the few found kept for the stretch */
    for (i = 0; i < ntris; i++) {
        const RD_TRI* t = &tris[i];
        float z = t->v[0].z;
        if (!(z < BACKDROP_Z) || !far_level(t)) {
            continue;
        }
        for (m = 0; m < nl && (fabsf(ly[m].z - z) > ly[m].z * 0.03f || (ly[m].state != t->state &&
                                            !rd_same_state(&f->states[ly[m].state], &f->states[t->state]))); m++) {
        }
        if (m == nl) {
            if (nl == LAYERS) {
                continue;
            }
            ly[nl].z = z, ly[nl].lo = 1e9f, ly[nl].hi = -1e9f, ly[nl].n = 0, ly[nl].k = 1.0f, ly[nl].state = t->state;
            nl++;
        }
        for (j = 0; j < 3; j++) {
            ly[m].lo = t->v[j].x < ly[m].lo ? t->v[j].x : ly[m].lo;
            ly[m].hi = t->v[j].x > ly[m].hi ? t->v[j].x : ly[m].hi;
        }
        ly[m].n++;
        if (n < FAR_MAX) {
            far_i[n] = (uint16_t)i, far_m[n] = (uint8_t)m;
            n++;
        }
    }
    /* only a plane that already spans the 4:3 screen (far things of their own, a moon, some far pieces, are left) */
    for (m = 0; m < nl; m++) {
        float lo = ly[m].lo, hi = ly[m].hi, c = 0.5f * (lo + hi), k, k2;
        if (ly[m].n > 64 || lo > 40.0f || hi < 600.0f || (lo <= need_lo && hi >= need_hi)) {
            continue;
        }
        k = lo > need_lo && c > lo ? (c - need_lo) / (c - lo) : 1.0f;     /* short of the left side */
        k2 = hi < need_hi && hi > c ? (need_hi - c) / (hi - c) : 1.0f;    /* short of the right */
        k = k2 > k ? k2 : k;
        ly[m].k = k > 1.25f ? 1.25f : k;
        ly[m].c = c;
        any = 1;
    }
    if (vita_debug && n != last_n && n >= 2 && logged < 40) {     /* each new set of far layers, taken or not */
        logged++;
        for (m = 0; m < nl; m++) {
            if (ly[m].n >= 2) {
                host_log("vgl: far layer %d: %d triangles at 1/w %.7f, x %.1f..%.1f: stretched %.3f", m, ly[m].n,
                         ly[m].z, ly[m].lo, ly[m].hi, ly[m].k);
            }
        }
    }
    last_n = n;
    for (i = 0; any && i < n; i++) {
        RD_TRI* t = &tris[far_i[i]];
        m = far_m[i];
        if (ly[m].k != 1.0f) {
            for (j = 0; j < 3; j++) {
                t->v[j].x = ly[m].c + (t->v[j].x - ly[m].c) * ly[m].k;
            }
        }
    }
}

static void prepare(const VFRAME* v) {
    const RD_FRAME* f = v->f;
    uint64_t t0 = now_us(), t1;
    int l, i;
    nverts = 0;
    nbatches = 0;
    vring_next();
    memcpy(palette, v->palette, sizeof palette);
    palette_serial = v->palette_serial;
    fog = v->fog;
    fog_on = 0;
    for (i = 0; i < 128; i++) {
        fog_on |= fog.table[i] != 0.0f;
    }
    uint64_t q0 = now_us(), q1, q2, q3, q4;
    if (v->early) {
        cut_wait();             /* the depths and the translucent steps begun at the hand-over */
        cut_us[2] += now_us() - q0;
    } else {
        depth_table(f);
    }
    q1 = now_us();
    /* the background: three corners of a plane, the fourth made from them; its colours and depth not over w */
    {
        const RD_VTX* bg = f->background;
        RD_VTX c[4];
        uint8_t col[4][4];
        int k, ch;
        c[0] = bg[0], c[1] = bg[1], c[2] = bg[2];
        c[3] = bg[1];
        c[3].x = bg[1].x + bg[2].x - bg[0].x;
        c[3].y = bg[1].y + bg[2].y - bg[0].y;
        c[3].z = bg[1].z + bg[2].z - bg[0].z;
        for (k = 0; k < 3; k++) {
            for (ch = 0; ch < 4; ch++) {
                col[k][ch] = (uint8_t)(c[k].base >> (ch == 3 ? 24 : 16 - 8 * ch));
            }
        }
        for (ch = 0; ch < 4; ch++) {
            int x = col[1][ch] + col[2][ch] - col[0][ch];
            col[3][ch] = (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
        }
        for (k = 0; k < 4; k++) {
            const float* bd = &vdepth[vbase[RD_LISTS]];
            float d = (k < 3 ? bd[k] : bd[1] + bd[2] - bd[0]) * depth_scale;
            /* its own 1/w, as rd_soft's zbuf takes it: a floor here hid what lies farther (the intro's sea, the
               sky's far clouds), now that depth is 1/w scaled and can be far below any floor */
            d = d < 0.0f ? 0.0f : d > 0.9999f ? 0.9999f : d;
            background[k].x = c[k].x * (2.0f / RD_WIDTH) - 1.0f;
            background[k].y = 1.0f - c[k].y * (2.0f / RD_HEIGHT);
            background[k].z = depth_mode == 1 ? 2.0f * d - 1.0f : depth_mode == 2 ? 0.5f + 0.5f * d : d;
            background[k].w = 1.0f;
            background[k].u = background[k].v = 0.0f;
            memcpy(background[k].clamp, no_clamp, sizeof no_clamp);
            background[k].fw = 1.0f;
            memcpy(background[k].base, col[k], 4);
            memset(background[k].offs, 0, 4);
        }
    }
    /* the translucent list's first steps on the cutting thread meanwhile */
    int pre_away = !v->early && cut_go_ready() && f->ntris[RD_TRANS] >= 256;
    if (trans_away < 0) {
        trans_away = getenv("MVC2_TRANS_AWAY") == NULL || atoi(getenv("MVC2_TRANS_AWAY")) != 0;
    }
    if (pre_away) {
        cut_run(trans_away ? 4 : 2, f, 0, 0);
    }
    wide_backdrop(f);
    for (l = 0; l < RD_TRANS; l++) {
        for (i = 0; i < f->ntris[l]; i++) {
            const RD_TRI* t = &f->tris[l][i];
            if (i + 4 < f->ntris[l]) {
                __builtin_prefetch(t + 4);
                __builtin_prefetch(&vdepth[vbase[l] + (i + 4) * 3]);
            }
            if (tri_drawn(&f->states[t->state], t)) {
                add_tri(f, l, i);
            }
        }
    }
    q2 = now_us();
    if (v->early) {
        /* done before it began */
    } else if (pre_away) {
        cut_wait();
        if (trans_away) {
            cut_us[2] += now_us() - q2;     /* all of it done there: what the render thread waited */
        }
    } else {
        trans_pre(f);
    }
    if (!v->early && (!pre_away || !trans_away)) {
        trans_post(f);
    }
    q3 = now_us();
    if (pkey_room && cut_go >= 0 && npieces >= 512) {
        /* the second half on the cutting thread, into its own arrays, then appended: the batches as one thread
           would have made them (the two at the join merged when their states are the same) */
        int h = npieces / 2, base_v, base_b, k;
        cut_job.kind = 1, cut_job.f = f, cut_job.from = h, cut_job.to = npieces;
        sceKernelSignalSema(cut_go, 1);
        add_pieces(&vb_main, f, 0, h);
        sceKernelWaitSema(cut_done, 1, NULL);
        base_v = nverts, base_b = nbatches;
        if (work_nv > 0 && nverts + work_nv <= vb_main.vmax && nbatches + work_nb <= MAX_BATCHES) {
            memcpy(verts + nverts, work_v, sizeof(VTX) * (size_t)work_nv);
            nverts += work_nv;
            k = 0;
            if (base_b > 0 && batches[base_b - 1].list == work_b[0].list &&
                rd_same_state(&batches[base_b - 1].s, &work_b[0].s)) {
                batches[base_b - 1].count += work_b[0].count;     /* its vertices follow on */
                batches[base_b - 1].clamp |= work_b[0].clamp, batches[base_b - 1].offs |= work_b[0].offs;
                k = 1;
            }
            for (; k < work_nb; k++) {
                batches[nbatches] = work_b[k];
                batches[nbatches].first += base_v;
                nbatches++;
            }
        }
    } else if (pkey_room) {
        add_pieces(&vb_main, f, 0, npieces);
    }
    tex_resolve(0);
    q4 = now_us();
    {
        uint64_t part[4] = { q1 - q0, q2 - q1, q3 - q2, q4 - q3 };
        int k;
        for (k = 0; k < 4; k++) {
            perf.part_us[k] += part[k];
            perf.part_worst[k] = part[k] > perf.part_worst[k] ? part[k] : perf.part_worst[k];
        }
    }
    t1 = now_us();
    perf.prep_us += t1 - t0;
    /* the render thread's period, prepare's end to prepare's end: with the translucent steps begun at the hand-over
       (job 5) prepare itself is short while the frames still come late (Abyss's triple finish: 5 ms of it, 46 fps) */
    {
        static uint64_t last_end;
        uint64_t period = last_end != 0 ? t1 - last_end : 0;
        last_end = t1;
        cut_heavy_run = t1 - t0 > CUT_HEAVY_US || t1 - t0 + last_draw_us > CUT_BUSY_US || period > CUT_PERIOD_US
                        ? cut_heavy_run + 1 : 0;
        if (cut_capped && period > CUT_PERIOD_KEEP_US) {
            cut_hold = CUT_HOLD;
        }
    }
    if (cut_heavy_run >= 2 || (cut_capped && t1 - t0 + last_draw_us > CUT_KEEP_US)) {
        cut_hold = CUT_HOLD;
    } else if (cut_hold > 0) {
        cut_hold--;
    }
    cut_capped = 0;
    perf.tris += (unsigned)(nverts / 3);
    perf.batches += (unsigned)nbatches;
}

/* ---- programs ---- */

static const char* const vs_src =
    "void main(float4 aPos, float2 aUV, float4 aBase, float4 aOffs, float4 aClamp, float aW,"
    " out float4 oPos : POSITION, out float2 vUV : TEXCOORD0, out float4 vBase : TEXCOORD1,"
    " out float4 vOffs : TEXCOORD2, out float vW : TEXCOORD3, out float4 vClamp : TEXCOORD4) {"
    " oPos = aPos; vUV = aUV; vBase = aBase; vOffs = aOffs; vW = aW; vClamp = aClamp; }";

/* the colours' math in C4/C3/C1: half when MVC2_HALF=1 (the USSE does two halves where it does one float; the
   colours are 8 bits), else float; the texture coordinates and the fog's 1/w stay float */
static const char* const fs_head =
    "uniform float4 uShade; uniform float4 uFog; uniform float4 uFogColor; uniform float4 uMisc;"
    "uniform sampler2D uTex; uniform sampler2D uPal; uniform sampler2D uFogT;"
    "\n#if SH_MODE >= 0\n"
    /* the TSP's mode, the program's own: decal, modulate, decal alpha, modulate alpha; the texel's alpha or 1 */
    "C4 shade(C4 t, C4 b) {\n"
    "#if !SH_TA\n"
    " t.a = 1.0;\n"
    "#endif\n"
    "#if SH_MODE == 1\n"
    " t.rgb *= b.rgb;\n"
    "#elif SH_MODE == 2\n"
    " t = C4(lerp(b.rgb, t.rgb, t.a), b.a);\n"
    "#elif SH_MODE == 3\n"
    " t *= b;\n"
    "#endif\n"
    " return t; }\n"
    "#elif FSX_SHADE\n"
    "C4 shade(C4 t, C4 b) { return t * b; }\n"
    "#elif FSX_BRANCH\n"
    /* the TSP's four modes by branches on the uniform (the same for all the batch's pixels), not lerps */
    "C4 shade(C4 t, C4 b) {"
    " if (uShade.w < 0.5) t.a = 1.0;"
    " if (uShade.z > 0.5) return C4(lerp(b.rgb, t.rgb, t.a), b.a);"
    " if (uShade.x > 0.5) t.rgb *= b.rgb;"
    " if (uShade.y > 0.5) t.a *= b.a;"
    " return t; }\n"
    "#else\n"
    "C4 shade(C4 t, C4 b) {"
    " C4 sh = (C4)uShade;"
    " t.a = lerp(1.0, t.a, sh.w);"
    " C3 m = t.rgb * lerp(C3(1.0, 1.0, 1.0), b.rgb, sh.x);"
    " C3 c = lerp(m, lerp(b.rgb, t.rgb, t.a), sh.z);"
    " C1 a = lerp(t.a * lerp(1.0, b.a, sh.y), b.a, sh.z);"
    " return C4(c, a); }\n"
    "#endif\n"
    "C4 finish(C4 c, C4 o, float w) {"
    "\n#if !FSX_NOOFFS\n"
    " c.rgb = min(c.rgb + o.rgb, 1.0);\n"
    "#endif\n"
    "#if FOG\n"
    "  float q = clamp(uFog.z / w, 1.0, 255.9999);"
    "  float e = floor(log2(q));"
    "  float p = 16.0 * e + 16.0 * (q * exp2(-e) - 1.0);"
    "  C1 f = (C1)tex2D(uFogT, float2((p + 0.5) * 0.0078125, 0.5)).r;"
    "  C1 t2 = (C1)uFog.y;"
    "  c.rgb = lerp(c.rgb, (C3)uFogColor.rgb, lerp(f, 1.0, t2));"
    "  c.a = lerp(c.a, f, t2);\n"
    "#endif\n"
    "#if PUNCH\n"
    " if (c.a < 0.4999) discard;\n"
    "#endif\n"
    " return c; }"
    "\n#if SNAP\n"
    /* a sprite sampled as the console samples it, once a game pixel: the texture coordinate moved from this screen
       pixel's centre to the centre of the game pixel it lies in (by the coordinate's slopes on the screen) */
    "float2 snapuv(float2 uv, float4 wp) {"
    " float2 gp = (wp.xy - SNAP_ORIGIN) * SNAP_SCALE;"
    " float2 d = (floor(gp) + 0.5 - gp) / SNAP_SCALE;"
    " return uv + ddx(uv) * d.x + ddy(uv) * d.y; }\n"
    "#define SUV(uv) snapuv(uv, wp)\n"
    "#else\n"
    "#define SUV(uv) (uv)\n"
    "#endif\n"
    "C4 pal(float2 at) {"
    " float i = floor(tex2D(uTex, at).r * 255.0 + 0.5);"
    " return (C4)tex2D(uPal, float2((i + uMisc.y + 0.5) * 0.0009765625, 0.5)); }";

/* MVC2_FSX (measuring only, the picture not right): 1 no UV clamp, 2 no offset colour, 4 shade as t * b */
#define FS_ARGS "float2 vUV : TEXCOORD0, C4 vBase : TEXCOORD1, C4 vOffs : TEXCOORD2, float vW : TEXCOORD3, float4 vClamp : TEXCOORD4" \
    "\n#if SNAP\n, float4 wp : WPOS\n#endif\n"

static const char* const fs_main[4] = {
    /* no texture */
    "float4 main(" FS_ARGS ") : COLOR { return finish(vBase, vOffs, vW); }",
    /* a texture */
    "float4 main(" FS_ARGS ") : COLOR { return finish(shade((C4)tex2D(uTex, CLMP(SUV(vUV))), vBase), vOffs, vW); }",
    /* a palette's, the nearest texel */
    "float4 main(" FS_ARGS ") : COLOR { return finish(shade(pal(CLMP(SUV(vUV))), vBase), vOffs, vW); }",
    /* a palette's, filtered: the four texels around it looked up, then blended */
    "float4 main(" FS_ARGS ") : COLOR {"
    " float2 s = SUV(vUV) * uMisc.zw - 0.5; float2 i = floor(s); float2 fr = s - i;"
    " float2 inv = 1.0 / uMisc.zw; float2 a = (i + 0.5) * inv;"
    " float2 b = CLMP(a + inv); a = CLMP(a);"
    " C4 c00 = pal(a), c10 = pal(float2(b.x, a.y)), c01 = pal(float2(a.x, b.y)), c11 = pal(b);"
    " C4 t = lerp(lerp(c00, c10, (C1)fr.x), lerp(c01, c11, (C1)fr.x), (C1)fr.y);"
    " return finish(shade(t, vBase), vOffs, vW); }",
};

/* a program: its kind, + P_PUNCH when it discards (punch-through: only those, as a discard keeps the PowerVR from
   leaving hidden pixels unshaded), + P_FOG when it fogs (table fog, or table 2 with uFog.y 1) */
/* + P_SHADE times the TSP's shading mode and its texture alpha (3 bits) for a textured kind: made when first used */
enum { P_UNTEX, P_TEX, P_PAL, P_PAL_FILTER, P_KINDS, P_PUNCH = 4, P_FOG = 8, P_CLAMP = 16, P_OFFS = 32, P_SHADE = 64,
       P_GENERIC = 512, P_SNAP = 1024, PROGRAMS = 2048 };

static struct {
    GLuint prog;
    GLint shade, fog, fog_color, misc;
    float last[4][4];
} progs[PROGRAMS];

/*
 * The programs. A batch's own program (its kind, punch, fog, UV bounds, offset colour, TSP shading mode and texture
 * alpha) is made from compiled shaders kept in DATA_DIR "shaders/pack.bin" (records of a source hash, a length and
 * the binary vitaGL takes back with glShaderBinary), read whole at the start. One not compiled yet is compiled by a
 * thread of its own (core 2, low priority: some 0.2 to 9 s each on the console) while the batch is drawn by the
 * generic program of its kind (P_GENERIC: the shading by uniforms, bounds and offset done: the same picture, more
 * GPU time); after that thread has compiled those the frames ask for, it compiles every other one, once ever, so
 * a later run has them all. Nothing is compiled on the render thread after the start (that was a 2 to 8 s hold-up
 * in a fight). MVC2_SHCACHE=0: no pack (everything compiled on the render thread, as it comes).
 */
static int sh_cache = -1;
static unsigned sh_loaded, sh_compiled;

#define PACK_MOST 4096       /* (1024 filled up: the shaders compiled after were lost, compiled again each run) */
static struct {
    uint64_t hash;
    const void* bin;
    uint32_t len;
} pack[PACK_MOST];
static volatile int npack;
static uint8_t* pack_mem;

static uint64_t src_hash(GLenum kind, int n, const char* const* src) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)kind;
    int i;
    for (i = 0; i < n; i++) {
        const unsigned char* c = (const unsigned char*)src[i];
        for (; *c; c++) {
            h = (h ^ *c) * 1099511628211ull;     /* FNV-1a */
        }
    }
    return h;
}

static const void* pack_find(uint64_t h, uint32_t* len) {
    int i, n = npack;
    __sync_synchronize();
    for (i = 0; i < n; i++) {
        if (pack[i].hash == h) {
            *len = pack[i].len;
            return pack[i].bin;
        }
    }
    return NULL;
}

/* a record appended to the file and the table (the render thread at the start, the compiling thread after) */
static SceUID pack_lock = -1;   /* the render thread (the start's compiles) and the compiling thread both add */
static void pack_add(uint64_t h, const void* bin, uint32_t len) {
    SceUID fd;
    uint8_t* rec;
    if (pack_lock >= 0) {
        sceKernelLockMutex(pack_lock, 1, NULL);
    }
    if (npack == PACK_MOST || (rec = malloc(12 + len)) == NULL) {
        if (pack_lock >= 0) {
            sceKernelUnlockMutex(pack_lock, 1);
        }
        return;
    }
    pack[npack].hash = h, pack[npack].bin = bin, pack[npack].len = len;
    __sync_synchronize();
    npack++;
    /* the record in one write: two writes from two threads interleaved, and a record cut short once hid every
       later one from the next start's read (the file grew 2.7 MB of shaders compiled again each run) */
    {
        uint32_t head[3] = { (uint32_t)h, (uint32_t)(h >> 32), len };
        memcpy(rec, head, 12);
        memcpy(rec + 12, bin, len);
    }
    fd = sceIoOpen(DATA_DIR "shaders/pack.bin", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, rec, 12 + len);
        sceIoClose(fd);
    }
    free(rec);
    if (pack_lock >= 0) {
        sceKernelUnlockMutex(pack_lock, 1);
    }
}

/* the records of one pack file into the table (a hash already there kept as it was) */
static void pack_read(const char* path, int mend) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;
    uint8_t* mem = NULL;
    uint32_t at = 0, len, keep = 0;
    int added = 0, cut = 0, bad = 0;
    if (fd < 0) {
        return;
    }
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size > 0 && size < (16 << 20) && (mem = malloc((size_t)size)) != NULL) {
        sceIoLseek(fd, 0, SCE_SEEK_SET);
        if (sceIoRead(fd, mem, (SceSize)size) != (int)size) {
            size = 0;
        }
        while (at + 12 <= (uint32_t)size && npack < PACK_MOST) {
            uint32_t head[3], gxp_size;
            uint64_t h;
            const uint8_t* bin;
            memcpy(head, mem + at, 12);
            if (head[2] < 20 || head[2] > (1u << 20) || at + 12 + head[2] > (uint32_t)size) {
                cut = 1;        /* a record cut short (the app closed as it was written) */
                break;
            }
            bin = mem + at + 12;
            memcpy(&gxp_size, bin + 12, 4);
            /* vitaGL's binary: 4 zero bytes, then a GXP program whose own size (at its byte 8) is the record's, give
               or take the 3 bytes of padding: a record mixed by two writers (an older build's) had another's header
               in it, and vitaGL took 1.4 GB for its size and crashed */
            if (memcmp(bin + 4, "GXP", 4) != 0 || gxp_size > head[2] - 4 || head[2] - 4 - gxp_size > 3) {
                bad++;
                at += 12 + head[2];
                continue;
            }
            if (keep != at) {
                memmove(mem + keep, mem + at, 12 + head[2]);    /* the good records together, for the mend */
            }
            h = (uint64_t)head[1] << 32 | head[0];
            if (pack_find(h, &len) == NULL) {
                pack[npack].hash = h;
                pack[npack].bin = mem + keep + 12;
                pack[npack].len = head[2];
                npack++;
                added++;
            }
            keep += 12 + head[2];
            at += 12 + head[2];
        }
        pack_mem = mem;
    }
    sceIoClose(fd);
    if (mend && mem != NULL && added == 0 && keep > 0 && !cut && !bad) {
        /* every record already in the VPK's pack: the copy only cost the card a read at each start */
        sceIoRemove(path);
        host_log("vgl: %s holds only the VPK's shaders: removed", path);
    }
    if ((cut || bad) && mend && mem != NULL) {
        /* the good records written again on their own, else every record added after a cut stays unread */
        SceUID w = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (w >= 0) {
            sceIoWrite(w, mem, keep);
            sceIoClose(w);
        }
    }
    if (cut || bad) {
        host_log("vgl: %s: %d broken records dropped%s (%u of %u bytes good)", path, bad, cut ? ", its tail cut short" : "",
                 keep, (unsigned)size);
    }
    host_log("vgl: %d shaders from %s", added, path);
}

/*
 * The VPK's own pack (app0:shaders/pack.bin: the shaders compiled on a console before the build, every Vita's GPU
 * being the same), then the memory card's (those compiled since: a later build takes them into its VPK, see
 * vita-tools/pull_shaders.sh)
 */
static void pack_load(void) {
    sceIoMkdir(DATA_DIR "shaders", 0777);
    pack_lock = sceKernelCreateMutex("vgl_pack", 0, 0, NULL);
    pack_read("app0:shaders/pack.bin", 0);
    pack_read(DATA_DIR "shaders/pack.bin", 1);
}

/* a shader: from the pack, else compiled here when allowed (the start's), else 0 */
static GLuint compile(GLenum kind, int n, const char* const* src, int allow) {
    GLuint sh;
    GLint len[4];
    uint64_t h = src_hash(kind, n, src);
    uint32_t blen;
    const void* bin;
    int i;
    if (sh_cache < 0) {
        sh_cache = getenv("MVC2_SHCACHE") == NULL || atoi(getenv("MVC2_SHCACHE")) != 0;
        if (sh_cache) {
            pack_load();
        }
    }
    if (sh_cache && (bin = pack_find(h, &blen)) != NULL) {
        sh = glCreateShader(kind);
        glShaderBinary(1, &sh, 0, bin, (GLsizei)blen);
        sh_loaded++;
        return sh;
    }
    if (!allow && sh_cache) {
        return 0;
    }
    sh = glCreateShader(kind);
    for (i = 0; i < n; i++) {
        len[i] = (GLint)strlen(src[i]);
    }
    glShaderSource(sh, n, src, len);
    glCompileShader(sh);
    sh_compiled++;
    if (sh_cache) {
        static uint8_t tmp[256 * 1024];
        GLsizei got = 0;
        vglGetShaderBinary(sh, sizeof tmp, &got, tmp);
        if (got > 0 && got <= (GLsizei)sizeof tmp) {
            void* keep = malloc((size_t)got);
            if (keep != NULL) {
                memcpy(keep, tmp, (size_t)got);
                pack_add(h, keep, (uint32_t)got);
            }
        }
    }
    return sh;
}

static int half_on;     /* MVC2_HALF */
static int snap_on = -1;   /* MVC2_SNAP: 1 (the default) sprites by the nearest texel sampled once a game pixel, as the
                              console does; 2 the filtered ones too; 0 neither */
static int fsx;         /* MVC2_FSX */
static float snap_ox;   /* MVC2_SNAP_OX: added to the snap's origin (a test of the screen position's convention) */

static GLuint vs_shader;
static unsigned progs_made;
static uint64_t progs_us;

/* program key i's fragment source: defs + fs_head + its kind's main */
static void program_source(int i, char* defs, size_t room, const char** src) {
    int textured = i % P_KINDS != P_UNTEX, sv = (i % P_GENERIC) / P_SHADE, generic = (i & P_GENERIC) != 0;
    int snap = (i & P_SNAP) != 0;
    snprintf(defs, room, "#define PUNCH %d\n#define FOG %d\n#define C4 %s4\n#define C3 %s3\n#define C1 %s\n"
             "#define CLMP(x) %s\n#define FSX_NOOFFS %d\n#define FSX_SHADE %d\n#define FSX_BRANCH %d\n"
             "#define SH_MODE %d\n#define SH_TA %d\n#define SNAP %d\n",
             (i & P_PUNCH) != 0, (i & P_FOG) != 0, half_on ? "half" : "float", half_on ? "half" : "float",
             half_on ? "half" : "float",
             ((i & P_CLAMP) || generic) && !(fsx & 1) ? "min(max((x), vClamp.xy), vClamp.zw)" : "(x)",
             (!(i & P_OFFS) && !generic) || (fsx & 2), (fsx & 4) != 0, (fsx & 8) != 0,
             textured && !generic && !(fsx & 12) ? (sv & 3) : -1, (sv & 4) != 0, snap);
    if (snap) {
        /* the snap's geometry in its own programs only: the others' source (and pack hash) is the viewport's own */
        size_t at = strlen(defs);
        snprintf(defs + at, room - at, "#define SNAP_ORIGIN float2(%.6f, 0.0)\n#define SNAP_SCALE float2(%.8f, %.8f)\n",
                 (double)(view_x0 - game_x0 * view_w / game_w) + snap_ox, (double)game_w / view_w,
                 (double)RD_HEIGHT / SCREEN_H);
    }
    src[0] = defs, src[1] = fs_head, src[2] = fs_main[i % P_KINDS];
}

/* program key i, its fragment shader from the pack (or compiled, when allow); 0 when not compiled yet */
static int make_program(int i, int allow) {
    char defs[400];
    const char* src[3];
    uint64_t t0 = now_us();
    GLuint fs, p;
    GLint ok = 0;
    program_source(i, defs, sizeof defs, src);
    fs = compile(GL_CG_FRAGMENT_SHADER_EXT, 3, src, allow);
    if (fs == 0) {
        return 0;
    }
    p = glCreateProgram();
    glAttachShader(p, vs_shader);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aUV");
    glBindAttribLocation(p, 2, "aBase");
    glBindAttribLocation(p, 3, "aOffs");
    glBindAttribLocation(p, 4, "aClamp");
    glBindAttribLocation(p, 5, "aW");
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        host_log("vgl: program %d did not link: %s", i, log);
        return 0;
    }
    glUseProgram(p);
    cur.prog = -1;
    /* vitaGL starts every sampler at unit 0: only glUniform1i moves one */
    glUniform1i(glGetUniformLocation(p, "uTex"), 0);
    glUniform1i(glGetUniformLocation(p, "uPal"), 1);
    glUniform1i(glGetUniformLocation(p, "uFogT"), 2);
    progs[i].prog = p;
    progs[i].shade = glGetUniformLocation(p, "uShade");
    progs[i].fog = glGetUniformLocation(p, "uFog");
    progs[i].fog_color = glGetUniformLocation(p, "uFogColor");
    progs[i].misc = glGetUniformLocation(p, "uMisc");
    memset(progs[i].last, 0xFF, sizeof progs[i].last);    /* NaNs: set at the first draw */
    progs_made++;
    progs_us += now_us() - t0;
    return 1;
}

/* ---- the compiling thread ---- */

static SceUID shc_sema = -1;
static volatile int shc_want[PROGRAMS];     /* 1 asked for by a draw, 2 queued as the frames asked */
static int shc_queue[PROGRAMS];
static volatile unsigned shc_head, shc_tail;   /* the render thread puts, the compiling thread takes */
static volatile unsigned shc_done;
static uint64_t shc_us;

/* a program worth having: textured kinds by every mode, the untextured by punch, fog and offset */
static int program_useful(int i) {
    if (i & P_SNAP) {
        int k = i % P_KINDS;
        return !(i & P_GENERIC) && (i & P_CLAMP) && k != P_UNTEX;
    }
    if (i & P_GENERIC) {
        return 0;
    }
    if (i % P_KINDS == P_UNTEX) {
        return !(i & P_CLAMP) && i < P_SHADE;
    }
    return 1;
}

static int shc_main(SceSize args, void* argp) {
    unsigned next_all = 0;
    int ready = 0;
    (void)args, (void)argp;
    for (;;) {
        int key = -1;
        if (shc_tail != shc_head) {
            key = shc_queue[shc_tail % PROGRAMS];
            __sync_synchronize();
            shc_tail++;
        } else if (next_all < PROGRAMS && render_no < 600) {
            /* not while the game starts: the compiler held its start up (its first disc read waited out the
               compiles, a minute) */
            SceUInt32 t = 500000;
            sceKernelWaitSema(shc_sema, 1, &t);
            continue;
        } else if (next_all < PROGRAMS) {
            /* then all the others, once ever: the next runs find them in the pack */
            for (; next_all < PROGRAMS && key < 0; next_all++) {
                if (program_useful((int)next_all) && progs[next_all].prog == 0 && shc_want[next_all] == 0) {
                    key = (int)next_all;
                }
            }
        }
        if (key < 0) {
            static int told;
            if (!told && next_all >= PROGRAMS) {
                told = 1;
                host_log("vgl: every program compiled: %u in %.1f s, %d shaders in the pack", shc_done, shc_us / 1e6, npack);
            }
            sceKernelWaitSema(shc_sema, 1, NULL);
            continue;
        }
        if (!ready) {
            /* the compiler, here: vitaGL starts it only at its own first compile, and none happened when the start
               found every shader in the pack */
            if (sh_compiled == 0) {
                shark_set_allocators(malloc, free);
                if (shark_init(NULL) < 0 && shark_init("ur0:data/external/libshacccg.suprx") < 0) {
                    host_log("vgl: no shader compiler (libshacccg.suprx): the generic programs only");
                    return 0;
                }
            }
            ready = 1;
        }
        {
            char defs[400];
            const char* src[3];
            uint64_t h, t0 = now_us();
            uint32_t len;
            program_source(key, defs, sizeof defs, src);
            h = src_hash(GL_CG_FRAGMENT_SHADER_EXT, 3, src);
            if (pack_find(h, &len) == NULL) {
                static char whole[16 * 1024];
                SceGxmProgram* prog;
                uint32_t size = 0;
                snprintf(whole, sizeof whole, "%s%s%s", src[0], src[1], src[2]);
                size = (uint32_t)strlen(whole);     /* in: the source's length (vitashark's), out: the program's */
                prog = shark_compile_shader_extended(whole, &size, SHARK_FRAGMENT_SHADER, SHARK_OPT_FAST, 1, 0, 1);
                if (prog != NULL && size > 0) {
                    uint8_t* bin = malloc(size + 4);
                    if (bin != NULL) {
                        memset(bin, 0, 4);       /* vitaGL's binary: no matrix uniforms, then the program */
                        memcpy(bin + 4, prog, size);
                        pack_add(h, bin, size + 4);
                    }
                } else {
                    static int told;
                    const SceShaccCgCompileOutput* out = shark_get_internal_compile_output();
                    if (told++ < 3) {
                        host_log("vgl: program %d did not compile (%d messages%s%s)", key,
                                 out != NULL ? out->diagnosticCount : -1,
                                 out != NULL && out->diagnosticCount > 0 ? ": " : "",
                                 out != NULL && out->diagnosticCount > 0 ? out->diagnostics[0].message : "");
                    }
                }
                shark_clear_output();
                shc_us += now_us() - t0;
                shc_done++;
                if (shc_tail == shc_head) {
                    sceKernelDelayThread(20000);    /* the others in turn: a breath between prewarm compiles */
                }
            }
        }
    }
    return 0;
}

static void shc_start(void) {
    SceUID th;
    if (!sh_cache) {
        return;
    }
    if (sh_compiled != 0) {
        shark_set_allocators(malloc, free); /* the app's heap (locked), not vitaGL's pools, from this thread */
    }
    shc_sema = sceKernelCreateSema("vgl_shc", 0, 0, 1, NULL);
    th = sceKernelCreateThread("vgl_shc", shc_main, 189, 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        host_log("vgl: no compiling thread (%08X)", th);
        shc_sema = -1;
        return;
    }
    rd_vgl_watch_thread("SHC", th);
}

/* the program to draw a batch with: its own when made or in the pack, else the generic one of its kind meanwhile */
static int program_for(int key) {
    static unsigned tried[PROGRAMS];    /* shc_done + 1 when the pack was last looked in for it */
    if (progs[key].prog != 0) {
        return key;
    }
    if (tried[key] != shc_done + 1) {   /* looked for again only once the compiling thread has made another */
        tried[key] = shc_done + 1;
        if (make_program(key, shc_sema < 0)) {
            return key;
        }
    }
    if (shc_want[key] == 0 && shc_sema >= 0) {
        shc_want[key] = 1;
        shc_queue[shc_head % PROGRAMS] = key;
        __sync_synchronize();
        shc_head++;
        sceKernelSignalSema(shc_sema, 1);
        if (render_no > 1) {
            host_log("vgl: program %d being compiled, its batches drawn by the generic one meanwhile", key);
        }
    }
    {
        int g = P_GENERIC + key % (P_FOG * 2);     /* kind, punch, fog */
        if (progs[g].prog == 0) {
            make_program(g, 1);
        }
        return progs[g].prog != 0 ? g : key;
    }
}

static int make_programs(void) {
    const char* vsrc[1] = { vs_src };
    uint64_t t0 = now_us();
    int i;
    vs_shader = compile(GL_CG_VERTEX_SHADER_EXT, 1, vsrc, 1);
    for (i = 0; i < P_FOG * 2; i++) {
        if (!make_program(P_GENERIC + i, 1)) {      /* the generic ones: compiled at the very first start only */
            return 0;
        }
    }
    for (i = 0; i < PROGRAMS; i++) {
        if (program_useful(i)) {
            make_program(i, 0);                     /* those in the pack */
        }
    }
    host_log("vgl: %u programs made in %.0f ms (%u shaders from the pack, %u compiled)", progs_made,
             (now_us() - t0) / 1000.0, sh_loaded, sh_compiled);
    if (getenv("MVC2_SHCTEST") != NULL) {
        /* the compiling thread's call, made here: does the compiler work from this thread at all */
        char defs[400], whole[16 * 1024];
        const char* src[3];
        uint32_t size = 0;
        SceGxmProgram* prog;
        if (sh_compiled == 0) {
            shark_set_allocators(malloc, free);
            host_log("vgl: shark_init %d", shark_init(NULL));
        }
        program_source(P_TEX + P_SHADE, defs, sizeof defs, src);
        snprintf(whole, sizeof whole, "%s%s%s", src[0], src[1], src[2]);
        size = (uint32_t)strlen(whole);
        prog = shark_compile_shader_extended(whole, &size, SHARK_FRAGMENT_SHADER, SHARK_OPT_FAST, 1, 0, 1);
        {
            const SceShaccCgCompileOutput* out = shark_get_internal_compile_output();
            host_log("vgl: compiled here: %p, %u bytes, %d messages %s", (void*)prog, size,
                     out ? out->diagnosticCount : -1, out && out->diagnosticCount > 0 ? out->diagnostics[0].message : "");
        }
        shark_clear_output();
    }
    shc_start();
    return 1;
}

static GLuint pal_tex, fog_tex;

static void set_uniform(int p, int which, GLint loc, const float* v) {
    if (loc >= 0 && memcmp(progs[p].last[which], v, 16) != 0) {
        memcpy(progs[p].last[which], v, 16);
        glUniform4fv(loc, 1, v);
    }
}

static const GLenum src_factor[8] = { GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                                      GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA };
static const GLenum dst_factor[8] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                                      GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA };
static const GLenum depth_funcs[8] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL,
                                       GL_ALWAYS };

static void use_program(int p) {
    if (cur.prog != p) {
        cur.prog = p;
        glUseProgram(progs[p].prog);
    }
}

static void set_blend(int on, int src, int dst) {
    if (cur.blend != on) {
        cur.blend = on;
        if (on) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }
    }
    if (on && (cur.src != src || cur.dst != dst)) {
        cur.src = src, cur.dst = dst;
        glBlendFunc(src_factor[src & 7], dst_factor[dst & 7]);
    }
}

static void set_depth(int func, int mask) {
    if (cur.depth_func != func) {
        cur.depth_func = func;
        glDepthFunc(depth_funcs[func & 7]);
    }
    if (cur.depth_mask != mask) {
        cur.depth_mask = mask;
        glDepthMask(mask ? GL_TRUE : GL_FALSE);
    }
}

static void bind_tex(TEX* t, const RD_STATE* s) {
    GLint ws = s->uv_mode & 2 ? GL_CLAMP_TO_EDGE : s->uv_mode & 8 ? GL_MIRRORED_REPEAT : GL_REPEAT;
    GLint wt = s->uv_mode & 1 ? GL_CLAMP_TO_EDGE : s->uv_mode & 4 ? GL_MIRRORED_REPEAT : GL_REPEAT;
    GLint filter = (is_pal(t->format) && !t->hwpal) || !s->bilinear ? GL_NEAREST : GL_LINEAR;
    if (cur.tex != t->id) {
        cur.tex = t->id;
        glBindTexture(GL_TEXTURE_2D, t->id);
        perf.binds++;
    }
    if (t->hwpal && pal_ring != NULL) {
        /* the descriptor is copied into the commands at each draw: the bank set now holds for this one */
        SceGxmTexture* g = vglGetGxmTexture(GL_TEXTURE_2D);
        if (g != NULL) {
            sceGxmTextureSetPalette(g, pal_ring + pal_slot * 1024 + (s->palette & 1023));
        }
    }
    if (t->filter != filter) {
        GLint min = filter;
        if (t->levels > 0) {
            min = filter == GL_LINEAR ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
        }
        t->filter = filter;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    }
    if (t->wrap_s != ws) {
        t->wrap_s = ws;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
    }
    if (t->wrap_t != wt) {
        t->wrap_t = wt;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
    }
}

/*
 * The frame's vertices written straight into GPU-mapped memory (uncached, mapped with sceGxmMapMemory), a ring of
 * VRING frames so the GPU still reading one is not written, and drawn from a buffer object over it (vglBufferData):
 * from arrays in the app's memory vitaGL copied each draw's vertices, per attribute, whole strides each (some
 * 4 MB a frame on the title). MVC2_VBO=0: as before.
 */
#define VRING 3
#define RING_VERTS (3 * 32768)
static VTX* vring[VRING];
static int vring_slot, vbo_on = -1;
static GLuint vbo;
static VTX* verts_heap;         /* the arrays in the app's memory: MVC2_VBO=0, or a frame past RING_VERTS */

static void vring_start(void) {
    size_t size = ((size_t)VRING * RING_VERTS * sizeof(VTX) + 0xFFFFF) & ~(size_t)0xFFFFF;
    SceUID blk;
    void* base = NULL;
    int k;
    vbo_on = getenv("MVC2_VBO") == NULL || atoi(getenv("MVC2_VBO")) != 0;
    if (!vbo_on) {
        return;
    }
    blk = sceKernelAllocMemBlock("vgl_verts", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
    if (blk < 0 || sceKernelGetMemBlockBase(blk, &base) < 0 || sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_READ) < 0) {
        host_log("vgl: no GPU-mapped memory for the vertices (%08X): copied each draw", blk);
        vbo_on = 0;
        return;
    }
    for (k = 0; k < VRING; k++) {
        vring[k] = (VTX*)base + (size_t)k * RING_VERTS;
    }
    glGenBuffers(1, &vbo);
    host_log("vgl: the vertices in %u KB of GPU-mapped memory, %d frames of %d", (unsigned)(size >> 10), VRING, RING_VERTS);
}

/* the next frame's vertex arrays */
static void vring_next(void) {
    if (vbo_on > 0) {
        vring_slot = (vring_slot + 1) % VRING;
        verts = vring[vring_slot];
        vb_main.v = verts, vb_main.vmax = RING_VERTS;
    }
}

static void attribs(const VTX* base) {
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(VTX), &base->x);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VTX), &base->u);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VTX), base->base);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VTX), base->offs);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(VTX), base->clamp);
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(VTX), &base->fw);
}

static void draw_batch(const BATCH* b) {
    const RD_STATE* s = &b->s;
    int p = P_UNTEX;
    float shade[4] = { 0, 0, 0, 1 }, fg[4] = { 0, 0, fog.density, 0 }, fc[4], misc[4] = { 0, 0, 1, 1 };
    if (b->tex != NULL) {
        p = !is_pal(b->tex->format) || b->tex->hwpal ? P_TEX : s->bilinear ? P_PAL_FILTER : P_PAL;
        shade[0] = s->shading == RD_MODULATE || s->shading == RD_MODULATE_ALPHA;
        shade[1] = s->shading == RD_MODULATE_ALPHA;
        shade[2] = s->shading == RD_DECAL_ALPHA;
        shade[3] = s->tex_alpha;
        misc[1] = (float)s->palette;
        misc[2] = (float)b->tex->w;
        misc[3] = (float)b->tex->h;
    }
    fg[0] = s->fog == RD_FOG_TABLE && fog_on && !rd_q.nofog;
    fg[1] = s->fog == RD_FOG_TABLE2;
    if (fg[0] != 0.0f || fg[1] != 0.0f) {
        p += P_FOG;
    }
    if (b->list == RD_PUNCH) {
        p += P_PUNCH;
    }
    if (b->clamp && b->tex != NULL) {
        p += P_CLAMP;
    }
    if (b->offs) {
        p += P_OFFS;
    }
    if (b->tex != NULL && !(fsx & 12)) {
        p += P_SHADE * ((s->shading & 3) | (s->tex_alpha ? 4 : 0));
    }
    if (snap_on && b->clamp && b->tex != NULL && (!s->bilinear || snap_on == 2)) {
        p += P_SNAP;    /* a sprite sampled once a game pixel, as the console does (MVC2_SNAP=2: filtered ones too) */
    }
    p = program_for(p);
    fc[0] = (fog.color >> 16 & 255) / 255.0f, fc[1] = (fog.color >> 8 & 255) / 255.0f;
    fc[2] = (fog.color & 255) / 255.0f, fc[3] = (fog.color >> 24) / 255.0f;
    misc[0] = b->list == RD_PUNCH;
    use_program(p);
    set_uniform(p, 0, progs[p].shade, shade);
    set_uniform(p, 1, progs[p].fog, fg);
    set_uniform(p, 2, progs[p].fog_color, fc);
    set_uniform(p, 3, progs[p].misc, misc);
    if (b->tex != NULL) {
        bind_tex(b->tex, s);
    }
    if (b->list == RD_OPAQUE) {
        set_blend(0, 0, 0);
        set_depth(s->depth, s->zwrite);
    } else {
        set_blend(1, s->src, s->dst);
        set_depth(RD_GEQUAL, b->list == RD_PUNCH);
    }
    glDrawArrays(GL_TRIANGLES, b->first, b->count);
}

/* ---- debugging: a picture, the batches ---- */

/* a picture: read on the render thread, written by a thread of its own at a low priority, in one write (each of
   many small writes to ux0: costs a great deal) */
static uint8_t* shot_px;
static volatile int shot_busy;
static SceUID sem_shot;
static unsigned shot_no;
static const char* shot_prefix = "shot";
static unsigned drawing_frame;  /* the game's number of the frame being drawn (rd_vgl_present's count) */
/* MVC2_SHOT_SOFT=1: with each picture (SELECT + L), rd_soft's of the very same frame, drawn on the game's thread
   as it is handed over (a few hundred ms, only then) and written beside it as soft_n.bmp, shot_n.bmp the GPU's */
static uint32_t* soft_px;
static volatile int soft_busy;
static unsigned soft_no;
static int soft_pairs = -1;
/* a burst (SELECT + down, or MVC2_BURST=frame,frame...: the game's frames to start one at): BURST_N pictures at half
   size, one every BURST_STEP frames (two seconds of motion), kept in a block of their own (not the heap) and written
   when it ends as burst_<first frame>_<k>.bmp, to follow a move frame by frame */
#define BURST_N 30
#define BURST_STEP 4
#define BURST_W (SCREEN_W / 2)
#define BURST_H (SCREEN_H / 2)
#define BURST_ROW ((BURST_W * 3 + 3) & ~3)
static uint8_t* burst_mem;
static int burst_left, burst_k, burst_tick;
static unsigned burst_first;
static volatile int burst_ready;    /* set for the file thread: burst_k pictures to write */

/* MVC2_SHOT=n,m,...: the game's frames n, m... as gpu_n.bmp, to set beside rd_soft's soft_n.bmp of the same run */
static int listed(const char* var, unsigned frame) {
    const char* list = getenv(var);
    while (list != NULL && *list != 0) {
        if ((unsigned)strtoul(list, NULL, 10) == frame) {
            return 1;
        }
        list = strchr(list, ',');
        list = list != NULL ? list + 1 : NULL;
    }
    return 0;
}

static void read_commands(void);
static int control_on;
static void watch_threads(void);
static void log_threads(void);

/* the render thread's files, here: the pictures, and control.txt once a second (opening a file can take half a
   second on a console whose writes are watched) */
static int io_main(SceSize args, void* argp) {
    uint64_t last_control = 0, last_watch = 0, last_threads_log = 0;
    (void)args, (void)argp;
    for (;;) {
        static const int row = SCREEN_W * 3;
        uint8_t* bmp;
        char name[96];
        int y;
        SceUInt timeout = 250000;
        int got = sceKernelWaitSema(sem_shot, 1, &timeout) >= 0;
        if (control_on && now_us() - last_control >= 1000000) {
            last_control = now_us();
            read_commands();
        }
        if (now_us() - last_watch >= 500000) {
            last_watch = now_us();
            watch_threads();
        }
        if (now_us() - last_threads_log >= 2000000) {
            last_threads_log = now_us();
            log_threads();
        }
        (void)got;
        if (soft_busy) {
            static const int srow = RD_WIDTH * 3;
            static uint8_t* sb;
            if (sb == NULL && (sb = malloc(54 + srow * RD_HEIGHT)) == NULL) {
                host_log("vgl: no memory for rd_soft's picture");
            }
            if (sb != NULL) {
                uint32_t v[6] = { 54 + srow * RD_HEIGHT, 0, 54, 40, RD_WIDTH, RD_HEIGHT };
                SceUID fd;
                memset(sb, 0, 54);
                sb[0] = 'B', sb[1] = 'M';
                memcpy(sb + 2, &v[0], 4), memcpy(sb + 6, &v[1], 4), memcpy(sb + 10, &v[2], 4);
                memcpy(sb + 14, &v[3], 4), memcpy(sb + 18, &v[4], 4), memcpy(sb + 22, &v[5], 4);
                sb[26] = 1, sb[28] = 24;
                for (y = 0; y < RD_HEIGHT; y++) {   /* rd_soft's rows run top down, a BMP's bottom up */
                    const uint32_t* sp = soft_px + (RD_HEIGHT - 1 - y) * RD_WIDTH;
                    uint8_t* d = sb + 54 + y * srow;
                    int x;
                    for (x = 0; x < RD_WIDTH; x++) {
                        d[x * 3] = (uint8_t)sp[x], d[x * 3 + 1] = (uint8_t)(sp[x] >> 8), d[x * 3 + 2] = (uint8_t)(sp[x] >> 16);
                    }
                }
                sceIoMkdir(DATA_DIR "shots", 0777);
                snprintf(name, sizeof name, DATA_DIR "shots/soft_%06u.bmp", soft_no);
                fd = sceIoOpen(name, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
                if (fd >= 0) {
                    sceIoWrite(fd, sb, 54 + srow * RD_HEIGHT);
                    sceIoClose(fd);
                    host_log("vgl: picture %s (rd_soft's)", name);
                }
            }
            soft_busy = 0;
        }
        if (trace_ready == 1) {
            /* one text in memory, one write */
            size_t room = (size_t)trace_n * 96 + 256, len = 0;
            char* txt = malloc(room);
            unsigned i;
            if (txt != NULL) {
                len += snprintf(txt, room, "frame,wait,prep,gl,extras,swap,missed,uploads,up_kb,tris,game,sound,game_wait,pieces\n");
                for (i = 0; i < trace_n; i++) {
                    const TRACE_ROW* r = &trace[TRACE_SLOT(trace_at - trace_n + 1 + i)];
                    len += snprintf(txt + len, room - len, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", trace_at - trace_n + 1 + i, r->wait,
                                    r->prep, r->gl, r->extras, r->swap, r->missed, r->uploads, r->up_kb, r->tris, r->game,
                                    r->sound, r->game_wait, r->pieces);
                }
                {
                    SceUID fd = sceIoOpen(DATA_DIR "trace.csv", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
                    if (fd >= 0) {
                        sceIoWrite(fd, txt, len);
                        sceIoClose(fd);
                    }
                    host_log("vgl: trace of %u frames written (%08X)", trace_n, fd);
                }
                free(txt);
            }
            trace_ready = 2;
        }
        if (burst_ready) {
            static uint8_t head[54];
            int k;
            uint32_t v[6] = { 54 + BURST_ROW * BURST_H, 0, 54, 40, BURST_W, BURST_H };
            memset(head, 0, 54);
            head[0] = 'B', head[1] = 'M';
            memcpy(head + 2, &v[0], 4), memcpy(head + 6, &v[1], 4), memcpy(head + 10, &v[2], 4);
            memcpy(head + 14, &v[3], 4), memcpy(head + 18, &v[4], 4), memcpy(head + 22, &v[5], 4);
            head[26] = 1, head[28] = 24;
            sceIoMkdir(DATA_DIR "shots", 0777);
            for (k = 0; k < burst_ready; k++) {
                SceUID fd;
                snprintf(name, sizeof name, DATA_DIR "shots/burst_%06u_%02d.bmp", burst_first, k);
                fd = sceIoOpen(name, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
                if (fd >= 0) {
                    sceIoWrite(fd, head, 54);
                    sceIoWrite(fd, burst_mem + (size_t)k * BURST_ROW * BURST_H, BURST_ROW * BURST_H);
                    sceIoClose(fd);
                }
            }
            host_log("vgl: burst of %d pictures from frame %u written", burst_ready, burst_first);
            burst_ready = 0;
        }
        if (!shot_busy) {
            continue;
        }
        /* kept: a picture's 1.5 MB asked of the heap each time came back NULL late in a run, and none was written */
        {
            static uint8_t* keep;
            if (keep == NULL && (keep = malloc(54 + row * SCREEN_H)) == NULL) {
                host_log("vgl: no memory for a picture");
            }
            bmp = keep;
        }
        if (bmp != NULL) {
            uint32_t v[6] = { 54 + row * SCREEN_H, 0, 54, 40, SCREEN_W, SCREEN_H };
            SceUID fd;
            memset(bmp, 0, 54);
            bmp[0] = 'B', bmp[1] = 'M';
            memcpy(bmp + 2, &v[0], 4), memcpy(bmp + 6, &v[1], 4), memcpy(bmp + 10, &v[2], 4);
            memcpy(bmp + 14, &v[3], 4), memcpy(bmp + 18, &v[4], 4), memcpy(bmp + 22, &v[5], 4);
            bmp[26] = 1, bmp[28] = 24;
            for (y = 0; y < SCREEN_H; y++) {    /* GL's rows run bottom up, as a BMP's */
                const uint8_t* s = shot_px + y * SCREEN_W * 4;
                uint8_t* d = bmp + 54 + y * row;
                int x;
                for (x = 0; x < SCREEN_W; x++) {
                    d[x * 3] = s[x * 4 + 2], d[x * 3 + 1] = s[x * 4 + 1], d[x * 3 + 2] = s[x * 4];
                }
            }
            sceIoMkdir(DATA_DIR "shots", 0777);
            snprintf(name, sizeof name, DATA_DIR "shots/%s_%06u.bmp", shot_prefix, shot_no);
            fd = sceIoOpen(name, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
            if (fd >= 0) {
                sceIoWrite(fd, bmp, 54 + row * SCREEN_H);
                sceIoClose(fd);
                host_log("vgl: picture %s", name);
            } else {
                host_log("vgl: picture %s not opened (%08X)", name, fd);
            }
        }
        shot_busy = 0;
    }
    return 0;
}

static void io_start(void) {
    SceUID th;
    sem_shot = sceKernelCreateSema("vgl_io", 0, 0, 1, NULL);
    th = sceKernelCreateThread("vgl_io", io_main, 180, 64 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        host_log("vgl: no file thread (%08X): no pictures or control.txt", th);
    } else {
        rd_vgl_watch_thread("IO", th);
    }
}

static void save_shot(const char* prefix, unsigned no) {
    if (shot_busy) {
        return;
    }
    shot_prefix = prefix;
    if (shot_px == NULL && (shot_px = malloc(SCREEN_W * SCREEN_H * 4)) == NULL) {
        return;
    }
    glReadPixels(0, 0, SCREEN_W, SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, shot_px);
    shot_no = no;
    shot_busy = 1;
    sceKernelSignalSema(sem_shot, 1);
}

/* the frame's batches into dump_<frame>.txt, written once */
static void dump_batches(void) {
    static const char* const lists[] = { "opaque", "punch", "trans" };
    size_t room = 256 + (size_t)nbatches * 300 + (size_t)nverts * 130, n = 0;
    char* out = malloc(room);
    char name[96];
    SceUID fd;
    int i;
    if (out == NULL) {
        return;
    }
    n += snprintf(out + n, room - n, "frame %u: %d batches, %d vertices, %d depths, fog %08X density %g on %d\n",
                  render_no, nbatches, nverts, nz, (unsigned)fog.color, fog.density, fog_on);
    for (i = 0; i < nbatches && n < room - 300; i++) {
        const BATCH* b = &batches[i];
        const RD_STATE* s = &b->s;
        const VTX* v = &verts[b->first];
        float dlo = 2.0f, dhi = -1.0f;
        int k;
        for (k = 0; k < b->count; k++) {
            const VTX* q = &verts[b->first + k];
            float d = q->w > 0 ? q->z / q->w : 0;
            dlo = d < dlo ? d : dlo;
            dhi = d > dhi ? d : dhi;
        }
        n += snprintf(out + n, room - n, "%s", "");
        n += snprintf(out + n, room - n, "d %.5f..%.5f ", dlo, dhi);
        n += snprintf(out + n, room - n,
                      "%4d %-6s n %4d tex %dx%d f%d pal %d sh %d ta %d ua %d bil %d off %d fog %d uv %X depth %d zw %d "
                      "blend %d/%d | v0 %.1f,%.1f d %.5f c %02X%02X%02X%02X\n",
                      i, lists[b->list], b->count / 3, b->tex ? b->tex->w : 0, b->tex ? b->tex->h : 0,
                      b->tex ? b->tex->format : -1, s->palette, s->shading, s->tex_alpha, s->use_alpha, s->bilinear,
                      s->offset, s->fog, s->uv_mode, s->depth, s->zwrite, s->src, s->dst,
                      v->w > 0 ? (v->x / v->w - ndc_bx) / ndc_kx : 0, v->w > 0 ? (1 - v->y / v->w) * 240 : 0,
                      v->w > 0 ? v->z / v->w : 0, v->base[3], v->base[0], v->base[1], v->base[2]);
        /* a small batch's every vertex: x, y in the game's pixels, its depth, the game's z (1/w), u, v, colours */
        for (k = 0; b->count <= 3000 && k < b->count && n < room - 100; k++) {
            const VTX* q = &verts[b->first + k];
            float iw = q->w > 0 ? 1.0f / q->w : 0;
            n += snprintf(out + n, room - n, "    %8.2f %8.2f d %.7f z %.7f uv %.4f %.4f c %02X%02X%02X%02X o %02X%02X%02X%02X\n",
                          (q->x * iw - ndc_bx) / ndc_kx, (1 - q->y * iw) * 240, q->z * iw, q->fw > 0 ? 1.0f / q->fw : 0, q->u, q->v,
                          q->base[3], q->base[0], q->base[1], q->base[2], q->offs[3], q->offs[0], q->offs[1], q->offs[2]);
        }
    }
    snprintf(name, sizeof name, DATA_DIR "dump_%06u.txt", drawing_frame);
    fd = sceIoOpen(name, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, out, n);
        sceIoClose(fd);
        host_log("vgl: batches in %s", name);
    }
    free(out);
}

/* ux0:data/mvc_2/control.txt, written by a tool on the PC: "<number> <words>" with shot, dump or perf. Done once
   for each new number; the file is never deleted here, so a tool's own write of it (a transaction: the old file
   moved aside, the new one renamed in) is not raced */
static int control_last = -1;
/* MVC2_CONTROL=1: control.txt read once a second. Off by default: on this console a file's open can take half a
   second, and an open on one thread then holds up the game's own opens (its loads) */
/* "gputime": for this many frames, the GPU waited for after each (glFinish), its time logged: costs the overlap */
static int gpu_time_left;
static uint64_t gpu_time_sum, gpu_time_worst;
static unsigned gpu_time_frames;

/* the number in control.txt when the game starts: a command from an earlier run is not done again */
static void control_start(void) {
    char buf[32];
    SceUID fd = sceIoOpen(DATA_DIR "control.txt", SCE_O_RDONLY, 0);
    int n;
    control_last = 0;
    if (fd >= 0) {
        n = sceIoRead(fd, buf, sizeof buf - 1);
        sceIoClose(fd);
        buf[n > 0 ? n : 0] = 0;
        sscanf(buf, "%d", &control_last);
    }
}

static void read_commands(void) {
    int last = control_last;
    char buf[128];
    SceUID fd = sceIoOpen(DATA_DIR "control.txt", SCE_O_RDONLY, 0);
    int n, seq;
    if (fd < 0) {
        return;
    }
    n = sceIoRead(fd, buf, sizeof buf - 1);
    sceIoClose(fd);
    buf[n > 0 ? n : 0] = 0;
    if (sscanf(buf, "%d", &seq) != 1 || seq == last) {
        return;
    }
    control_last = seq;
    if (strstr(buf, "shot") != NULL) {
        __sync_fetch_and_or(&requests, RD_VGL_SHOT);
    }
    if (strstr(buf, "dump") != NULL) {
        __sync_fetch_and_or(&requests, RD_VGL_DUMP);
    }
    if (strstr(buf, "perf") != NULL) {
        __sync_fetch_and_or(&requests, RD_VGL_PERF);
    }
    if (strstr(buf, "gputime") != NULL) {
        gpu_time_left = 120;
    }
    host_log("vgl: control.txt: %s", buf);
}

/* ---- the port's own pictures (app0:assets, from ports/tools/vita_assets.py): 960 by 544, RGB 565 ---- */

#define PIC_W 960
#define PIC_H 544

/* a picture as a GL texture (0 when it could not be read) */
static GLuint pic_load(const char* name) {
    char path[96];
    GLuint t = 0;
    size_t n = (size_t)PIC_W * PIC_H * 2;
    uint16_t* px = malloc(n);
    FILE* f;
    snprintf(path, sizeof path, "app0:assets/%s.565", name);
    f = fopen(path, "rb");
    if (px != NULL && f != NULL && fread(px, 1, n, f) == n) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        cur.tex = t;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, PIC_W, PIC_H, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        host_log("vgl: picture %s not read", path);
    }
    if (f != NULL) {
        fclose(f);
    }
    free(px);
    return t;
}

/* a 1 by 1 texture of one colour (for a bar) */
static GLuint pic_colour(uint8_t r, uint8_t g, uint8_t b) {
    GLuint t;
    uint16_t px = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    cur.tex = t;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, &px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

/* the screen's pixels x0..x1, y0..y1 (top down) from the texture's u0..u1, v0..v1 (0 to 1), opaque, over what is there;
   the viewport and scissor left whole */
static void pic_draw(GLuint tex, float tw, float th, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                     float v1) {
    static const float zero[4] = { 0, 0, 0, 0 }, decal[4] = { 0, 0, 0, 1 };
    float size[4] = { 0, 0, tw, th };
    float nx0 = x0 / SCREEN_W * 2 - 1, nx1 = x1 / SCREEN_W * 2 - 1, ny0 = 1 - y0 / SCREEN_H * 2, ny1 = 1 - y1 / SCREEN_H * 2;
    const float xy[6][4] = { { nx0, ny0, u0, v0 }, { nx1, ny0, u1, v0 }, { nx0, ny1, u0, v1 },
                             { nx0, ny1, u0, v1 }, { nx1, ny0, u1, v0 }, { nx1, ny1, u1, v1 } };
    VTX q[6];
    int i, op;
    if (tex == 0) {
        return;
    }
    memset(q, 0, sizeof q);
    for (i = 0; i < 6; i++) {
        q[i].x = xy[i][0], q[i].y = xy[i][1], q[i].z = 0.0f, q[i].w = 1.0f, q[i].u = xy[i][2], q[i].v = xy[i][3];
        memcpy(q[i].clamp, no_clamp, sizeof no_clamp);
        q[i].fw = 1.0f;
        memset(q[i].base, 255, 4);
    }
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    op = program_for(P_TEX + P_SHADE * 4);    /* decal */
    use_program(op);
    set_uniform(op, 0, progs[op].shade, decal);
    set_uniform(op, 1, progs[op].fog, zero);
    set_uniform(op, 2, progs[op].fog_color, zero);
    set_uniform(op, 3, progs[op].misc, size);
    glBindTexture(GL_TEXTURE_2D, tex);
    cur.tex = tex;
    set_blend(0, RD_ONE, RD_ZERO);
    set_depth(RD_ALWAYS, 0);
    attribs(q);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* ASPECT_RATIO=ORIGINAL: the pillars beside the game's 4:3 drawn from Borders.png (its middle, where the game is,
   transparent in the PNG, is not drawn) */
static GLuint borders_tex;
static void draw_borders(void) {
    float r0 = view_x0 + view_w;
    if (aspect_mode != ASPECT_ORIGINAL || borders_tex == 0) {
        return;
    }
    pic_draw(borders_tex, PIC_W, PIC_H, 0, 0, view_x0, SCREEN_H, 0, 0, view_x0 / PIC_W, 1);
    pic_draw(borders_tex, PIC_W, PIC_H, r0, 0, SCREEN_W, SCREEN_H, r0 / PIC_W, 0, 1, 1);
}

/* the first start's screen while the disc's files are unpacked (rd_vgl_boot_progress): First_boot.png, its black bar
   filled in yellow from the left as far as the unpacking has come */
#define BOOT_BAR_X0 45.0f   /* inside the picture's black bar (42 to 392, 240 to 270): a 3-pixel black frame kept */
#define BOOT_BAR_X1 390.0f
#define BOOT_BAR_Y0 243.0f
#define BOOT_BAR_Y1 268.0f
static volatile int boot_on, boot_permille, boot_shown;
static GLuint boot_tex, boot_bar_tex;

void rd_vgl_boot_progress(int permille) {
    if (permille < 0) {
        boot_on = 0;
        return;
    }
    boot_permille = permille > 1000 ? 1000 : permille;
    if (!boot_on) {
        host_log("vgl: the first start's screen on");
    }
    boot_on = 1;
}

/* no game data found: Warning.png, until the program is left (the PS button) */
static volatile int warn_on;
static GLuint warn_tex;

void rd_vgl_warning(void) {
    warn_on = 1;
}

static void draw_warning(void) {
    if (warn_tex == 0) {
        warn_tex = pic_load("warning");
    }
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    pic_draw(warn_tex, PIC_W, PIC_H, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1);
    vglSwapBuffers(GL_FALSE);
}

static void draw_boot(void) {
    static int drawn;
    float x1;
    if (boot_tex == 0) {
        host_log("vgl: the first start's screen: loading its picture");
        boot_tex = pic_load("first_boot");
        boot_bar_tex = pic_colour(250, 200, 20);
        host_log("vgl: the first start's screen: picture %u", (unsigned)boot_tex);
    }
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    pic_draw(boot_tex, PIC_W, PIC_H, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1);
    x1 = BOOT_BAR_X0 + (BOOT_BAR_X1 - BOOT_BAR_X0) * boot_permille / 1000.0f;
    if (x1 > BOOT_BAR_X0) {
        pic_draw(boot_bar_tex, 1, 1, BOOT_BAR_X0, BOOT_BAR_Y0, x1, BOOT_BAR_Y1, 0, 0, 1, 1);
    }
    vglSwapBuffers(GL_FALSE);
    if (drawn++ == 0) {
        host_log("vgl: the first start's screen drawn");
    }
    boot_shown = 1;
}

/* the first start's screen up before the unpacking starts writing: its picture read from app0: first, as the
   unpacking's long writes to the card held that read up for more than a minute (the vitaGL splash shown meanwhile) */
void rd_vgl_boot_wait(void) {
    int i;
    for (i = 0; i < 300 && !boot_shown; i++) {
        sceKernelDelayThread(10000);
    }
    if (!boot_shown) {
        host_log("vgl: the first start's screen not up after 3 s");
    }
}

/* ---- the render thread ---- */

static void draw_frame(void) {
    uint64_t t0 = now_us();
    int i;
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    cur.depth_mask = 1;
    glClearColor(0, 0, 0, 1);
    glClearDepthf(0.0f);
    {
        uint64_t g0 = now_us();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        gl_step("the clear", g0);
    }
    draw_borders();
    glViewport((GLint)view_x0, 0, (GLsizei)view_w, SCREEN_H);
    /* the GXM clips only to its region: without the scissor a triangle past the game's sides is drawn into the
       pillars (and costs their pixels) */
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)view_x0, 0, (GLsizei)view_w, SCREEN_H);
    if (palette_serial != palette_uploaded && hwpal > 0) {
        hwpal_upload();
    }
    if (palette_serial != palette_uploaded) {
        palette_uploaded = palette_serial;
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, pal_tex);
        {
            static uint8_t rgba[1024 * 4];
            for (i = 0; i < 1024; i++) {
                rgba[i * 4] = (uint8_t)(palette[i] >> 16), rgba[i * 4 + 1] = (uint8_t)(palette[i] >> 8);
                rgba[i * 4 + 2] = (uint8_t)palette[i], rgba[i * 4 + 3] = (uint8_t)(palette[i] >> 24);
            }
            uint64_t g0 = now_us();
            static int made;
            if (!made) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                made = 1;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            }
            gl_step("the palette", g0);
        }
        glActiveTexture(GL_TEXTURE0);
    }
    if (fog_on) {
        static uint8_t table[128];
        static uint8_t last[128];
        for (i = 0; i < 128; i++) {
            table[i] = (uint8_t)(fog.table[i] * 255.0f + 0.5f);
        }
        if (memcmp(table, last, sizeof table) != 0) {
            memcpy(last, table, sizeof table);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, fog_tex);
            static int made;
            uint64_t g0 = now_us();
            if (!made) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 128, 1, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, table);
                made = 1;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, table);
            }
            gl_step("the fog table", g0);
            glActiveTexture(GL_TEXTURE0);
        }
    }
    /* the background, filling the depth buffer with its plane */
    {
        static const RD_STATE plain = { RD_OPAQUE, RD_ALWAYS, 1, RD_ONE, RD_ZERO, 0, 1, 0, 0, 0, 0, RD_FOG_NONE };
        BATCH b;
        memset(&b, 0, sizeof b);
        b.s = plain;
        b.list = RD_OPAQUE;
        b.count = 6;
        attribs(background);
        {
            static const uint8_t order[6] = { 0, 1, 2, 2, 1, 3 };
            VTX six[6];
            for (i = 0; i < 6; i++) {
                six[i] = background[order[i]];
            }
            attribs(six);
            draw_batch(&b);
        }
    }
    if (vbo_on > 0 && verts == vring[vring_slot]) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        vglBufferData(GL_ARRAY_BUFFER, verts);
        attribs(NULL);          /* offsets in the buffer */
        glBindBuffer(GL_ARRAY_BUFFER, 0);   /* the attributes keep it; the overlay's from its own array */
    } else {
        attribs(verts);
    }
    for (i = 0; i < nbatches; i++) {
        uint64_t g0 = now_us();
        draw_batch(&batches[i]);
        gl_step(batches[i].list == RD_TRANS ? "a translucent batch's draw" : "an opaque or punch batch's draw", g0);
    }
    if (overlay_on && overlay_tex != 0) {
        /* in the left pillar, over everything */
        static const float zero[4] = { 0, 0, 0, 0 }, decal[4] = { 0, 0, 0, 1 }, size[4] = { 0, 0, OV_W, OV_H };
        float x0 = 2.0f / SCREEN_W * 2 - 1, x1 = (2.0f + OV_W) / SCREEN_W * 2 - 1;
        /* WIDE has no pillar: below the gauges then */
        float top = aspect_mode == ASPECT_WIDE ? 140.0f : 2.0f;
        float y0 = 1 - top / SCREEN_H * 2, y1 = 1 - (top + OV_H) / SCREEN_H * 2;
        VTX q[6];
        const float xy[6][4] = { { x0, y0, 0, 0 }, { x1, y0, 1, 0 }, { x0, y1, 0, 1 },
                                 { x0, y1, 0, 1 }, { x1, y0, 1, 0 }, { x1, y1, 1, 1 } };
        memset(q, 0, sizeof q);
        for (i = 0; i < 6; i++) {
            q[i].x = xy[i][0], q[i].y = xy[i][1], q[i].z = 0.0f, q[i].w = 1.0f, q[i].u = xy[i][2], q[i].v = xy[i][3];
            memcpy(q[i].clamp, no_clamp, sizeof no_clamp);
            q[i].fw = 1.0f;
            memset(q[i].base, 255, 4);
        }
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, SCREEN_W, SCREEN_H);
        const int op = program_for(P_TEX + P_SHADE * 4);    /* decal, the texel's alpha */
        use_program(op);
        set_uniform(op, 0, progs[op].shade, decal);
        set_uniform(op, 1, progs[op].fog, zero);
        set_uniform(op, 2, progs[op].fog_color, zero);
        set_uniform(op, 3, progs[op].misc, size);
        glBindTexture(GL_TEXTURE_2D, overlay_tex);
        cur.tex = overlay_tex;
        set_blend(1, RD_SRC_ALPHA, RD_INV_SRC_ALPHA);
        set_depth(RD_ALWAYS, 0);
        attribs(q);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    if (dbg_menu_open() && dmenu_update()) {
        /* the debug menu, in the middle, over everything */
        static const float zero[4] = { 0, 0, 0, 0 }, decal[4] = { 0, 0, 0, 1 }, size[4] = { 0, 0, DM_W, DM_H };
        float w = DM_W * 1.5f, h = DM_H * 1.5f, left = (SCREEN_W - w) * 0.5f, top = (SCREEN_H - h) * 0.5f;
        float x0 = left / SCREEN_W * 2 - 1, x1 = (left + w) / SCREEN_W * 2 - 1;
        float y0 = 1 - top / SCREEN_H * 2, y1 = 1 - (top + h) / SCREEN_H * 2;
        VTX q[6];
        const float xy[6][4] = { { x0, y0, 0, 0 }, { x1, y0, 1, 0 }, { x0, y1, 0, 1 },
                                 { x0, y1, 0, 1 }, { x1, y0, 1, 0 }, { x1, y1, 1, 1 } };
        memset(q, 0, sizeof q);
        for (i = 0; i < 6; i++) {
            q[i].x = xy[i][0], q[i].y = xy[i][1], q[i].z = 0.0f, q[i].w = 1.0f, q[i].u = xy[i][2], q[i].v = xy[i][3];
            memcpy(q[i].clamp, no_clamp, sizeof no_clamp);
            q[i].fw = 1.0f;
            memset(q[i].base, 255, 4);
        }
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, SCREEN_W, SCREEN_H);
        const int op = program_for(P_TEX + P_SHADE * 4);
        use_program(op);
        set_uniform(op, 0, progs[op].shade, decal);
        set_uniform(op, 1, progs[op].fog, zero);
        set_uniform(op, 2, progs[op].fog_color, zero);
        set_uniform(op, 3, progs[op].misc, size);
        glBindTexture(GL_TEXTURE_2D, dmenu_tex);
        cur.tex = dmenu_tex;
        set_blend(1, RD_SRC_ALPHA, RD_INV_SRC_ALPHA);
        set_depth(RD_ALWAYS, 0);
        attribs(q);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    perf.gl_us += now_us() - t0;
}

static int render_main(SceSize args, void* argp) {
    int i, ok;
    const char* aspect = getenv("MVC2_ASPECT");
    (void)args, (void)argp;
    if (aspect == NULL) {
        aspect = config_get("aspect");
    }
    {
        const char* ar = getenv("ASPECT_RATIO");
        if (ar != NULL) {
            aspect_mode = strcasecmp(ar, "ORIGINAL") == 0 ? ASPECT_ORIGINAL
                          : strcasecmp(ar, "STRETCHED") == 0 ? ASPECT_STRETCHED : ASPECT_WIDE;
        } else if (aspect != NULL) {
            aspect_mode = strcmp(aspect, "stretch") == 0 ? ASPECT_STRETCHED : ASPECT_ORIGINAL;
        }
    }
    view_w = aspect_mode == ASPECT_ORIGINAL ? SCREEN_H * 4.0f / 3.0f : SCREEN_W;
    if (aspect_mode == ASPECT_WIDE) {
        /* the vertical's scale across: 847 of the game's pixels on the 960, 103.5 more each side */
        game_w = SCREEN_W * (float)RD_HEIGHT / SCREEN_H;
        game_x0 = (RD_WIDTH - game_w) * 0.5f;
        host_wide_x = -game_x0;
    }
    ndc_kx = 2.0f / game_w;
    ndc_bx = -game_x0 * ndc_kx - 1.0f;
    /* whole and even: vitaGL's glViewport halves the width with a shift (725 drew 724 wide from 117), and the
       snap's game pixels must be exactly the ones the geometry lands on */
    view_w = (float)((int)view_w & ~1);
    view_x0 = (float)(((int)SCREEN_W - (int)view_w) / 2);
    /* the GPU's room for a frame's geometry (its parameter buffer): a hyper combo's many large translucent
       triangles filled the default, and the GPU then stalled the next frame's start by up to half a second */
    vglSetParamBufferSize(48 * 1024 * 1024);   /* 32 MB: half-second stalls came back in hypers once a vertex carried its clamp too */
    vglSetupGarbageCollector(0x10000100 - 10, SCE_KERNEL_CPU_MASK_USER_2);
    vglSetupRuntimeShaderCompiler(SHARK_OPT_FAST, GL_TRUE, GL_FALSE, GL_TRUE);
    {
        /* vitaGL's RAM pool is the free memory less the threshold, and fails past 200 MB: 64 MB of it, the
           rest left (textures and the frame's buffers go to the 112 MB of VRAM first) */
        SceKernelFreeMemorySizeInfo mem = { sizeof mem };
        int threshold;
        sceKernelGetFreeMemorySize(&mem);
        threshold = mem.size_user - 64 * 1024 * 1024;
        threshold = threshold < 16 * 1024 * 1024 ? 16 * 1024 * 1024 : threshold;
        host_log("vgl: free user memory %d KB, RAM pool threshold %d KB", mem.size_user >> 10, threshold >> 10);
        ok = vglInitExtended(0, SCREEN_W, SCREEN_H, threshold, SCE_GXM_MULTISAMPLE_NONE);
    }
    /* the vdpm build's return value is a stack word on its success path, not 1: what follows (the shaders
       linking, GL errors, a picture) tells whether it started */
    host_log("vgl: vglInitExtended returned %d; free VRAM %u KB", ok, (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10));
    vglWaitVblankStart(GL_TRUE);
    {
        /* what a time stamp costs: they are taken per batch and per upload */
        uint64_t a = now_us(), b;
        int k;
        for (k = 0; k < 10000; k++) {
            b = now_us();
        }
        b = now_us();
        host_log("vgl: a time stamp costs %.3f us", (b - a) / 10001.0);
    }
    fsx = getenv("MVC2_FSX") != NULL ? atoi(getenv("MVC2_FSX")) : 0;
    snap_on = getenv("MVC2_SNAP") == NULL ? 1 : atoi(getenv("MVC2_SNAP"));
    snap_ox = getenv("MVC2_SNAP_OX") == NULL ? 0.0f : (float)atof(getenv("MVC2_SNAP_OX"));
    half_on = getenv("MVC2_HALF") == NULL || atoi(getenv("MVC2_HALF")) != 0;   /* on unless MVC2_HALF=0: 1 or 2 levels in 255 at most, the GPU much faster */
    hwpal = getenv("MVC2_HWPAL") != NULL && atoi(getenv("MVC2_HWPAL")) != 0;
    host_log("vgl: palettes looked up by %s; colours in %s", hwpal ? "the texture unit (MVC2_HWPAL)" : "the shaders",
             half_on ? "half floats (MVC2_HALF)" : "floats");
    verts = verts_heap = malloc(sizeof(VTX) * MAX_VERTS);
    batches = malloc(sizeof(BATCH) * MAX_BATCHES);
    conv = malloc(1024 * 1024 * 2);
    vb_main = (VBUF){ verts, &nverts, batches, &nbatches, MAX_VERTS, MAX_BATCHES, 0, 0, 0.0f, 0.0f };
    vring_start();
    if (verts == NULL || batches == NULL || conv == NULL || !make_programs()) {
        host_log("vgl: could not start (memory or the shaders: is ur0:data/libshacccg.suprx there?)");
        sceKernelSignalSema(sem_started, 1);
        return 0;
    }
    cut_thread_start();
    watchdog_start();
    glGenTextures(1, &pal_tex);
    glGenTextures(1, &fog_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, pal_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fog_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    for (i = 0; i < 6; i++) {
        glEnableVertexAttribArray(i);
    }
    depth_mode = getenv("MVC2_DEPTHMODE") != NULL ? atoi(getenv("MVC2_DEPTHMODE")) : 0;
    if (depth_mode != 1) {
        glDepthRangef(-1.0f, 1.0f);     /* d through as it is: no 2 d - 1 rounding near 0 */
    }
    {
        GLfloat r[2] = { 0, 0 };
        glGetFloatv(GL_DEPTH_RANGE, r);
        host_log("vgl: depth range %g to %g (wanted -1 to 1)", r[0], r[1]);
    }
    seams_fix = getenv("MVC2_SEAMS") == NULL || atoi(getenv("MVC2_SEAMS")) != 0;
    memset(&cur, 0xFF, sizeof cur);
    host_log("vgl: aspect %s: the game's x %.1f to %.1f on the screen's %.0f from x %.0f",
             aspect_mode == ASPECT_WIDE ? "WIDE" : aspect_mode == ASPECT_STRETCHED ? "STRETCHED" : "ORIGINAL",
             game_x0, game_x0 + game_w, view_w, view_x0);
    host_log("vgl: started, %dx%d, the game at %.0fx%d from x %.0f; free VRAM %u KB, RAM %u KB", SCREEN_W, SCREEN_H,
             view_w, SCREEN_H, view_x0, (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10), (unsigned)(vglMemFree(VGL_MEM_RAM) >> 10));
    control_on = getenv("MVC2_CONTROL") != NULL && atoi(getenv("MVC2_CONTROL")) != 0;
    overlay_on = getenv("MVC2_OVERLAY") != NULL && atoi(getenv("MVC2_OVERLAY")) != 0;
    if (getenv("MVC2_TRACE") != NULL && atoi(getenv("MVC2_TRACE")) > 0) {
        trace_n = (unsigned)atoi(getenv("MVC2_TRACE"));
        trace = calloc(trace_n, sizeof *trace);
        trace_at = getenv("MVC2_TRACE_AT") != NULL && (unsigned)atoi(getenv("MVC2_TRACE_AT")) >= trace_n
                   ? (unsigned)atoi(getenv("MVC2_TRACE_AT")) : trace_n;
        if (getenv("MVC2_GPUTIME") != NULL && atoi(getenv("MVC2_GPUTIME")) != 0) {
            gpu_time_left = 1 << 30;    /* glFinish after each frame's draw calls: the GPU's time in the trace's extras */
        }
        host_log("vgl: tracing frames %u to %u%s", trace_at - trace_n + 1, trace_at, trace != NULL ? "" : ": no memory");
    }
    if (control_on) {
        control_start();
    }
    io_start();
    if (aspect_mode == ASPECT_ORIGINAL) {
        borders_tex = pic_load("borders");
    }
    started_ok = 1;
    sceKernelSignalSema(sem_started, 1);
    perf.last_vcount = sceDisplayGetVcount();
    for (;;) {
        VFRAME* v;
        uint64_t t0, t1, t2, tw = now_us(), tp, td, tend;
        unsigned vc_missed_now = 0;
        int want;
        /* the next frame, waited for a thirtieth of a second at a time: the first start's unpacking (which may begin
           while this waits, before the game's first frame) has its screen drawn each time none came. A frame comes
           every 16.7 ms in a game, so the wait runs out only in a pause */
        for (;;) {
            SceUInt timeout = 33000;
            if (sceKernelWaitSema(sem_ready, 1, &timeout) >= 0) {
                break;
            }
            if (boot_on) {
                draw_boot();
            } else if (warn_on) {
                draw_warning();
            }
        }
        if (boot_tex != 0 && !boot_on) {
            glDeleteTextures(1, &boot_tex);     /* the unpacking done: its picture's memory back */
            glDeleteTextures(1, &boot_bar_tex);
            boot_tex = boot_bar_tex = 0;
        }
        t0 = now_us();
        v = vf[render_no & 1];
        render_no++;
        drawing_frame = v->frame_no;
        drawing_frame_wd = drawing_frame;
        want = v->shot ? RD_VGL_SHOT : 0;
        v->shot = 0;
        prepare(v);
        tp = now_us();
        sceKernelSignalSema(sem_free, 1);   /* the frame goes back to the game */
        want |= __sync_fetch_and_and(&requests, 0);
        draw_frame();
        td = now_us();
        last_draw_us = td - tp;
        if (gpu_time_left > 0) {
            uint64_t g0 = now_us(), g;
            glFinish();
            g = now_us() - g0;
            gpu_time_sum += g;
            gpu_time_worst = g > gpu_time_worst ? g : gpu_time_worst;
            gpu_time_frames++;
            if (--gpu_time_left == 0) {
                host_log("vgl: GPU after the draw calls: %.2f ms a frame, worst %.2f, over %u frames (%u tris, %d batches "
                         "the last)", gpu_time_sum / 1000.0 / gpu_time_frames, gpu_time_worst / 1000.0, gpu_time_frames,
                         (unsigned)(nverts / 3), nbatches);
                gpu_time_sum = gpu_time_worst = 0;
                gpu_time_frames = 0;
            }
        }
        if (want & RD_VGL_SHOT) {
            save_shot("shot", soft_pairs > 0 ? drawing_frame : render_no);
        } else if (listed("MVC2_SHOT", drawing_frame)) {
            save_shot("gpu", drawing_frame);
        }
        if (((want & RD_VGL_BURST) || listed("MVC2_BURST", drawing_frame)) && burst_left == 0 && !burst_ready) {
            if (burst_mem == NULL) {
                SceUID blk = sceKernelAllocMemBlock("vgl_burst", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                                    ((size_t)BURST_N * BURST_ROW * BURST_H + 0xFFFFF) & ~0xFFFFF, NULL);
                if (blk < 0 || sceKernelGetMemBlockBase(blk, (void**)&burst_mem) < 0) {
                    host_log("vgl: no block for a burst (%08X)", blk);
                    burst_mem = NULL;
                }
            }
            if (burst_mem != NULL && (shot_px != NULL || (shot_px = malloc(SCREEN_W * SCREEN_H * 4)) != NULL)) {
                burst_left = BURST_N, burst_k = 0, burst_tick = 0, burst_first = drawing_frame;
                host_log("vgl: a burst from frame %u", burst_first);
            }
        }
        if (burst_left > 0 && burst_tick++ % BURST_STEP == 0 && !shot_busy) {
            /* the frame just drawn, halved (each 2 by 2 averaged), its rows bottom up as GL's and a BMP's */
            uint8_t* out = burst_mem + (size_t)burst_k * BURST_ROW * BURST_H;
            int y, x;
            glReadPixels(0, 0, SCREEN_W, SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, shot_px);
            for (y = 0; y < BURST_H; y++) {
                const uint8_t *r0 = shot_px + (size_t)(2 * y) * SCREEN_W * 4, *r1 = r0 + SCREEN_W * 4;
                uint8_t* d = out + (size_t)y * BURST_ROW;
                for (x = 0; x < BURST_W; x++) {
                    const uint8_t *a = r0 + 8 * x, *b = r1 + 8 * x;
                    d[3 * x] = (uint8_t)((a[2] + a[6] + b[2] + b[6] + 2) >> 2);
                    d[3 * x + 1] = (uint8_t)((a[1] + a[5] + b[1] + b[5] + 2) >> 2);
                    d[3 * x + 2] = (uint8_t)((a[0] + a[4] + b[0] + b[4] + 2) >> 2);
                }
            }
            burst_k++;
            if (--burst_left == 0) {
                burst_ready = burst_k;
                sceKernelSignalSema(sem_shot, 1);
            }
        }
        if ((want & RD_VGL_DUMP) || listed("MVC2_DUMP", drawing_frame)) {
            dump_batches();
        }
        t1 = now_us();
        vglSwapBuffers(GL_FALSE);
        t2 = now_us();
        perf.swap_us += t2 - t1;
        {
            unsigned vc = sceDisplayGetVcount(), us = (unsigned)(t2 - (perf.last_swap ? perf.last_swap : t0));
            vc_missed_now = vc - perf.last_vcount > 1 ? vc - perf.last_vcount - 1 : 0;
            if (vc - perf.last_vcount > 1) {
                perf.missed += vc - perf.last_vcount - 1;
            }
            perf.last_vcount = vc;
            perf.last_swap = t2;
            perf.worst_us = us > perf.worst_us ? us : perf.worst_us;
        }
        if (trace != NULL && TRACE_ON(drawing_frame)) {
            static unsigned up_was, kb_was;
            TRACE_ROW* r = &trace[TRACE_SLOT(drawing_frame)];
            unsigned up = perf.uploads - up_was, kb = perf.upload_kb - kb_was;
            if (perf.uploads < up_was) {    /* the window's figures were reset */
                up = perf.uploads, kb = perf.upload_kb;
            }
            up_was = perf.uploads, kb_was = perf.upload_kb;
            r->wait = (uint32_t)(t0 - tw), r->prep = (uint32_t)(tp - t0), r->gl = (uint32_t)(td - tp);
            r->extras = (uint32_t)(t1 - td), r->swap = (uint32_t)(t2 - t1), r->missed = vc_missed_now;
            r->uploads = up, r->up_kb = kb, r->tris = (uint32_t)(nverts / 3), r->pieces = (uint32_t)npieces;
            if (drawing_frame == trace_at) {
                trace_ready = 1;
                sceKernelSignalSema(sem_shot, 1);
            }
        }
        perf.frames++;
        ov.frames++;
        ov.prep += tp - t0;
        ov.gl += td - tp;
        ov.swap += t2 - t1;
        if (vc_missed_now) {
            ov.missed += vc_missed_now;
        }
        if ((unsigned)(t2 - tw) > ov.worst_us) {
            ov.worst_us = (unsigned)(t2 - tw);
        }
        if (overlay_on && render_no % 30 == 0) {
            overlay_update();
        }
        if (render_no % 60 == 0) {
            GLenum e = glGetError();
            if (e != GL_NO_ERROR) {
                host_log("vgl: GL error %04X by frame %u", e, render_no);
            }
        }
        tex_sweep();
        tend = now_us();
        /* the slowest loop of the window, part by part: waiting for the game's frame, reading it, the GL calls,
           the pictures and dumps, the swap, and the rest (the sweep, the log) */
        if (tend - tw > perf.slow_total) {
            perf.slow_total = tend - tw;
            perf.slow[0] = t0 - tw, perf.slow[1] = tp - t0, perf.slow[2] = td - tp, perf.slow[3] = t1 - td;
            perf.slow[4] = t2 - t1, perf.slow[5] = tend - t2;
        }
        {
            uint64_t r0 = now_us();
            perf_report(want & RD_VGL_PERF);
            report_us = now_us() - r0 > report_us ? now_us() - r0 : report_us;
        }
    }
    return 0;
}

/* ---- the game's side ---- */

int rd_vgl_init(void) {
    SceUID th;
    int i;
    for (i = 0; i < 2; i++) {
        vf[i] = calloc(1, sizeof(VFRAME));
        if (vf[i] == NULL) {
            return 0;
        }
    }
    vf[0]->f = rd_frame;
    vf[1]->f = malloc(sizeof(RD_FRAME));
    if (vf[1]->f == NULL) {
        return 0;
    }
    fill = 0;
    sem_ready = sceKernelCreateSema("vgl_ready", 0, 0, 1, NULL);
    sem_free = sceKernelCreateSema("vgl_free", 0, 1, 1, NULL);
    sem_started = sceKernelCreateSema("vgl_started", 0, 0, 1, NULL);
    rd_vgl_watch_thread("HOST", sceKernelGetThreadId());
    th = sceKernelCreateThread("vgl_render", render_main, 0x10000100 - 4, 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) {
        rd_vgl_watch_thread("RNDR", th);
    }
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        host_log("vgl: no render thread (%08X)", th);
        return 0;
    }
    sceKernelWaitSema(sem_started, 1, NULL);
    return started_ok;
}

void rd_vgl_request(int what) {
    requests |= what;
}

/* SELECT + L: a picture; SELECT + R: the next frame's batches */
static void debug_keys(void) {
    static uint32_t held;
    SceCtrlData pad;
    uint32_t now;
    if (!vita_debug) {
        return;         /* DEBUG_OPTIONS=0: SELECT is the game's again */
    }
    if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) < 0) {    /* as SDL reads it (and as vita-agent-use's input reaches it) */
        return;
    }
    now = pad.buttons;
    if ((now & SCE_CTRL_SELECT) && (now & SCE_CTRL_DOWN) && (now & (SCE_CTRL_L1 | SCE_CTRL_LTRIGGER))) {
        held = now;     /* the debug menu's combination: no picture, burst or overlay switch */
        return;
    }
    if (dbg_menu_open()) {
        now &= ~(uint32_t)(SCE_CTRL_UP | SCE_CTRL_DOWN);    /* its lines' moves: no burst, no overlay switch */
    }
    /* a combination when both its buttons are down and were not both before: in either order */
#define COMBO(k) ((now & SCE_CTRL_SELECT) && (now & (k)) && !((held & SCE_CTRL_SELECT) && (held & (k))))
    /* through the Ext2 call the handheld's shoulder buttons are L1 and R1 (LTRIGGER and RTRIGGER being L2 and R2):
       either, as vita-agent-use's injected presses set the triggers */
    if (COMBO(SCE_CTRL_LTRIGGER) || COMBO(SCE_CTRL_L1)) {
        requests |= RD_VGL_SHOT;
    }
    if (COMBO(SCE_CTRL_RTRIGGER) || COMBO(SCE_CTRL_R1)) {
        requests |= RD_VGL_DUMP | RD_VGL_SHOT;
        host_log("vgl: SELECT + R: the next frame's batches and pictures");
    }
    if (COMBO(SCE_CTRL_DOWN)) {
        requests |= RD_VGL_BURST;
    }
    if (COMBO(SCE_CTRL_UP)) {
        overlay_on = !overlay_on;
    }
#undef COMBO
    held = now;
}

void rd_vgl_present(void) {
    VFRAME* v = vf[fill];
    RD_FRAME* f = v->f;
    uint64_t t0 = now_us();
    int i;
    sceKernelWaitSema(sem_free, 1, NULL);   /* the render thread has read the frame before */
    game_wait_us += now_us() - t0;
    if (trace != NULL && TRACE_ON(presented + 1)) {
        trace[TRACE_SLOT(presented + 1)].game_wait = (uint32_t)(now_us() - t0);
    }
    debug_keys();
    if (soft_pairs < 0) {
        const char* e = getenv("MVC2_SHOT_SOFT");
        soft_pairs = e != NULL && atoi(e) != 0;
    }
    v->shot = 0;
    if (soft_pairs > 0 && listed("MVC2_SHOT", presented + 1) && !soft_busy) {
        __sync_fetch_and_or(&requests, RD_VGL_SHOT);    /* a listed frame: rd_soft's picture of it too */
    }
    if (soft_pairs > 0 && (requests & RD_VGL_SHOT) && !soft_busy) {
        uint64_t s0 = now_us();
        __sync_fetch_and_and(&requests, ~RD_VGL_SHOT);
        if (soft_px == NULL) {
            soft_px = malloc(RD_WIDTH * RD_HEIGHT * 4);
        }
        if (soft_px != NULL) {
            rd_end_frame(soft_px, RD_WIDTH);    /* the textures, palette and fog as the GPU will see them */
            soft_no = presented + 1;
            soft_busy = 1;
            v->shot = 1;
            host_log("vgl: rd_soft drew frame %u in %.1f ms for its picture", soft_no, (now_us() - s0) / 1000.0);
            sceKernelSignalSema(sem_shot, 1);
        }
    }
    /* frozen with the frame: the palette, the fog, and each texture as it is now */
    memcpy(v->palette, rd_palette, sizeof v->palette);
    v->palette_serial = rd_palette_serial;
    v->fog = rd_fog;
    v->nsnaps = 0;
    for (i = 0; i < f->nstates; i++) {
        RD_STATE* s = &f->states[i];
        int k;
        if (s->tex == NULL) {
            continue;
        }
        for (k = v->nsnaps - 1; k >= 0 && v->snaps[k].orig != s->tex; k--) {
        }
        if (k < 0) {
            if (v->nsnaps == MAX_SNAPS) {
                static int told;
                if (told++ < 4) {
                    host_log("vgl: frame %u has more than %d textures: the rest drawn untextured", presented + 1,
                             MAX_SNAPS);
                }
                s->tex = NULL;
                continue;
            }
            k = v->nsnaps++;
            v->snaps[k].t = *s->tex;
            v->snaps[k].orig = s->tex;
        }
        s->tex = &v->snaps[k].t;
    }
    v->frame_no = ++presented;
    v->handed = now_us();
    /* a frame with a translucent list worth it: its depths and translucent steps begun now on the cutting thread
       (idle: the render thread's prepare of the frame before is done, it only draws that one now) */
    v->early = cut_go_ready() && trans_away != 0 && f->ntris[RD_TRANS] >= 256;
    if (v->early) {
        cut_run(5, f, 0, 0);
    }
    sceKernelSignalSema(sem_ready, 1);
    fill ^= 1;
    rd_frame = vf[fill]->f;
}
