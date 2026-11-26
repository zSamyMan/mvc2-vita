/*
 * rd.c  The frame's triangles, gathered by list for a back end
 */

#include <math.h>
#include <string.h>

#include "rd.h"
#include "rd_list.h"

uint32_t rd_palette[1024];
uint32_t rd_palette_serial;

static RD_FRAME first_frame;
RD_FRAME* rd_frame = &first_frame;
RD_QUALITY rd_q;

void rd_quality(const char* words) {
    rd_q.half = words != NULL && strstr(words, "half") != NULL;
    rd_q.fast = words != NULL && strstr(words, "fast") != NULL;
    rd_q.nofog = words != NULL && strstr(words, "nofog") != NULL;
    rd_q.nosort = words != NULL && strstr(words, "nosort") != NULL;
}

#ifdef __3DS__
/* each list's part of the pool: its share of their recent most, each with a quarter more and 512 over. A list that
   ran out with the pool full is taken to want twice its part. Its most falls by a 64th a frame */
static void share_pool(RD_FRAME* f) {
    static int most[RD_LISTS];
    int want[RD_LISTS], i, sum = 0, at = 0;
    for (i = 0; i < RD_LISTS; i++) {
        int n = f->room[i] > 0 && f->ntris[i] >= f->room[i] ? f->room[i] * 2 : f->ntris[i];
        most[i] = n > most[i] - most[i] / 64 ? n : most[i] - most[i] / 64;
        want[i] = most[i] + most[i] / 4 + 512;
        sum += want[i];
    }
    for (i = 0; i < RD_LISTS; i++) {
        int room = (int)((int64_t)RD_POOL_TRIS * want[i] / sum);
        f->tris[i] = &f->pool[at];
        f->info[i] = &f->info_pool[at];
        f->room[i] = room;
        at += room;
    }
    f->dropped = 0;
    f->moved = 0;
}

/* a list's part full: the parts laid out again, each keeping its triangles, the list given half of the pool's unused
   rest and the others the other half. 0 when none is left */
static int grow_part(RD_FRAME* f, int list) {
    int start[RD_LISTS], room[RD_LISTS], i, used = 0, spare, mine, at = 0;
    for (i = 0; i < RD_LISTS; i++) {
        used += f->ntris[i];
    }
    spare = RD_POOL_TRIS - used;
    if (spare <= 0) {
        return 0;
    }
    mine = (spare + 1) / 2;
    for (i = 0; i < RD_LISTS; i++) {
        room[i] = f->ntris[i] + (i == list ? mine : (spare - mine) / (RD_LISTS - 1));
        start[i] = at;
        at += room[i];
    }
    room[RD_LISTS - 1] += RD_POOL_TRIS - at;
    /* those moving up from the last, then those moving down from the first: none lands on one not yet moved */
    for (i = RD_LISTS - 1; i >= 0; i--) {
        if (&f->pool[start[i]] > f->tris[i]) {
            memmove(&f->pool[start[i]], f->tris[i], (size_t)f->ntris[i] * sizeof(RD_TRI));
            memmove(&f->info_pool[start[i]], f->info[i], (size_t)f->ntris[i] * sizeof(RD_TRI_INFO));
        }
    }
    for (i = 0; i < RD_LISTS; i++) {
        if (&f->pool[start[i]] < f->tris[i]) {
            memmove(&f->pool[start[i]], f->tris[i], (size_t)f->ntris[i] * sizeof(RD_TRI));
            memmove(&f->info_pool[start[i]], f->info[i], (size_t)f->ntris[i] * sizeof(RD_TRI_INFO));
        }
        f->tris[i] = &f->pool[start[i]];
        f->info[i] = &f->info_pool[start[i]];
        f->room[i] = room[i];
    }
    f->moved++;
    return 1;
}
#endif

void rd_begin_frame(void) {
    int i;
#ifdef __3DS__
    share_pool(rd_frame);
#else
    for (i = 0; i < RD_LISTS; i++) {
        rd_frame->tris[i] = &rd_frame->pool[i * RD_MAX_TRIS];
        rd_frame->room[i] = RD_MAX_TRIS;
    }
#endif
    for (i = 0; i < RD_LISTS; i++) {
        rd_frame->ntris[i] = 0;
    }
    rd_frame->nstates = 0;
    rd_frame->nputs = 0;
    rd_frame->ngroups = 0;
}

RD_FOG rd_fog;

void rd_background_plane(const RD_VTX* v) {
    memcpy(rd_frame->background, v, sizeof rd_frame->background);
}

void rd_background(uint32_t argb) {
    RD_VTX v[3];
    memset(v, 0, sizeof v);
    v[1].x = RD_WIDTH;
    v[2].y = RD_HEIGHT;
    v[0].base = v[1].base = v[2].base = argb;
    rd_background_plane(v);
}

/* the fog registers keep their values from frame to frame */
void rd_fog_table(const float* table) {
    int i;
    for (i = 0; i < 128; i++) {
        float f = table[i];
        rd_fog.table[i] = (float)(int)((f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f) * 255.0f + 0.5f) / 255.0f;
    }
}

void rd_fog_color(uint32_t argb) {
    rd_fog.color = argb;
}

void rd_fog_density(uint32_t reg) {
    rd_fog.density = (float)(reg >> 8 & 255) / 128.0f * ldexpf(1.0f, (int8_t)(reg & 255));
}

/* the frame's copy of s: the last one again when it is the same */
static __attribute__((section(".text.nlhot"))) int add_state(const RD_STATE* s) {
    if (rd_frame->nstates > 0 && rd_same_state(&rd_frame->states[rd_frame->nstates - 1], s)) {
        return rd_frame->nstates - 1;
    }
    if (rd_frame->nstates == RD_MAX_STATES) {
        return -1;
    }
    rd_frame->states[rd_frame->nstates] = *s;
#ifdef __3DS__
    rd_frame->seen[rd_frame->nstates] = 0;
#endif
    return rd_frame->nstates++;
}

static int add_tri(int list, int state, const RD_VTX* a, const RD_VTX* b, const RD_VTX* c, int share, int stays, int culled) {
    RD_TRI* t;
    int i = rd_frame->ntris[list];
    if (i >= rd_frame->room[list]) {
#ifdef __3DS__
        if (!grow_part(rd_frame, list)) {
            rd_frame->dropped++;
            return 0;
        }
#else
        return 0;
#endif
    }
    t = &rd_frame->tris[list][rd_frame->ntris[list]++];
    t->state = (uint16_t)state;
    t->share = (uint8_t)share;
    t->stays = (uint8_t)stays;
    t->v[0] = *a;
    t->v[1] = *b;
    t->v[2] = *c;
#ifdef __3DS__
    if (rd_seen && !rd_frame->seen[state] && (list != RD_TRANS || !rd_unseen(a, b, c))) {
        rd_frame->seen[state] = 1;
    }
#endif
    (void)culled;   /* the 3DS finds it again with the triangle's info, as its lists are built */
    return 1;
}

int rd_discard;
#ifdef __3DS__
int rd_seen, rd_group_info;
#endif

/* the back ends' area, from the same corners in the same order */
static double area(const RD_VTX* a, const RD_VTX* b, const RD_VTX* c) {
    return ((double)b->x - a->x) * ((double)c->y - a->y) - ((double)b->y - a->y) * ((double)c->x - a->x);
}

/* ASPECT_RATIO=WIDE: the stage's models (host_wide_stage on, from the game's stage draws) made wider about the
   world's x 0, the more the farther behind the players' plane (z 0) they lie: a far band or a wall's end that stopped
   short of the wider screen's side reaches past it. In the world, not on the screen: a corner's x from its own world x
   (its x on the screen less the camera's), so the widening is still as the camera moves and every depth scrolls
   as fast as before; nothing seen slides or stretches with the camera, and the players' plane is as it was. The
   camera looks down -z there (eye.z from the plane, F its focal length in the game's pixels) */
static struct {
    int on;
    float xe, ez;
} wide_stage;
/* the widening by depth behind the players' plane: WIDE_STAGE_MID reached by WIDE_STAGE_D0 and kept the same as far
   as WIDE_STAGE_D1, then growing by WIDE_STAGE_FAR more to WIDE_STAGE_D2. A widening the same at every depth is the
   world made wider: the stage then looks as it does from a camera a little nearer its middle, stretched as a whole,
   every layer of a model built of layers at different depths still lined up. Widened more with depth, those layers
   parted (stage 4's tree, the clock tower: 4 to 10% by depth); flat at 3% from 400 the ends fell short again (stage
   4's left wall, the clock tower's corners) and the tree, nearer than 400, still parted in the rise. So: the rise
   kept to the floor by the players (to 60: the clock tower's right hall, near, fell a corner short at 150 and 8%),
   the middle as wide as the ends need (12%: the clock tower's right wall, 5760 behind, ended at x 738 with 9%,
   7 px short, its end seen through the translucent piece before it; 11.1% reaches), the far bands alone more (stage 1's mesas,
   9100 behind, ending at x 693 at the right wall: 16%, 70 px more; 13.5% left a pixel) */
#define WIDE_STAGE_MID 0.12f
#define WIDE_STAGE_FAR 0.04f
#define WIDE_STAGE_D0 60.0f
#define WIDE_STAGE_D1 7000.0f
#define WIDE_STAGE_D2 9000.0f
#define WIDE_STAGE_F 812.3573f  /* 320 / tan(43 / 2 degrees): fov_x's, and the players' plane's distance */
void host_wide_stage(int on, float xe, float ez) {
    wide_stage.on = on, wide_stage.xe = xe, wide_stage.ez = ez;
}
static const RD_VTX* widen(const RD_VTX* v, int n, RD_VTX* out) {
    int i;
    for (i = 0; i < n; i++) {
        float z = v[i].z, d;
        out[i] = v[i];
        if (!(z > 0.0f && z < 3.0e38f)) {
            continue;
        }
        d = 1.0f / z - wide_stage.ez;    /* behind the players' plane */
        if (d > 0.0f) {
            float s0 = 320.0f - WIDE_STAGE_F * wide_stage.xe * z;    /* the world's x 0 at its depth */
            float k = d < WIDE_STAGE_D0 ? WIDE_STAGE_MID / WIDE_STAGE_D0 * d : WIDE_STAGE_MID;
            if (d > WIDE_STAGE_D1) {
                k += d < WIDE_STAGE_D2 ? WIDE_STAGE_FAR / (WIDE_STAGE_D2 - WIDE_STAGE_D1) * (d - WIDE_STAGE_D1) : WIDE_STAGE_FAR;
            }
            out[i].x = v[i].x + k * (v[i].x - s0);
        }
    }
    return out;
}

void rd_strip(const RD_STATE* s, const RD_VTX* v, int n) {
    int st, i, added = 0;
    if (rd_discard || (st = add_state(s)) < 0) {
        return;
    }
    static RD_VTX wv[256];     /* the game's thread alone draws */
    if (wide_stage.on && n <= 256) {
        v = widen(v, n, wv);
    }
    for (i = 2; i < n; i++) {
        /* one winding for the whole strip */
        const RD_VTX *a = i & 1 ? &v[i - 1] : &v[i - 2], *b = i & 1 ? &v[i - 2] : &v[i - 1];
        int stays = 1;
        if (s->cull != 0) {
            double d = area(a, b, &v[i]);
            /* every back end culls it by its winding (cull 2: area below 0, 3: above) */
            if (s->cull >= 2 && (s->cull == 2 ? d < 0 : d > 0)) {
                added = 0;
                continue;
            }
            stays = !(fabs(d) < RD_CULL_VAL);
        }
        added = add_tri(s->list, st, a, b, &v[i], !added ? 0 : i & 1 ? RD_SHARE(2, 1) : RD_SHARE(0, 2), stays,
                        s->cull != 0 && !stays);
    }
}

#ifdef __3DS__
/* a quad's triangle culled by its state, by its area as the back ends find it */
static int quad_culled(const RD_STATE* s, const RD_VTX* a, const RD_VTX* b, const RD_VTX* c) {
    double d;
    if (s->cull == 0) {
        return 0;
    }
    d = area(a, b, c);
    return fabs(d) < RD_CULL_VAL || (s->cull == 2 && d < 0) || (s->cull == 3 && d > 0);
}
#else
#define quad_culled(s, a, b, c) 0
#endif

int rd_snap_x;

void rd_quad(const RD_STATE* s, const RD_VTX* v) {
    RD_VTX q[4];
    int st, first;
    if (rd_discard || (st = add_state(s)) < 0) {
        return;
    }
    RD_VTX wv[4];
    if (wide_stage.on) {
        v = widen(v, 4, wv);
    }
    if (rd_snap_x && v[0].x == v[3].x && v[1].x == v[2].x) {
        /* each side to the first column whose centre is at or past it */
        memcpy(q, v, sizeof q);
        q[0].x = q[3].x = ceilf(v[0].x - 0.5f);
        q[1].x = q[2].x = ceilf(v[1].x - 0.5f);
        v = q;
    }
    first = add_tri(s->list, st, &v[0], &v[1], &v[2], 0, s->cull == 0, quad_culled(s, &v[0], &v[1], &v[2]));
    add_tri(s->list, st, &v[0], &v[2], &v[3], first ? RD_SHARE(0, 2) : 0, s->cull == 0, quad_culled(s, &v[0], &v[2], &v[3]));
}

int rd_gpu_tl;
int rd_gpu_tl_kinds = 7, rd_gpu_tl_max = RD_GPU_GROUPS, rd_gpu_tl_trans;
float rd_gpu_tl_trans_deep;
uint32_t rd_gpu_tl_deep_left;
int rd_gpu_tl_two_pass;
uint32_t rd_gpu_tl_two_passed;
int rd_gpu_tl_nearclip;
uint32_t rd_gpu_tl_near_left;

__attribute__((section(".text.nlhot"))) int rd_gpu_put(const float m[16], float near_w) {
    RD_GPU_PUT* p;
    if (!rd_gpu_tl || rd_discard || rd_frame->nputs == RD_GPU_PUTS) {
        return -1;
    }
    p = &rd_frame->puts[rd_frame->nputs];
    memcpy(p->m, m, sizeof p->m);
    p->near_w = near_w;
    return rd_frame->nputs++;
}

/* in words: gcc calls memcpy for a struct over 64 bytes */
static void copy_words(void* to, const void* from, size_t n) {
    uint32_t w;
    size_t i;
    for (i = 0; i < n / 4; i++) {
        memcpy(&w, (const char*)from + 4 * i, 4);
        memcpy((char*)to + 4 * i, &w, 4);
    }
}

_Static_assert(sizeof(RD_GPU_GROUP) % 4 == 0, "copied in words");

/* drawn where its list's triangles sent so far end */
__attribute__((section(".text.nlhot"))) int rd_gpu_group(int put, const RD_STATE* s, const RD_GPU_GROUP* g) {
    RD_GPU_DRAW* d;
    int st, deep = s->list == RD_TRANS && rd_gpu_tl_trans_deep > 0.0f && !(g->wlo > 0.0f && g->whi <= g->wlo * rd_gpu_tl_trans_deep);
    /* a deep translucent group is sorted whole, at one depth: the triangles it passes through would go wrong,
       but for a two-sided shell, whose back faces are behind its front ones */
    if (deep && !(rd_gpu_tl_two_pass && g->two_sided)) {
        rd_gpu_tl_deep_left++;
        return 0;
    }
    if (rd_gpu_tl_nearclip && put >= 0 && put < rd_frame->nputs && !(g->wlo >= rd_frame->puts[put].near_w)) {
        rd_gpu_tl_near_left++;
        return 0;
    }
    if (put < 0 || put >= rd_frame->nputs || rd_frame->ngroups + 1 + deep > rd_gpu_tl_max || !(rd_gpu_tl_kinds >> g->kind & 1) ||
        (st = add_state(s)) < 0) {
        return 0;
    }
    d = &rd_frame->groups[rd_frame->ngroups++];
    copy_words(&d->g, g, sizeof *g);
    d->g.faces = 0;
    d->put = (uint16_t)put;
    d->state = (uint16_t)st;
#ifdef __3DS__
    rd_frame->seen[st] = 1;
#endif
    d->at = rd_frame->ntris[s->list];
    if (deep) {
        d->g.faces = 1;
        d->g.key = g->key_far;
        d[1] = d[0];
        d[1].g.faces = 2;
        d[1].g.key = g->key_near;
        rd_frame->ngroups++;
        rd_gpu_tl_two_passed++;
    }
#ifdef __3DS__
    if (rd_group_info) {
        int k;
        for (k = 0; k <= deep; k++) {
            RD_GPU_INFO q;
            q.wlo = g->wlo;
            q.whi = g->whi;
            q.key = !deep ? g->key : k == 0 ? g->key_far : g->key_near;
            q.near_w = rd_frame->puts[put].near_w;
            q.at = rd_frame->ntris[s->list];
            q.mesh = (int16_t)(g->mesh >= 0 && g->mesh <= 32767 ? g->mesh : -1);
            q.list = s->list;
            q.free = (uint8_t)rd_order_free(s);
            rd_frame->ginfo[d - rd_frame->groups + k] = q;
        }
    }
#endif
    return 1;
}

#ifndef __3DS__
/* only the 3DS's back end draws them */
int rd_gpu_mesh_make(const NL_MESH* m) {
    (void)m;
    return -1;
}

void rd_gpu_mesh_free(int id) {
    (void)id;
}

uint32_t rd_gpu_mesh_room(void) {
    return 0;
}
#endif
