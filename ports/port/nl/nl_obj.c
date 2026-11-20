/*
 * nl_obj.c  The NAOMI library's models (libnlobj: nlObjPut and its texture loads)
 *
 * A model: a 6-word header (word 0: draw it; word 1 bit 1: no lighting), then groups, a zero word last.
 * A group: the polygon header, a bounding sphere, texture number (32), lighting byte (36: -1 flat), ambient
 * (40), face and offset ARGB floats (44, 60), primitives' length (76), primitives (80). A primitive: a
 * header word (bit 3 triangles, else a strip; bit 6 Gouraud; bits 0-1 culling; bit 7 a strip going on), a
 * count, then 32-byte vertices (bit 0 of x set) or 8-byte ones pointing back to a position's record.
 * Drawn as nlobjasm.src does, with triangles cut at the near plane.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "naomi_host.h"
#include "nl_port.h"
#include "prof.h"

typedef struct {
    float x, y, z;
} NLV3;

/* nlproj.c's projection, in ftrv's order (nl_set_cnv_proj_pers) */
extern float nl_cnv_proj_pers[16];

static NLV3 light = { 1.0f, 0.0f, 0.0f };
static float global_ambience = 0.3f;
static int trnsl_blend = 0x68;      /* nlObjPutSetTrnslMode's form: SRC_ALPHA, INV_SRC_ALPHA */

/* nlproj.c's NLOBJPUTPARAM: libnlobj cuts at -znear (0x0c) against the matrix's fourth row, -w */
extern uint8_t nl_objput_param[];

static float near_w = 1e-3f;
#define MAX_VTX 4096

/* a vertex after the transform, before the divide */
typedef struct {
    float w, x, y;
    float u, v;
    float i;            /* the lighting, 1 when unlit */
    uint32_t col;       /* the record's ARGB, in a group of vertex colours */
    uint32_t argb;      /* as drawn */
} OVTX;

/* one being cut at the near plane, its colour as floats (ARGB, 0 to 1) */
typedef struct {
    float w, x, y;
    float u, v;
    float c[4];
} FVTX;

static OVTX ov[MAX_VTX];
static RD_VTX sv[MAX_VTX];          /* for those in front of the near plane */
static const uint8_t* rec[MAX_VTX];
static uint8_t cached[MAX_VTX];     /* sv and w from the group's cache */
static int obj_log;

/* a group's screen vertices by their records' places (8-byte steps from the group's first
   primitive), so that a back-reference takes its record's vertex */
#define CACHE_SLOTS 16384
#define CACHE_VTX 4096
typedef struct {
    RD_VTX s;
    float w;
} CVTX;
static uint32_t slot[CACHE_SLOTS];  /* the group's number above, the vertex below */
static CVTX cache[CACHE_VTX];
static const uint8_t* cache_base;
static uint32_t cache_group;
static int ncache;

/* the SH-4's data is word aligned */
static uint32_t word(const uint8_t* p) {
    uint32_t w;
    memcpy(&w, __builtin_assume_aligned(p, 4), 4);
    return w;
}

static float flt(const uint8_t* p) {
    float f;
    memcpy(&f, __builtin_assume_aligned(p, 4), 4);
    return f;
}

/* d = a x b, 4x4 by rows, d neither of them; a row's four sums made side by side */
static __attribute__((section(".text.nlhot"))) void mul(float* restrict d, const float* restrict a, const float* restrict b) {
    int r;
#pragma GCC unroll 4
    for (r = 0; r < 4; r++) {
        const float* x = a + r * 4;
        float c0 = x[0] * b[0], c1 = x[0] * b[1], c2 = x[0] * b[2], c3 = x[0] * b[3];
        c0 += x[1] * b[4];
        c1 += x[1] * b[5];
        c2 += x[1] * b[6];
        c3 += x[1] * b[7];
        c0 += x[2] * b[8];
        c1 += x[2] * b[9];
        c2 += x[2] * b[10];
        c3 += x[2] * b[11];
        c0 += x[3] * b[12];
        c1 += x[3] * b[13];
        c2 += x[3] * b[14];
        c3 += x[3] * b[15];
        d[r * 4] = c0;
        d[r * 4 + 1] = c1;
        d[r * 4 + 2] = c2;
        d[r * 4 + 3] = c3;
    }
}

/* the PowerVR2 state of a polygon header */

static const RD_TEXTURE* group_texture(uint32_t pcw, uint32_t tcw, int texno) {
    uint32_t index = tcw & 0x1FFFFF;
    if (!(pcw & 8) || texno < 0 || index >= NL_MAX_TEX || !nl_tex[index].loaded) {
        return NULL;
    }
    return &nl_tex[index].tex;
}

static void pvr_state(RD_STATE* s, uint32_t pcw, uint32_t isp, uint32_t tsp, uint32_t tcw, const RD_TEXTURE* tex) {
    int list = pcw >> 24 & 7;
    uint32_t* w = (uint32_t*)(void*)s;
    size_t k;
    _Static_assert(sizeof(RD_STATE) % 4 == 0, "zeroed in words");
    for (k = 0; k < sizeof *s / 4; k++) {
        w[k] = 0;   /* its padding too: states are compared in words */
    }
    s->list = list == 2 ? RD_TRANS : list == 4 ? RD_PUNCH : RD_OPAQUE;
    s->depth = (uint8_t)(isp >> 29);
    s->zwrite = !(isp >> 26 & 1);
    s->src = (uint8_t)(tsp >> 29);
    s->dst = (uint8_t)(tsp >> 26 & 7);
    s->use_alpha = tsp >> 20 & 1;
    s->tex_alpha = !(tsp >> 19 & 1);
    s->fog = tsp >> 22 & 3;
    s->bilinear = (tsp >> 13 & 3) != 0;
    s->shading = (uint8_t)(tsp >> 6 & 3);
    s->uv_mode = (uint8_t)(tsp >> 15 & 15);
    s->mip_d = (uint8_t)(tsp >> 8 & 15);
    s->offset = pcw >> 2 & 1;
    s->tex = tex;
    if (tex != NULL) {
        int fmt = tcw >> 27 & 7;
        s->palette = (uint16_t)(fmt == 5 ? (tcw >> 21 & 63) * 16 : fmt == 6 ? (tcw >> 25 & 3) * 256 : 0);
    }
}

/* what a vertex's transform and colour take from its group */
typedef struct {
    float m[16];        /* model to screen, ftrv's order */
    NLV3 l;             /* the light, in the model's space */
    int lit;
    int vtxcol;         /* the vertices carry their colours */
    float ambient;
    float face[4];      /* ARGB, 0 to 1 */
    int face_one;       /* all of face 1: a vertex's colour is drawn as it is */
    uint32_t face_argb; /* the face as drawn */
} XFORM;

typedef struct {
    XFORM xf;
    RD_STATE state;
} GROUP;

static void cache_start(const uint8_t* base) {
    cache_base = base;
    ncache = 0;
    if (++cache_group == 0x10000) {
        memset(slot, 0, sizeof slot);
        cache_group = 1;
    }
}

/* the slot of the record at r, or -1 */
static int cache_slot(const uint8_t* r) {
    uintptr_t off = (uintptr_t)r - (uintptr_t)cache_base;
    return off & 7 || off >> 3 >= CACHE_SLOTS ? -1 : (int)(off >> 3);
}

static const CVTX* cache_find(const uint8_t* r) {
    int k = cache_slot(r);
    return k < 0 || slot[k] >> 16 != cache_group ? NULL : &cache[slot[k] & 0xFFFF];
}

static void cache_put(const uint8_t* r, const RD_VTX* s, float w) {
    int k = cache_slot(r);
    if (k >= 0 && ncache < CACHE_VTX) {
        cache[ncache].s = *s;
        cache[ncache].w = w;
        slot[k] = cache_group << 16 | ncache++;
    }
}

/* the record at *pp: its position's record, and *pp moved past it */
static const uint8_t* record(const uint8_t** pp) {
    const uint8_t* p = *pp;
    if (word(p) & 1) {
        *pp = p + 32;
        return p;
    }
    *pp = p + 8;
    return p + 8 + (int32_t)word(p + 4);
}

/* a vertex's colour, ARGB 0 to 1 */
static inline __attribute__((always_inline)) void colour(const XFORM* g, const OVTX* o, float* c) {
    if (g->vtxcol) {
        c[0] = g->face[0] * (o->col >> 24) / 255.0f;
        c[1] = g->face[1] * (o->col >> 16 & 255) / 255.0f;
        c[2] = g->face[2] * (o->col >> 8 & 255) / 255.0f;
        c[3] = g->face[3] * (o->col & 255) / 255.0f;
    } else {
        c[0] = g->face[0];
        c[1] = g->face[1] * o->i;
        c[2] = g->face[2] * o->i;
        c[3] = g->face[3] * o->i;
    }
}

static inline __attribute__((always_inline)) void transform(const XFORM* g, const uint8_t* r, OVTX* o) {
    float x = flt(r), y = flt(r + 4), z = flt(r + 8);
    const float* m = g->m;
    o->w = x * m[0] + y * m[4] + z * m[8] + m[12];
    o->x = x * m[1] + y * m[5] + z * m[9] + m[13];
    o->y = x * m[2] + y * m[6] + z * m[10] + m[14];
    o->u = flt(r + 24);
    o->v = flt(r + 28);
    if (g->vtxcol) {
        o->col = word(r + 16);
        if (g->face_one) {
            o->argb = o->col;       /* a byte over 255, and back, is the byte */
        } else {
            float c[4];
            colour(g, o, c);
            o->argb = nl_float_argb(c[0], c[1], c[2], c[3]);
        }
    } else if (g->lit) {
        float d = flt(r + 12) * g->l.x + flt(r + 16) * g->l.y + flt(r + 20) * g->l.z;
        float i = g->ambient + (d > 0.0f ? d : 0.0f);
        o->i = i;
        o->argb = (g->face_argb & 0xFF000000u) | nl_float_byte(g->face[1] * i) << 16 |
                  nl_float_byte(g->face[2] * i) << 8 | nl_float_byte(g->face[3] * i);
    } else {
        o->i = 1.0f;
        o->argb = g->face_argb;
    }
}

static void screen(const OVTX* o, RD_VTX* d) {
    float rw = 1.0f / o->w;
    d->x = o->x * rw;
    d->y = o->y * rw;
    d->z = rw;
    d->u = o->u;
    d->v = o->v;
    d->base = o->argb;
    d->offset = 0;
}

static FVTX with_colour(const GROUP* g, const OVTX* o) {
    FVTX f;
    f.w = o->w;
    f.x = o->x;
    f.y = o->y;
    f.u = o->u;
    f.v = o->v;
    colour(&g->xf, o, f.c);
    return f;
}

static void screen_cut(const FVTX* o, RD_VTX* d) {
    float rw = 1.0f / o->w;
    d->x = o->x * rw;
    d->y = o->y * rw;
    d->z = rw;
    d->u = o->u;
    d->v = o->v;
    d->base = nl_float_argb(o->c[0], o->c[1], o->c[2], o->c[3]);
    d->offset = 0;
}

static FVTX lerp(const FVTX* a, const FVTX* b) {
    float t = (near_w - a->w) / (b->w - a->w);
    FVTX o;
    int k;
    o.w = near_w;
    o.x = a->x + (b->x - a->x) * t;
    o.y = a->y + (b->y - a->y) * t;
    o.u = a->u + (b->u - a->u) * t;
    o.v = a->v + (b->v - a->v) * t;
    for (k = 0; k < 4; k++) {
        o.c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
    }
    return o;
}

/* one triangle, cut at the near plane (w = znear) */
static void triangle(const GROUP* g, const OVTX* a, const OVTX* b, const OVTX* c) {
    if (a->w >= near_w && b->w >= near_w && c->w >= near_w) {
        RD_VTX v[3];
        screen(a, &v[0]);
        screen(b, &v[1]);
        screen(c, &v[2]);
        rd_strip(&g->state, v, 3);
        return;
    }
    FVTX in[3] = { with_colour(g, a), with_colour(g, b), with_colour(g, c) };
    FVTX poly[4];
    RD_VTX v[4];
    int n = 0, k;
    for (k = 0; k < 3; k++) {
        const FVTX *p = &in[k], *q = &in[(k + 1) % 3];
        if (p->w >= near_w) {
            poly[n++] = *p;
        }
        if ((p->w >= near_w) != (q->w >= near_w)) {
            poly[n++] = lerp(p, q);
        }
    }
    if (n < 3) {
        return;
    }
    for (k = 0; k < n; k++) {
        screen_cut(&poly[k], &v[k]);
    }
    if (n == 3) {
        rd_strip(&g->state, v, 3);
    } else {
        RD_VTX s[4] = { v[0], v[1], v[3], v[2] };
        rd_strip(&g->state, s, 4);
    }
}

/* a primitive at p (its header word read): the data after it */
static __attribute__((noinline)) const uint8_t* primitive(GROUP* g, const XFORM* x, uint32_t head, const uint8_t* p) {
    int n = (int)word(p), k, front = 1;
    p += 4;
    g->state.cull = (uint8_t)(head & 3);
    g->state.flat = !(head & 64);
    if (head & 8) {
        while (n-- > 0) {
            for (k = 0; k < 3; k++) {
                transform(x, record(&p), &ov[k]);
            }
            triangle(g, &ov[0], &ov[1], &ov[2]);
        }
        return p;
    }
    if (n > MAX_VTX) {
        n = MAX_VTX;
    }
    for (k = 0; k < n; k++) {
        int full = word(p) & 1;
        const CVTX* c;
        rec[k] = record(&p);
        c = full ? NULL : cache_find(rec[k]);
        cached[k] = c != NULL;
        if (c != NULL) {
            sv[k] = c->s;
            ov[k].w = c->w;
        } else {
            transform(x, rec[k], &ov[k]);
        }
        front &= ov[k].w >= near_w;
    }
    if (front && n >= 3) {
        /* all in front: each vertex to the screen once */
        for (k = 0; k < n; k++) {
            if (!cached[k]) {
                screen(&ov[k], &sv[k]);
                cache_put(rec[k], &sv[k], ov[k].w);
            }
        }
        rd_strip(&g->state, sv, n);
        return p;
    }
    for (k = 0; k < n; k++) {
        if (cached[k]) {
            transform(x, rec[k], &ov[k]);
        }
    }
    for (k = 2; k < n; k++) {
        if (k & 1) {
            triangle(g, &ov[k - 1], &ov[k - 2], &ov[k]);
        } else {
            triangle(g, &ov[k - 2], &ov[k - 1], &ov[k]);
        }
    }
    return p;
}

/* nlproj.c's window and focus */
extern float nl_focusX2, nl_focusY2;
extern float nl_projectMatrixLeft, nl_projectMatrixTop, nl_projectMatrixWidth, nl_projectMatrixHeight;

float host_wide_x;

/* how much wider a scene's model must be to reach the sides of a screen wider than 4:3 (a few models made only
   for the 4:3's view, the title's water): the half widths' ratio and a little more; 1 on 4:3 */
float host_wide_scale(void) {
    return host_wide_x != 0.0f ? (320.0f + host_wide_x) / 320.0f * 1.02f : 1.0f;
}

/* the fight's gauges (disp.c's front_disp, list 11) moved out to the screen's corners when it shows more than the
   game's 640 (ASPECT_RATIO=WIDE): each model by where its origin lands, left of the middle by host_wide_x to the
   left, right of it to the right, the middle's (the timer, the fight's words) left where they are */
static int hud_on, hud_log_frame = -1;
static int hud_groups;          /* a gauge whose origin is in the middle: each group moved by its own place */
static unsigned hud_frame;      /* the game's frame (+1) whose gauges were drawn last */
static float hud_dz = 24.0f;    /* MVC2_HUD_DZ: the middle's half width, in the game's pixels */
static float hud_dz2 = 64.0f;   /* MVC2_HUD_DZ2: the same for a middle model's groups (the clock's stay) */

static int hud_logging(void) {
    return hud_log_frame >= 0 && host_frame() >= (unsigned)hud_log_frame && host_frame() < (unsigned)hud_log_frame + 2;
}

void host_hud(int on) {
    static int read;
    if (!read) {
        read = 1;
        if (getenv("MVC2_HUD_DZ") != NULL) {
            hud_dz = (float)atof(getenv("MVC2_HUD_DZ"));
        }
        if (getenv("MVC2_HUD_DZ2") != NULL) {
            hud_dz2 = (float)atof(getenv("MVC2_HUD_DZ2"));
        }
        if (getenv("MVC2_HUDLOG") != NULL) {
            hud_log_frame = atoi(getenv("MVC2_HUDLOG"));
        }
    }
    hud_on = on && host_wide_x != 0.0f;
    hud_groups = 0;
    if (hud_on) {
        hud_frame = host_frame() + 1;
    }
}

/* the fight's lines of text on the gauges' row (the score, the wins, PRESS START): moved as the gauges are, by
   where the line's middle is, in a frame whose gauges were drawn; else 0 */
float host_text_dx(float x0, float x1) {
    float mid = (x0 + x1) * 0.5f;
    if (host_wide_x == 0.0f || hud_frame != host_frame() + 1) {
        return 0.0f;
    }
    return mid < 320.0f - hud_dz ? -host_wide_x : mid > 320.0f + hud_dz ? host_wide_x : 0.0f;
}

/* to_screen moved by the gauge's side: its x column takes dx times its w column, so every vertex lands dx further */
static void hud_shift(const int* obj, float* to_screen) {
    const float* m = nl_xmtrx();
    float w0 = 0.0f, x0 = 0.0f, sx, dx;
    int i;
    for (i = 0; i < 4; i++) {
        w0 += m[12 + i] * to_screen[i * 4];
        x0 += m[12 + i] * to_screen[i * 4 + 1];
    }
    sx = w0 != 0.0f ? x0 / w0 : 320.0f;
    dx = sx < 320.0f - hud_dz ? -host_wide_x : sx > 320.0f + hud_dz ? host_wide_x : 0.0f;
    if (hud_logging()) {
        host_log("hud: frame %u obj %p origin x %.1f: moved %.1f", host_frame(), (const void*)obj, (double)sx, (double)dx);
    }
    /* one in the middle (the clock, or both sides' hyper gauges in one model): its groups each by their own place */
    hud_groups = dx == 0.0f;
    if (dx != 0.0f) {
        for (i = 0; i < 4; i++) {
            to_screen[i * 4 + 1] += dx * to_screen[i * 4];
        }
    }
}

enum { SPHERE_OUT, SPHERE_FRONT, SPHERE_CUT };

/* nlobjasm's test of a group's bounding sphere (s: centre, radius): out of sight, wholly in front of the
   near limit, or across it. *limit is the limit's float bits (-znear, against the fourth row, -w) with the
   last group's cut flag in bit 0, one register in Sega's code. */
static int sphere(const float* m, const uint8_t* s, float scale, uint32_t* limit) {
    float x = flt(s), y = flt(s + 4), z = flt(s + 8), r = flt(s + 12) * scale;
    float w = x * m[0] + y * m[4] + z * m[8] + m[12];
    float sx = x * m[1] + y * m[5] + z * m[9] + m[13];
    float sy = x * m[2] + y * m[6] + z * m[10] + m[14];
    float t = x * m[3] + y * m[7] + z * m[11] + m[15];
    float zn, rx, ry, dx, dy;
    int seen = SPHERE_FRONT;
    memcpy(&zn, limit, 4);
    *limit &= ~1u;
    t += r;
    if (t > zn) {
        t -= r;
        t -= r;
        *limit |= 1;
        if (t > zn) {
            return SPHERE_OUT;
        }
        seen = SPHERE_CUT;
    }
    rx = nl_focusX2 * -r;
    ry = nl_focusY2 * -r;
    /* the window widened by what the screen shows past the game's sides (ASPECT_RATIO=WIDE) */
    dx = sx - (nl_projectMatrixLeft - host_wide_x) * w;
    dy = sy - nl_projectMatrixTop * w;
    if (!(dx > rx) || !(dy > ry) || dx > (nl_projectMatrixWidth + 2.0f * host_wide_x) * w - rx ||
        dy > nl_projectMatrixHeight * w - ry) {
        return SPHERE_OUT;
    }
    return seen;
}

/* the light in the model's space: the transpose of the model matrix's turn, over its scale */
static NLV3 model_light(const float* m, float inv_scale) {
    NLV3 l;
    l.x = light.x * m[0] + light.y * m[1] + light.z * m[2];
    l.y = light.x * m[4] + light.y * m[5] + light.z * m[6];
    l.z = light.x * m[8] + light.y * m[9] + light.z * m[10];
    l.x *= inv_scale;
    l.y *= inv_scale;
    l.z *= inv_scale;
    return l;
}

/* models on the GPU (rd_gpu_tl): a group's records as a mesh, kept by the group's address and made when they hash
   the same in two frames. Each draw hashes the group's bounding sphere and a 64th of the records, in turn, the
   whole pass checked against the records the mesh was made from: a new sphere makes it again, other records mark
   the group as one the game moves, drawn here from then on. */
#define MESH_SLOTS 1024
#define MESH_VTX 2048
#define MESH_TRI 2048
#define MESH_SPANS 64
#define MESH_RECS 8192      /* a group's records' bytes over 8 */

/* a mesh's triangles' planes, for one that may be drawn in two passes */
typedef struct {
    uint16_t ntri;
    uint16_t seen[2];       /* the triangles last found facing each way */
    float plane[][4];
} PLANES;

/* two cache lines: a draw finds and checks its mesh in the first */
typedef struct __attribute__((aligned(64))) {
    const uint8_t* group;   /* NULL: a free slot */
    uint32_t len;           /* the records' bytes */
    uint32_t used;
    int16_t id;             /* the renderer's, or -1: drawn here */
    uint8_t kind;
    uint8_t turn;           /* the slice hashed next */
    uint32_t retry;         /* with no mesh: the frame to try again from, or 0 for never */
    uint32_t head;          /* the bounding sphere's hash */
    uint32_t sum, run;      /* the records' hash, and the pass's so far */
    float lo[3], hi[3];
    PLANES* planes;         /* NULL for one drawn in one pass */
} MESH;

_Static_assert(sizeof(MESH) == 64, "a mesh slot is two cache lines");

/* a mesh as it is made */
typedef struct {
    NL_MVTX v[MESH_VTX];
    uint16_t idx[MESH_TRI * 3];
    NL_MSPAN span[MESH_SPANS];
    uint16_t map[MESH_RECS];    /* a record's shared vertex + 1, by its 8-byte place */
} MAKING;

static MESH* meshes;        /* both made when first wanted; meshes on a 64-byte boundary in meshes_at */
static void* meshes_at;
static MAKING* making;
static int nmeshes;
static uint32_t mesh_full;  /* the frame the renderer last had no room: no more tries until the next */
static int gpu_put;         /* the put's record, -1 none yet, -2 none to be had */

/* in four lanes of words, so the multiplies overlap */
static __attribute__((hot, section(".text.nlhot"))) uint32_t mesh_hash(const uint8_t* p, uint32_t n) {
    uint32_t a = 2166136261u ^ n, b = 0x9E3779B9u, c = 0x85EBCA6Bu, d = 0xC2B2AE35u, k;
    for (k = 0; k + 16 <= n; k += 16) {
        a = (a ^ word(p + k)) * 0x9E3779B1u;
        b = (b ^ word(p + k + 4)) * 0x85EBCA77u;
        c = (c ^ word(p + k + 8)) * 0xC2B2AE3Du;
        d = (d ^ word(p + k + 12)) * 0x27D4EB2Fu;
    }
    for (; k < n; k += 4) {
        a = (a ^ word(p + k)) * 0x9E3779B1u;
    }
    return a ^ (b << 7 | b >> 25) ^ (c << 13 | c >> 19) ^ (d << 21 | d >> 11);
}

/* MVC2_MESH_CHECK=n: a made mesh's records hashed again an nth at a time, one a draw (0: never) */
static int mesh_check = -1;

/* an nth of the records, in whole 32-byte steps */
static inline __attribute__((always_inline)) uint32_t mesh_slice(uint32_t len) {
    uint32_t c = mesh_check > 0 ? (uint32_t)mesh_check : 64;
    uint32_t n = (((c & (c - 1)) == 0 ? len >> __builtin_ctz(c) : len / c) + 31) & ~31u;
    return n != 0 ? n : 32;
}

static uint32_t mesh_fold(uint32_t run, const uint8_t* p, uint32_t len, uint32_t off, uint32_t n) {
    return (run + mesh_hash(p + 80 + off, off + n < len ? n : len - off)) * 0x2545F491u;
}

/* the records' hash, a slice at a time as the draws take them */
static uint32_t mesh_sum(const uint8_t* p, uint32_t len) {
    uint32_t n = mesh_slice(len), off, run = len;
    for (off = 0; off < len; off += n) {
        run = mesh_fold(run, p, len, off, n);
    }
    return run;
}

static void mesh_bounds(NL_MESH* m, const NL_MVTX* v) {
    float p[3] = { v->x, v->y, v->z };
    int k;
    for (k = 0; k < 3; k++) {
        if (p[k] < m->lo[k]) {
            m->lo[k] = p[k];
        }
        if (p[k] > m->hi[k]) {
            m->hi[k] = p[k];
        }
    }
}

/* a vertex from the record at r, with c's normal or colour */
static int mesh_vertex(NL_MESH* m, const uint8_t* r, const uint8_t* c) {
    NL_MVTX* v;
    if (m->nv == MESH_VTX) {
        return -1;
    }
    v = &making->v[m->nv];
    memset(v, 0, sizeof *v);
    v->x = flt(r);
    v->y = flt(r + 4);
    v->z = flt(r + 8);
    v->u = flt(r + 24);
    v->v = flt(r + 28);
    if (m->kind == RD_GPU_LIT) {
        v->nx = flt(c + 12);
        v->ny = flt(c + 16);
        v->nz = flt(c + 20);
    } else if (m->kind == RD_GPU_VTXCOL) {
        v->argb = word(c + 16);
    }
    mesh_bounds(m, v);
    return m->nv++;
}

/* a shared vertex: the one already made from r, or a new one */
static int mesh_shared(NL_MESH* m, const uint8_t* prim, const uint8_t* r) {
    uint16_t* s = &making->map[(r - prim) >> 3];
    int i;
    if (*s != 0) {
        return *s - 1;
    }
    if ((i = mesh_vertex(m, r, r)) >= 0) {
        *s = (uint16_t)(i + 1);
    }
    return i;
}

static int mesh_triangle(NL_MESH* m, const uint8_t* prim, const uint8_t* a, const uint8_t* b, const uint8_t* c,
                         int flat) {
    uint16_t* t = &making->idx[m->ntri * 3];
    int i0, i1, i2;
    if (m->ntri == MESH_TRI) {
        return 0;
    }
    if (flat) {
        i0 = mesh_vertex(m, a, c);
        i1 = mesh_vertex(m, b, c);
        i2 = mesh_vertex(m, c, c);
    } else {
        i0 = mesh_shared(m, prim, a);
        i1 = mesh_shared(m, prim, b);
        i2 = mesh_shared(m, prim, c);
    }
    if (i0 < 0 || i1 < 0 || i2 < 0) {
        return 0;
    }
    t[0] = (uint16_t)i0;
    t[1] = (uint16_t)i1;
    t[2] = (uint16_t)i2;
    making->span[m->nspan - 1].count++;
    m->ntri++;
    return 1;
}

/* the group at p as a mesh: 0 if it can't be one (env-mapped, a record outside it, too big) */
static __attribute__((noinline, cold)) int mesh_build(NL_MESH* m, const uint8_t* p, int kind) {
    const uint8_t *prim = p + 80, *end = prim + word(p + 76), *q = prim;
    NL_MSPAN* span = making->span;
    int k;
    if (end - prim > MESH_RECS * 8) {
        return 0;
    }
    memset(making->map, 0, (size_t)((end - prim) >> 3) * sizeof making->map[0]);
    memset(m, 0, sizeof *m);
    m->kind = kind;
    m->v = making->v;
    m->idx = making->idx;
    m->span = span;
    for (k = 0; k < 3; k++) {
        m->lo[k] = INFINITY;
        m->hi[k] = -INFINITY;
    }
    while (q < end) {
        uint32_t head = word(q);
        int n, flat;
        if ((int32_t)head <= 0) {
            break;
        }
        if (head & 0x100) {
            return 0;
        }
        n = (int)word(q + 4) * (head & 8 ? 3 : 1);
        q += 8;
        if (n < 0 || n > MAX_VTX) {
            return 0;
        }
        for (k = 0; k < n; k++) {
            if (q + 8 > end) {
                return 0;
            }
            rec[k] = record(&q);
            if (rec[k] < prim || rec[k] + 32 > end || (rec[k] - prim) & 7) {
                return 0;
            }
        }
        if (q > end) {
            return 0;
        }
        flat = !(head & 64) && kind != RD_GPU_UNLIT;
        if (m->nspan == 0 || span[m->nspan - 1].cull != (head & 3) || span[m->nspan - 1].flat != flat) {
            if (m->nspan == MESH_SPANS) {
                return 0;
            }
            span[m->nspan].first = (uint16_t)m->ntri;
            span[m->nspan].count = 0;
            span[m->nspan].cull = (uint8_t)(head & 3);
            span[m->nspan].flat = (uint8_t)flat;
            m->nspan++;
        }
        if (head & 8) {
            for (k = 0; k + 2 < n; k += 3) {
                if (!mesh_triangle(m, prim, rec[k], rec[k + 1], rec[k + 2], flat)) {
                    return 0;
                }
            }
        } else {
            for (k = 2; k < n; k++) {
                const uint8_t *a = k & 1 ? rec[k - 1] : rec[k - 2], *b = k & 1 ? rec[k - 2] : rec[k - 1];
                if (!mesh_triangle(m, prim, a, b, rec[k], flat)) {
                    return 0;
                }
            }
        }
    }
    return m->ntri > 0;
}

/* a translucent mesh with none of its triangles culled, and not flat, kept with its triangles' planes so that a
   deep one can be drawn in two passes, its back faces then its front: right for a convex shell. 0 for any other. */
static __attribute__((noinline, cold)) int mesh_planes(MESH* e, const NL_MESH* m) {
    PLANES* q;
    float *f = NULL, big = 0.0f, size = 0.0f;
    int k, flat = 1;
    for (k = 0; k < m->nspan; k++) {
        if (m->span[k].cull > 1) {
            return 0;
        }
    }
    if ((q = host_try_malloc(sizeof *q + (size_t)m->ntri * sizeof q->plane[0])) == NULL) {
        return 0;
    }
    for (k = 0; k < m->ntri; k++) {
        const NL_MVTX *a = &m->v[m->idx[3 * k]], *b = &m->v[m->idx[3 * k + 1]], *c = &m->v[m->idx[3 * k + 2]];
        float ux = b->x - a->x, uy = b->y - a->y, uz = b->z - a->z;
        float vx = c->x - a->x, vy = c->y - a->y, vz = c->z - a->z;
        float* p = q->plane[k];
        p[0] = uy * vz - uz * vy;
        p[1] = uz * vx - ux * vz;
        p[2] = ux * vy - uy * vx;
        p[3] = -(p[0] * a->x + p[1] * a->y + p[2] * a->z);
        if (p[0] * p[0] + p[1] * p[1] + p[2] * p[2] > big) {
            big = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
            f = p;
        }
    }
    for (k = 0; k < 3; k++) {
        size = fmaxf(size, m->hi[k] - m->lo[k]);
    }
    /* flat: every vertex within a thousandth of the mesh's size of its largest triangle's plane */
    for (k = 0; f != NULL && flat && k < m->nv; k++) {
        flat = fabsf(f[0] * m->v[k].x + f[1] * m->v[k].y + f[2] * m->v[k].z + f[3]) <= 1e-3f * size * sqrtf(big);
    }
    if (flat) {
        free(q);
        return 0;
    }
    q->ntri = (uint16_t)m->ntri;
    q->seen[0] = q->seen[1] = 0;
    e->planes = q;
    return 1;
}

/* faces of q facing both ways as seen through m: the eye, in the model's space, is what m's three rows take to 0 */
static int mesh_both_ways(PLANES* q, const float* m) {
    float a0 = m[0], a1 = m[4], a2 = m[8], a3 = m[12];
    float b0 = m[1], b1 = m[5], b2 = m[9], b3 = m[13];
    float c0 = m[2], c1 = m[6], c2 = m[10], c3 = m[14];
    float ex = a1 * (b2 * c3 - b3 * c2) - a2 * (b1 * c3 - b3 * c1) + a3 * (b1 * c2 - b2 * c1);
    float ey = -(a0 * (b2 * c3 - b3 * c2) - a2 * (b0 * c3 - b3 * c0) + a3 * (b0 * c2 - b2 * c0));
    float ez = a0 * (b1 * c3 - b3 * c1) - a1 * (b0 * c3 - b3 * c0) + a3 * (b0 * c1 - b1 * c0);
    float ew = -(a0 * (b1 * c2 - b2 * c1) - a1 * (b0 * c2 - b2 * c0) + a2 * (b0 * c1 - b1 * c0));
    int k, found = 0;
    for (k = 0; k < 2; k++) {
        const float* p = q->plane[q->seen[k]];
        float s = p[0] * ex + p[1] * ey + p[2] * ez + p[3] * ew;
        found |= s > 0.0f ? 1 : s < 0.0f ? 2 : 0;
    }
    for (k = 0; found != 3 && k < q->ntri; k++) {
        const float* p = q->plane[k];
        float s = p[0] * ex + p[1] * ey + p[2] * ez + p[3] * ew;
        if (s > 0.0f && !(found & 1)) {
            found |= 1;
            q->seen[0] = (uint16_t)k;
        } else if (s < 0.0f && !(found & 2)) {
            found |= 2;
            q->seen[1] = (uint16_t)k;
        }
    }
    return found == 3;
}

/* e's mesh let go */
static __attribute__((noinline, cold)) void mesh_drop(MESH* e) {
    rd_gpu_mesh_free(e->id);
    free(e->planes);
    e->planes = NULL;
    e->id = -1;
}

/* every mesh let go, for a table grown full of groups no longer drawn */
static __attribute__((noinline, cold)) void mesh_clear(void) {
    int k;
    for (k = 0; k < MESH_SLOTS; k++) {
        if (meshes[k].group != NULL && meshes[k].id >= 0) {
            mesh_drop(&meshes[k]);
        }
    }
    memset(meshes, 0, MESH_SLOTS * sizeof *meshes);
    nmeshes = 0;
}

/* up to 8 of the least lately drawn meshes not drawn for two frames let go, their room the renderer's again once
   the frames drawing them are done */
static __attribute__((noinline, cold)) void mesh_evict(uint32_t f) {
    int n, k;
    for (n = 0; n < 8; n++) {
        MESH* old = NULL;
        for (k = 0; k < MESH_SLOTS; k++) {
            MESH* e = &meshes[k];
            if (e->group != NULL && e->id >= 0 && e->used + 2 < f && (old == NULL || e->used < old->used)) {
                old = e;
            }
        }
        if (old == NULL) {
            return;
        }
        mesh_drop(old);
        old->retry = f + 1;
    }
}

static uint32_t mesh_home(const uint8_t* p) {
    return (uint32_t)((uintptr_t)p >> 3) * 2654435761u >> 22;
}

static inline __attribute__((always_inline)) uint32_t mesh_slot(const uint8_t* p) {
    uint32_t h = mesh_home(p);
    while (meshes[h].group != NULL && meshes[h].group != p) {
        h = (h + 1) & (MESH_SLOTS - 1);
    }
    return h;
}

/* the mesh of the group at p, made or made again as needed: NULL to draw it here. A make the renderer has no
   room for is tried again in a later frame, never again in this one. sides: its planes kept, if it may be drawn in
   two passes. */
static MESH* mesh_find(const uint8_t* p, int kind, int sides) {
    uint32_t len = word(p + 76), head = mesh_hash(p + 16, 16), n = mesh_slice(len), off, f = host_frame();
    MESH* e = &meshes[mesh_slot(p)];
    NL_MESH m;
    int k;
    if (e->group == p && e->len == len && e->kind == kind && e->head == head) {
        if (e->id >= 0 && mesh_check == 0) {
            e->used = f;
            return e;
        }
        if (e->id >= 0) {
            off = e->turn * n;
            e->run = mesh_fold(e->run, p, len, off, n);
            e->turn++;
            if (off + n >= len) {
                if (e->run != e->sum) {
                    mesh_drop(e);
                    e->retry = 0;
                    return NULL;
                }
                e->turn = 0;
                e->run = len;
            }
            e->used = f;
            return e;
        }
        if (e->retry == 0 || f < e->retry || mesh_full == f) {
            return NULL;
        }
        if (mesh_sum(p, len) != e->sum) {
            e->retry = 0;
            return NULL;
        }
    } else {
        if (e->group == NULL) {
            if (nmeshes == MESH_SLOTS * 3 / 4) {
                mesh_clear();
                e = &meshes[mesh_slot(p)];
            }
            nmeshes++;
            e->group = p;
        } else if (e->id >= 0) {
            mesh_drop(e);
        }
        e->id = -1;
        e->len = len;
        e->kind = (uint8_t)kind;
        e->head = head;
        e->used = f;
        e->retry = f + 1;
        e->sum = mesh_sum(p, len);
        return NULL;
    }
    e->used = f;
    e->turn = 0;
    e->run = len;
    if (!mesh_build(&m, p, kind)) {
        e->retry = 0;
        return NULL;
    }
    if ((k = rd_gpu_mesh_make(&m)) < 0) {
        e->retry = f + 1;
        mesh_full = f;
        mesh_evict(f);
        return NULL;
    }
    e->id = (int16_t)k;
    for (k = 0; k < 3; k++) {
        e->lo[k] = m.lo[k];
        e->hi[k] = m.hi[k];
    }
    if (sides) {
        mesh_planes(e, &m);
    }
    return e;
}

/* the float's exponent not all ones */
static int finite_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return (u & 0x7F800000u) != 0x7F800000u;
}

/* MVC2_PROF_GROUPS=n in a profiling build: one group in n timed in its parts, a tick read each (taken off), its
   times taken n times */
#ifdef MVC2_PROF
static int prof_groups = -1, group_on, group_seq;
static void group_add(int zone, uint32_t a, uint32_t b) {
    uint32_t t = b - a;
    PROF_ADD(zone, (t > prof_read_ticks ? t - prof_read_ticks : 0) * (uint32_t)prof_groups);
}
#define GROUP_NOW() (group_on ? PROF_NOW() : 0u)
#define GROUP_ADD(zone, a, b) (group_on ? group_add((zone), (a), (b)) : (void)0)
#else
#define GROUP_NOW() 0u
#define GROUP_ADD(zone, a, b) ((void)(a), (void)(b))
#endif

/* the mesh table, made on first use: 0 if there is no room for it */
static __attribute__((noinline, cold)) int gpu_setup(void) {
    static int failed;
    if (mesh_check < 0) {
        const char* c = getenv("MVC2_MESH_CHECK");
        mesh_check = c == NULL ? 64 : atoi(c) < 0 ? 0 : atoi(c) > 255 ? 255 : atoi(c);
    }
    if (meshes == NULL) {
        if (failed || (meshes_at = host_try_calloc(MESH_SLOTS + 1, sizeof *meshes)) == NULL ||
            (making = host_try_malloc(sizeof *making)) == NULL) {
            free(meshes_at);
            meshes_at = NULL;
            failed = 1;
            return 0;
        }
        meshes = (MESH*)(((uintptr_t)meshes_at + sizeof *meshes - 1) & ~(uintptr_t)(sizeof *meshes - 1));
    }
    return 1;
}

/* the group at p for the GPU to draw: 0 to draw it here, as a lit one whose light isn't finite (a model scaled to 0) */
static int gpu_group(const uint8_t* p, const GROUP* g) {
    int kind = g->xf.vtxcol ? RD_GPU_VTXCOL : g->xf.lit ? RD_GPU_LIT : RD_GPU_UNLIT, k;
    const float* m = g->xf.m;
    MESH* e;
    RD_GPU_GROUP r;
    uint32_t t0 = GROUP_NOW(), t1;
    if ((mesh_check < 0 || meshes == NULL) && !gpu_setup()) {
        return 0;
    }
    if ((g->state.list == RD_TRANS && !rd_gpu_tl_trans) || gpu_put == -2 ||
        (kind == RD_GPU_LIT && !(finite_bits(g->xf.l.x) && finite_bits(g->xf.l.y) && finite_bits(g->xf.l.z))) ||
        (e = mesh_find(p, kind, rd_gpu_tl_two_pass && g->state.list == RD_TRANS)) == NULL) {
        return 0;
    }
    t1 = GROUP_NOW();
    GROUP_ADD(PZ_NLG_FIND, t0, t1);
    if (gpu_put == -1 && (gpu_put = rd_gpu_put(m, near_w)) < 0) {
        gpu_put = -2;
        return 0;
    }
    r.mesh = e->id;
    r.kind = kind;
    memcpy(r.face, g->xf.face, sizeof r.face);
    r.face_argb = g->xf.face_argb;
    r.l[0] = g->xf.l.x;
    r.l[1] = g->xf.l.y;
    r.l[2] = g->xf.l.z;
    r.ambient = g->xf.ambient;
    r.wlo = r.whi = r.key = m[12];
    for (k = 0; k < 3; k++) {
        float a = m[4 * k] * e->lo[k], b = m[4 * k] * e->hi[k];
        r.wlo += a < b ? a : b;
        r.whi += a < b ? b : a;
        r.key += m[4 * k] * (e->lo[k] + e->hi[k]) * 0.5f;
    }
    /* a deep one drawn in two passes only if it has faces facing each way: never a flat one */
    r.two_sided = e->planes != NULL && rd_gpu_tl_trans_deep > 0.0f &&
                  !(r.wlo > 0.0f && r.whi <= r.wlo * rd_gpu_tl_trans_deep) && mesh_both_ways(e->planes, m);
    /* the box centre's 1/w; for two passes, the middles of its far and near halves' */
    r.key_far = (r.key + r.whi) * 0.5f;
    r.key_near = (r.wlo + r.key) * 0.5f;
    r.key = g->state.list == RD_TRANS && r.key > near_w ? 1.0f / r.key : 0.0f;
    r.key_far = g->state.list == RD_TRANS && r.key_far > near_w ? 1.0f / r.key_far : 0.0f;
    r.key_near = g->state.list == RD_TRANS && r.key_near > near_w ? 1.0f / r.key_near : r.key_far;
    r.faces = 0;
    k = rd_gpu_group(gpu_put, &g->state, &r);
    GROUP_ADD(PZ_NLG_PUT, t1, GROUP_NOW());
    return k;
}

static void draw(const int* obj, const float* model, const float* toscreen, float trnsl, float scale_factor) {
    float scale = fabsf(scale_factor);
    const uint8_t* p = (const uint8_t*)obj + 24;
    uint32_t limit = word(nl_objput_param + 0x0c);
    GROUP g;
    XFORM x;    /* g.xf, where the strips' stores can't reach it */
    float hud_base[16];
    int groups = hud_on && hud_groups;
    mul(g.xf.m, model, toscreen);
    if (groups) {
        memcpy(hud_base, g.xf.m, sizeof hud_base);
    }
    g.xf.l = model_light(model, 1.0f / scale_factor);
    gpu_put = -1;
#ifdef MVC2_PROF
    if (prof_groups < 0) {
        prof_groups = getenv("MVC2_PROF_GROUPS") != NULL ? atoi(getenv("MVC2_PROF_GROUPS")) : 0;
        prof_groups = prof_groups > 0 ? prof_groups : 0;
    }
#endif
    while (word(p) != 0) {
        uint32_t pcw = word(p), isp = word(p + 4), tsp = word(p + 8), tcw = word(p + 12), t0 = 0;
        int texno = (int32_t)word(p + 32), mode = (int8_t)p[36], k;
        const uint8_t *prim = p + 80, *end = prim + word(p + 76);
#ifdef MVC2_PROF
        group_on = prof_groups > 0 && ++group_seq % prof_groups == 0;
        t0 = GROUP_NOW();
#endif
        if (groups) {
            /* the group's bounding sphere's centre on the screen picks its side */
            const float* b = hud_base;
            float x = flt(p + 16), y = flt(p + 20), z = flt(p + 24);
            float w = x * b[0] + y * b[4] + z * b[8] + b[12];
            float sx = w != 0.0f ? (x * b[1] + y * b[5] + z * b[9] + b[13]) / w : 320.0f;
            float dx = sx < 320.0f - hud_dz2 ? -host_wide_x : sx > 320.0f + hud_dz2 ? host_wide_x : 0.0f;
            memcpy(g.xf.m, hud_base, sizeof hud_base);
            if (dx != 0.0f) {
                for (k = 0; k < 4; k++) {
                    g.xf.m[k * 4 + 1] += dx * g.xf.m[k * 4];
                }
            }
            if (hud_logging()) {
                host_log("hud:   group centre x %.1f: moved %.1f", (double)sx, (double)dx);
            }
        }
        if (sphere(g.xf.m, p + 16, scale, &limit) == SPHERE_OUT) {
            p = end;
            continue;
        }
        cache_start(prim);
        if (trnsl < 1.0f) {
            pcw = (pcw | 0x02000000) & ~0x04000000u;
            tsp = ((tsp | 0x001000C0) & 0x03FFFFFF) | (uint32_t)((0xFC ^ trnsl_blend) & 0xFC) << 24;
        }
        pvr_state(&g.state, pcw, isp, tsp, tcw, group_texture(pcw, tcw, texno));
        if (obj_log) {
            host_log("  group pcw %08X isp %08X tsp %08X tcw %08X tex %d mode %d", pcw, isp, tsp, tcw, texno, mode);
        }
        g.xf.lit = mode >= 0 && !(obj[1] & 2);
        g.xf.vtxcol = mode == -3;
        g.xf.ambient = flt(p + 40) * global_ambience;
        for (k = 0; k < 4; k++) {
            g.xf.face[k] = flt(p + 44 + k * 4);
        }
        if ((pcw >> 4 & 3) != 2 || g.xf.vtxcol) {
            g.xf.face[0] = g.xf.face[1] = g.xf.face[2] = g.xf.face[3] = 1.0f;
        }
        g.xf.face[0] *= trnsl;
        g.xf.face[1] *= nl_fade_r;
        g.xf.face[2] *= nl_fade_g;
        g.xf.face[3] *= nl_fade_b;
        g.xf.face_one = g.xf.face[0] == 1.0f && g.xf.face[1] == 1.0f && g.xf.face[2] == 1.0f && g.xf.face[3] == 1.0f;
        g.xf.face_argb = nl_float_argb(g.xf.face[0], g.xf.face[1], g.xf.face[2], g.xf.face[3]);
        GROUP_ADD(PZ_NLG_PRE, t0, GROUP_NOW());
        if (rd_gpu_tl && gpu_group(p, &g)) {
            p = end;
            continue;
        }
        x = g.xf;
        t0 = GROUP_NOW();
        while (prim < end) {
            uint32_t head = word(prim);
            if ((int32_t)head <= 0) {
                break;
            }
            prim = primitive(&g, &x, head, prim + 4);
        }
        GROUP_ADD(PZ_NLG_HERE, t0, GROUP_NOW());
        p = end;
    }
}

/* MVC2_OBJLOG=frame: the models drawn in that frame, and their groups' polygon headers */
static __attribute__((noinline, cold)) int obj_log_frame(int* logged) {
    static int read;
    static const char* e;
    if (!read) {
        e = getenv("MVC2_OBJLOG");
        read = 1;
    }
    return e != NULL && host_frame() == (unsigned)atoi(e) && (*logged)++ < 200;
}

static __attribute__((section(".text.nlhot"))) void put(const int* obj, float trnsl, int use_view) {
    float to_screen[16], zneg, scale_factor;
    static int logged;

    if (obj == NULL || (obj[0] != 0 && obj[0] != 1)) {
        return;
    }
    scale_factor = nl_take_scale_factor();
    if (rd_discard) {
        return;
    }
    memcpy(&zneg, nl_objput_param + 0x0c, 4);
    near_w = -zneg > 0.0f ? -zneg : 1e-3f;     /* before any projection is set */
    if (use_view) {
        mul(to_screen, nl_view_matrix(), nl_cnv_proj_pers);
    } else {
        memcpy(to_screen, nl_cnv_proj_pers, sizeof to_screen);
        if (hud_on) {
            hud_shift(obj, to_screen);
        }
    }
    if (obj_log_frame(&logged)) {
        host_log("obj %p view %d trnsl %g", (const void*)obj, use_view, (double)trnsl);
        obj_log = 1;
    }
    {
        PROF_START(t0);
#ifdef MVC2_PROF
        uint32_t q[3];
        prof_perf_models(0, q);
#endif
        draw(obj, nl_xmtrx(), to_screen, trnsl, scale_factor);
#ifdef MVC2_PROF
        prof_perf_models(1, q);
#endif
        PROF_STOP(PZ_NL_OBJ, t0);
    }
    obj_log = 0;
}

__attribute__((section(".text.nlhot"))) void nlObjPut(int* obj) {
    put(obj, 1.0f, 0);
}

__attribute__((section(".text.nlhot"))) void nlObjPutUseViewMatrix(int* obj) {
    put(obj, 1.0f, 1);
}

__attribute__((section(".text.nlhot"))) void nlObjPutTrnsl(int* obj, float trs) {
    put(obj, trs, 0);
}

__attribute__((section(".text.nlhot"))) void nlObjPutUseViewMatrixTrnsl(int* obj, float trs) {
    put(obj, trs, 1);
}

/* nlobjcem.src: a flagged group's (head bit 8) texture coordinates from its normals turned by the
   matrix; lighting byte -2 left alone, below -2 normals as bytes (z, y, x at 12) */
void nlObjPutCheapEnvMap(int* obj) {
    const float* m = nl_xmtrx();
    uint8_t* p;
    if (obj == NULL || obj[0] < 0) {
        return;
    }
    for (p = (uint8_t*)obj + 24; word(p) != 0; p += 80 + word(p + 76)) {
        int mode = (int8_t)p[36];
        uint8_t *prim = p + 80, *end = prim + word(p + 76);
        if (mode == -2 || !(word(prim) & 0x100)) {
            continue;
        }
        while (prim < end && (int32_t)word(prim) > 0) {
            uint32_t head = word(prim);
            int n = (int)word(prim + 4) * (head & 8 ? 3 : 1);
            prim += 8;
            while (n-- > 0) {
                if (word(prim) & 1) {
                    float nx, ny, nz, u, v, scale = mode < -2 ? 1.0f / 256.0f : 0.5f;
                    uint32_t vw;
                    if (mode < -2) {
                        nz = (int8_t)prim[12], ny = (int8_t)prim[13], nx = (int8_t)prim[14];
                    } else {
                        nx = flt(prim + 12), ny = flt(prim + 16), nz = flt(prim + 20);
                    }
                    u = 0.5f + scale * (m[0] * nx + m[4] * ny + m[8] * nz);
                    v = 0.5f + scale * (m[1] * nx + m[5] * ny + m[9] * nz);
                    memcpy(&vw, &v, 4);
                    vw = (vw & ~1u) | (word(prim + 28) & 1);
                    memcpy(prim + 24, &u, 4);
                    memcpy(prim + 28, &vw, 4);
                    prim += 32;
                } else {
                    prim += 8;
                }
            }
        }
    }
}

void nlObjPutInit(void) {
}

void nlObjPutSetGlobalAmbience(float amb) {
    global_ambience = amb;
}

void nlObjPutSetLightDirectionV(const NLV3* v) {
    light = *v;
}

void nlObjPutSetTrnslMode(int src, int dst) {
    trnsl_blend = (~src & 7) << 5 | (~dst & 7) << 2;
}

void nlObjPutInitTexture(void) {
}

/* each group's texture into the common textures from the offset, its index kept in the group's texture word */
int nlObjPutLoadTexture(int* obj, const NLthd* thd) {
    uint8_t* p;
    if (obj == NULL || obj[0] < 0) {
        return 0;
    }
    for (p = (uint8_t*)obj + 24; word(p) != 0; p += 80 + word(p + 76)) {
        int texno = (int32_t)word(p + 32);
        uint32_t tcw = word(p + 12);
        if (texno >= 0) {
            int index = nl_tex_offset + texno;
            if (index >= 0 && index < NL_MAX_TEX) {
                nl_tex_load(&nl_tex[index], &thd[texno]);
                tcw = (tcw & ~0x1FFFFFu) | (uint32_t)index;
            }
        }
        if ((tcw >> 27 & 7) == 5 || (tcw >> 27 & 7) == 6) {
            tcw = (tcw & 0xF81FFFFF) | (word(p + 60) & 63) << 21;
        }
        memcpy(p + 12, &tcw, 4);
    }
    return 0;
}
