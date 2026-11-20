/*
 * dc_adx.c  CRI's ADX player, the Dreamcast's music from the disc: one player, decoded a frame at a time on the
 * game's thread and mixed after the AICA. The game only waits on it, so what it reads back changes no frame. On the
 * 3DS with MVC2_ADX_THREAD=1, a file read ahead is decoded on the sound thread instead, as each frame is made.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "adx.h"
#include "naomi_host.h"
#include "shinobi.h"
#include "snd.h"

#ifdef __3DS__
#include <3ds.h>
#endif
#ifdef __vita__
#include <stdio.h>
#include <psp2/kernel/threadmgr.h>
#endif

#define ADX_SECTORS 16
#define ADX_BUF (ADX_SECTORS * GDD_SCT_SIZE)
#define ADX_BLOCK 18            /* a channel's 32 samples: a 16-bit scale and 16 bytes of 4-bit deltas */

Sint32 gdFsSeek(GDFS gd, Sint32 offset, Sint32 origin);    /* dc_gd.c */

struct _adx_talk {
    Sint32 vol;                 /* 0.1 dB steps, 0 the file's own level */
    Sint32 stat;
    Sint32 pause;
    GDFS gd;
    uint32_t size;
    int ch;
    uint32_t rate;
    uint32_t total;             /* samples a channel */
    int loop;
    uint32_t loop_start, loop_end;
    uint32_t data;              /* the first block */
    int32_t coef1, coef2;
    int32_t hist[2][2];
    int16_t dec[2][32];
    uint32_t sample;            /* the next sample to decode */
    int dec_left;
    uint32_t step, frac;        /* the file's rate against the output's, 16.16 */
    int16_t out[2];
    uint8_t buf[ADX_BUF];
    uint32_t buf_off, buf_len;  /* the file's bytes in buf */
#ifdef __vita__
    const uint8_t* head;        /* the file's first head_len bytes, kept in memory by dc_gd.c */
    uint32_t head_len;
    int pre_on;                 /* the reader is reading this file whole: bytes past head wait for it */
#endif
};

static struct _adx_talk player;

static uint32_t be16(const uint8_t* p) {
    return (uint32_t)p[0] << 8 | p[1];
}

static uint32_t be32(const uint8_t* p) {
    return be16(p) << 16 | be16(p + 2);
}

#ifdef __3DS__
/* MVC2_ADX_FILL=1, with MVC2_ADX_AHEAD: the music mixed into the sound thread's frames between the game's too, from
   the pieces read ahead only. The player is the game thread's ADXT calls' and the sound thread's by turns, under
   fill.lock; a file that ends on the sound thread is closed by the game's. */
static struct {
    int on;                     /* the player shared with the sound thread, under lock */
    LightLock lock;
    int ended;                  /* a file ended, not closed yet */
    int extra;                  /* the sound thread mixing a frame between the game's */
    int between;                /* MVC2_ADX_FILL's frames between the game's made */
    int thread, threaded;       /* MVC2_ADX_THREAD; the file playing decoded on the sound thread */
    int sound;                  /* the sound thread mixing the game's frame */
    uint32_t made, unready, busy, blind;
} fill;

#define FILL_LOCK()                                                                                                    \
    do {                                                                                                               \
        if (fill.on) {                                                                                                 \
            LightLock_Lock(&fill.lock);                                                                                \
        }                                                                                                              \
    } while (0)
#define FILL_UNLOCK()                                                                                                  \
    do {                                                                                                               \
        if (fill.on) {                                                                                                 \
            LightLock_Unlock(&fill.lock);                                                                              \
        }                                                                                                              \
    } while (0)
#else
#define FILL_LOCK()
#define FILL_UNLOCK()
#endif

#ifdef __3DS__
/* MVC2_ADX_AHEAD=1: the file read ahead by ctr_stream.c, in pieces of ADX_SECTORS sectors that start one sector
   short of the last one's end, so that a block is always whole in one. The first piece is what the header was read
   with; after the piece with the last block played the pieces go on from the loop's start. A block whose piece
   isn't read yet is read here, as without it. */
#define NO_PIECE 0xFFFFFFFFu

static int ahead_on = -1;
static struct {
    int on, cur;                /* the piece in use, or -1 */
    const uint8_t* at;
    uint32_t off;               /* its bytes, from off */
    uint32_t next;              /* the next piece's first sector, or NO_PIECE */
    uint32_t last, loop;        /* the last block played's offset; the loop's first sector */
    uint32_t taken, here, missed;
} ah;

/* the piece after the one from sector sct */
static uint32_t ahead_after(const struct _adx_talk* p, uint32_t sct) {
    uint32_t step = sct + ADX_SECTORS - 1;
    return ah.last >= step * GDD_SCT_SIZE ? step : p->loop ? ah.loop : NO_PIECE;
}

static void ahead_queue(const struct _adx_talk* p) {
    while (ah.next != NO_PIECE && ctr_stream_queue(ah.next * GDD_SCT_SIZE, ADX_BUF) >= 0) {
        ah.next = ahead_after(p, ah.next);
    }
}

static void ahead_start(ADXT p, const char* fname) {
    uint32_t bytes = ADX_BLOCK * (uint32_t)p->ch;
    if (ahead_on < 0) {
        ahead_on = getenv("MVC2_ADX_AHEAD") != NULL && atoi(getenv("MVC2_ADX_AHEAD")) != 0;
        if (ahead_on) {
            host_log("adx: the music read ahead on another thread (MVC2_ADX_AHEAD)");
        }
    }
    if (!ahead_on || p->total == 0 || !ctr_stream_open(dc_gd_path(fname), ADX_BUF)) {
        return;
    }
    ah.on = 1;
    ah.cur = -1;
    ah.last = p->data + ((p->loop ? p->loop_end : p->total) - 1) / 32 * bytes;
    ah.loop = (p->data + p->loop_start / 32 * bytes) / GDD_SCT_SIZE;
    ah.next = p->data / GDD_SCT_SIZE;
    if (p->buf_len == ADX_BUF && p->buf_off == ah.next * GDD_SCT_SIZE &&
        ctr_stream_put(p->buf_off, p->buf_len, p->buf) >= 0) {
        ah.next = ahead_after(p, ah.next);
    }
    ahead_queue(p);
}

static void ahead_stop(void) {
    if (ah.on) {
        ctr_stream_close();
        ah.on = 0;
        ah.cur = -1;
    }
}

/* the bytes from a piece read ahead; NULL when its piece isn't read yet, or wasn't asked for (asked for from here) */
static const uint8_t* ahead_at(ADXT p, uint32_t off, uint32_t n) {
    const uint8_t* at;
    uint32_t from;
    int i;
    if (ah.cur >= 0 && off >= ah.off && off + n <= ah.off + ADX_BUF) {
        return ah.at + (off - ah.off);
    }
    if ((i = ctr_stream_find(off, n)) < 0) {
        ah.missed++;
        ctr_stream_drop();
        ah.cur = -1;
        ah.next = off / GDD_SCT_SIZE;
        ahead_queue(p);
        return NULL;
    }
    if ((at = ctr_stream_take(i, &from)) == NULL) {
        return NULL;
    }
    if (ah.cur >= 0) {
        ctr_stream_free(ah.cur);
    }
    ah.at = at;
    ah.off = from;
    ah.cur = i;
    ah.taken++;
    ahead_queue(p);
    return ah.at + (off - ah.off);
}

static void ahead_report(void) {
    static unsigned frame;
    unsigned f = host_frame();
    if (ah.taken != 0 && f / 600 != frame / 600) {
        float mean, most;
        uint32_t n = ctr_stream_times(&mean, &most);
        frame = f;
        host_log("adx ahead: %u pieces taken, %u reads here, %u missed; %u read, %.1f ms each, %.1f at most",
                 (unsigned)ah.taken, (unsigned)ah.here, (unsigned)ah.missed, (unsigned)n, (double)mean, (double)most);
        if (fill.between || fill.thread) {
            host_log("adx fill: %u frames mixed between the game's, %u not read yet, %u while busy, %u blind",
                     (unsigned)fill.made, (unsigned)fill.unready,
                     (unsigned)__atomic_exchange_n(&fill.busy, 0, __ATOMIC_RELAXED), (unsigned)fill.blind);
            fill.made = fill.unready = fill.blind = 0;
        }
        ah.taken = ah.here = ah.missed = 0;
    }
}

/* whether the next n samples' blocks are all in pieces read already: mix_run's steps, without the decoding */
static int ahead_ready(const struct _adx_talk* p, uint32_t n) {
    uint32_t bytes = ADX_BLOCK * (uint32_t)p->ch;
    uint32_t s = p->sample, d = (uint32_t)p->dec_left;
    uint32_t from = ah.cur >= 0 ? ah.off : NO_PIECE;
    while (n > 0) {
        uint32_t k;
        if (p->loop && s >= p->loop_end) {
            s = p->loop_start;
            d = 0;
        }
        if (s >= p->total) {
            return 1;
        }
        if (d == 0) {
            uint32_t off = p->data + s / 32 * bytes;
            if (from == NO_PIECE || off < from || off + bytes > from + ADX_BUF) {
                int i = ctr_stream_find(off, bytes);
                if (i < 0 || ctr_stream_take(i, &from) == NULL) {
                    return 0;
                }
            }
            d = 32;
        }
        k = d < 32 - (s & 31) ? d : 32 - (s & 31);
        k = k < n ? k : n;
        k = k < p->total - s ? k : p->total - s;
        if (p->loop) {
            k = k < p->loop_end - s ? k : p->loop_end - s;
        }
        s += k;
        d -= k;
        n -= k;
    }
    return 1;
}
#endif

#ifdef __vita__
/*
 * The Vita's memory card takes some 20 ms for any read, and a read of the music's next 32 KB now and then (on the
 * host's thread, in the frame's sound) took up to 140 ms: a stutter every few seconds of a fight. So the whole file
 * is read into memory by a thread on core 2 as it starts playing, 64 KB at a time, and the player takes its bytes
 * from there; only those not read yet (the song's first moment) are read here as before. A new song bumps the
 * generation: the reader leaves the old one at its next piece, and the buffer is reused once it has.
 */
#define PRE_PIECE (64u << 10)
#define PRE_ROOM (8u << 20)     /* the largest song (ADX_*.BIN) is under 6 MB: one buffer, never moved */
static struct {
    SceUID thread, go, lock;
    uint8_t* data;              /* PRE_ROOM bytes, the reader's: allocated once, so never moved under the player */
    volatile uint32_t size;     /* the wanted song's size (0: none), set by pre_start */
    volatile uint32_t ready;    /* bytes of the song the reader is on, read so far */
    volatile int gen;           /* the wanted song's generation, bumped by pre_start */
    volatile int cur;           /* the generation the reader is on: its bytes count only when it is gen */
    volatile int busy;
    char want[1200];            /* the wanted song's path, under lock */
    uint32_t want_size;
} pre;

/* the bytes the player may take: the reader is on the song wanted, so far */
static uint32_t pre_ready(void) {
    int g = pre.gen;
    __sync_synchronize();
    return pre.cur == g ? pre.ready : 0;
}

static int pre_main(SceSize args, void* argp) {
    (void)args, (void)argp;
    for (;;) {
        int gen;
        uint32_t size;
        char path[1200];
        FILE* f;
        sceKernelWaitSema(pre.go, 1, NULL);
        sceKernelLockMutex(pre.lock, 1, NULL);
        gen = pre.gen;
        size = pre.want_size;
        snprintf(path, sizeof path, "%s", pre.want);
        sceKernelUnlockMutex(pre.lock, 1);
        if (size == 0 || pre.cur == gen) {
            continue;       /* none wanted, or a signal for the song already read */
        }
        pre.busy = 1;
        pre.ready = 0;
        __sync_synchronize();
        pre.cur = gen;
        f = fopen(path, "rb");
        if (f == NULL) {
            host_log("adx: could not open %s to read it ahead", path);
        }
        if (f != NULL) {
            setvbuf(f, NULL, _IONBF, 0);
            while (pre.ready < size && pre.gen == gen) {
                uint32_t n = size - pre.ready < PRE_PIECE ? size - pre.ready : PRE_PIECE;
                if (fread(pre.data + pre.ready, 1, n, f) != n) {
                    break;
                }
                __sync_synchronize();
                pre.ready += n;
            }
            fclose(f);
            host_log("adx: %s read ahead, %u of %u KB%s", path, (unsigned)(pre.ready >> 10), (unsigned)(size >> 10),
                     pre.gen == gen ? "" : " (left for the next song)");
        }
        pre.busy = 0;
    }
    return 0;
}

/* the next song for the reader (size 0: none), at once: the game's thread never waits for the reader to leave the
   old one (on a slow memory card its open or piece in hand took seconds, the game held up meanwhile); the player
   takes the new song's head from memory, then silence, until the reader is on it */
static void pre_start(const char* fname, uint32_t size) {
    if (pre.thread == 0) {
        pre.data = malloc(PRE_ROOM);
        pre.go = sceKernelCreateSema("adx_pre", 0, 0, 1, NULL);
        pre.lock = sceKernelCreateMutex("adx_pre", 0, 0, NULL);
        pre.thread = pre.data == NULL ? -1 : sceKernelCreateThread("adx_reader", pre_main, 170, 32 * 1024, 0, 0x00040000, NULL);
        {
            extern void rd_vgl_watch_thread(const char* name, int id) __attribute__((weak));
            if (rd_vgl_watch_thread && pre.thread >= 0) {
                rd_vgl_watch_thread("ADX", pre.thread);
            }
        }
        if (pre.thread < 0 || sceKernelStartThread(pre.thread, 0, NULL) < 0) {
            pre.thread = -1;
        }
    }
    if (pre.thread < 0) {
        pre.size = 0;
        if (size != 0) {
            host_log("adx: no reader thread: the music is read as it plays");
        }
        return;
    }
    if (size > PRE_ROOM) {
        size = 0;           /* not read ahead: read as it plays */
    }
    sceKernelLockMutex(pre.lock, 1, NULL);
    if (size != 0) {
        snprintf(pre.want, sizeof pre.want, "%s", dc_gd_path(fname));
    }
    pre.want_size = size;
    pre.size = size;
    __sync_synchronize();
    pre.gen++;
    sceKernelUnlockMutex(pre.lock, 1);
    if (size != 0) {
        sceKernelSignalSema(pre.go, 1);
    }
}
#endif

/* the file's bytes at off (n of them, n at most a sector), read in when they are not in the buffer */
static const uint8_t* bytes_at(ADXT p, uint32_t off, uint32_t n) {
#ifdef __vita__
    if (off + n <= pre_ready()) {
        return pre.data + off;
    }
    if (p->head != NULL && off + n <= p->head_len) {
        return p->head + off;
    }
    if (p->pre_on && pre.size > 0 && (pre.busy || pre.cur != pre.gen)) {
        /* not read yet: a read here would wait behind the reader's on the memory card (once 4 s, the game
           held up); a moment of silence instead, the reader soon past it */
        static const uint8_t silence[GDD_SCT_SIZE * 2] = { 0 };
        static unsigned told;
        if (told++ < 8) {
            host_log("adx: byte %u not read yet (%u of %u): silence (frame %u)", off, (unsigned)pre_ready(),
                     (unsigned)pre.size, host_frame());
        }
        return n <= sizeof silence ? silence : p->head;
    }
#endif
#ifdef __3DS__
    if (ah.on) {
        const uint8_t* b = ahead_at(p, off, n);
        if (b != NULL) {
            return b;
        }
    }
    if (fill.sound) {
        /* on the sound thread the reader's piece is waited for, never read here */
        int i;
        for (i = 0; i < 200 && ah.on; i++) {
            const uint8_t* b;
            svcSleepThread(1000000);
            if ((b = ahead_at(p, off, n)) != NULL) {
                return b;
            }
        }
    }
    if (fill.extra || fill.sound) {
        /* never on the sound thread: ahead_ready said the block was read */
        static const uint8_t silence[ADX_BLOCK * 2] = {0};
        fill.blind++;
        return silence;
    }
#endif
    if (off < p->buf_off || off + n > p->buf_off + p->buf_len) {
        uint32_t sct = off / GDD_SCT_SIZE;
#ifdef __3DS__
        ah.here += (uint32_t)ah.on;
#endif
        gdFsSeek(p->gd, (Sint32)sct, 0);
        gdFsRead(p->gd, ADX_SECTORS, p->buf);
        p->buf_off = sct * GDD_SCT_SIZE;
        p->buf_len = ADX_BUF;
    }
    return p->buf + (off - p->buf_off);
}

static void close_files(ADXT p) {
#ifdef __3DS__
    ahead_stop();
#endif
#ifdef __vita__
    pre_start(NULL, 0);
#endif
    if (p->gd != NULL) {
        gdFsClose(p->gd);
        p->gd = NULL;
    }
}

static void stop(ADXT p) {
    close_files(p);
    p->stat = ADXT_STAT_STOP;
}

/* the game thread's stop: the sound thread off the player before its files are closed */
static void halt(ADXT p) {
#ifdef __3DS__
    FILL_LOCK();
    p->stat = ADXT_STAT_STOP;
    fill.ended = 0;
    FILL_UNLOCK();
    close_files(p);
#else
    stop(p);
#endif
}

/* the file played to its end; with MVC2_ADX_FILL closed on the game's thread after */
static void end_file(ADXT p) {
#ifdef __3DS__
    if (fill.on) {
        fill.ended = 1;
        p->stat = ADXT_STAT_PLAYEND;
        return;
    }
#endif
    stop(p);
    p->stat = ADXT_STAT_PLAYEND;
}

/* decodes the 32 samples from sample on (a multiple of 32) */
static void decode_block(ADXT p) {
    uint32_t block = p->sample / 32;
    const uint8_t* b = bytes_at(p, p->data + block * ADX_BLOCK * (uint32_t)p->ch, ADX_BLOCK * (uint32_t)p->ch);
    int c, i;
    for (c = 0; c < p->ch; c++, b += ADX_BLOCK) {
        int32_t scale = (int32_t)(be16(b) & 0x1FFF) + 1;
        int32_t h1 = p->hist[c][0], h2 = p->hist[c][1];
#ifndef __3DS__
        for (i = 0; i < 32; i++) {
            int n = b[2 + i / 2] >> (i & 1 ? 0 : 4) & 15;
            int32_t s = (n >= 8 ? n - 16 : n) * scale + (p->coef1 * h1 >> 12) + (p->coef2 * h2 >> 12);
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            p->dec[c][i] = (int16_t)s;
            h2 = h1;
            h1 = s;
        }
#else
        /* a byte's two samples a pass */
        const int32_t c1 = p->coef1, c2 = p->coef2;
        int16_t* d = p->dec[c];
        for (i = 0; i < 16; i++) {
            uint32_t v = b[2 + i];
            int32_t s = ((int32_t)(v << 24) >> 28) * scale + (c1 * h1 >> 12) + (c2 * h2 >> 12);
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            d[i * 2] = (int16_t)s;
            h2 = h1;
            h1 = s;
            s = ((int32_t)(v << 28) >> 28) * scale + (c1 * h1 >> 12) + (c2 * h2 >> 12);
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            d[i * 2 + 1] = (int16_t)s;
            h2 = h1;
            h1 = s;
        }
#endif
        p->hist[c][0] = h1;
        p->hist[c][1] = h2;
    }
    p->dec_left = 32 - (int)(p->sample & 31);
}

/* the next sample of each channel; 0 when the file has ended */
static int next_sample(ADXT p) {
    int i;
    if (p->loop && p->sample >= p->loop_end) {
        p->sample = p->loop_start;
        p->dec_left = 0;
    }
    if (p->sample >= p->total) {
        end_file(p);
        return 0;
    }
    if (p->dec_left == 0) {
        uint32_t want = p->sample;
        p->sample &= ~31u;
        decode_block(p);
        p->sample = want;
    }
    i = (int)(p->sample & 31);
    p->out[0] = p->dec[0][i];
    p->out[1] = p->dec[p->ch - 1][i];
    p->sample++;
    p->dec_left--;
    return 1;
}

#ifdef __3DS__
static int16_t gain(int32_t s, int32_t g) {
    s = s * g >> 15;
    return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

/* one run of samples from a block, at most left of them, into buf: next_sample's; 0 when the file has ended */
static uint32_t mix_run(ADXT p, int16_t* buf, uint32_t left, int32_t g) {
    const int16_t *l, *r;
    uint32_t k, i;
    int j;
    if (p->loop && p->sample >= p->loop_end) {
        p->sample = p->loop_start;
        p->dec_left = 0;
    }
    if (p->sample >= p->total) {
        end_file(p);
        return 0;
    }
    if (p->dec_left == 0) {
        uint32_t want = p->sample;
        p->sample &= ~31u;
        decode_block(p);
        p->sample = want;
    }
    j = (int)(p->sample & 31);
    k = (uint32_t)p->dec_left;
    k = k < 32u - (uint32_t)j ? k : 32u - (uint32_t)j;
    k = k < left ? k : left;
    k = k < p->total - p->sample ? k : p->total - p->sample;
    if (p->loop) {
        k = k < p->loop_end - p->sample ? k : p->loop_end - p->sample;
    }
    l = &p->dec[0][j];
    r = &p->dec[p->ch - 1][j];
    p->out[0] = l[k - 1];
    p->out[1] = r[k - 1];
    p->sample += k;
    p->dec_left -= (int)k;
    if (g == 32768) {
        for (i = 0; i < k; i++) {
            buf[i * 2] = l[i];
            buf[i * 2 + 1] = r[i];
        }
    } else {
        for (i = 0; i < k; i++) {
            buf[i * 2] = gain(l[i], g);
            buf[i * 2 + 1] = gain(r[i], g);
        }
    }
    return k;
}

/* adx_mix's samples when the file's rate is the output's: next_sample's, a block's run at a time */
static void mix_same_rate(ADXT p, int16_t* buf, int n, int32_t g) {
    uint32_t i = 0, k;
    while (i < (uint32_t)n && (k = mix_run(p, buf + i * 2, (uint32_t)n - i, g)) != 0) {
        i += k;
    }
}
#endif

static void adx_mix(int16_t* buf, int n) {
    static Sint32 g_vol = 0;
    static int32_t g = 32768;
    ADXT p = &player;
    int i, c;
    if (p->stat != ADXT_STAT_PLAYING || p->pause) {
        return;
    }
    if (p->vol != g_vol) {
        g_vol = p->vol;
        g = (int32_t)(32768.0 * pow(10.0, g_vol / 200.0));
    }
#ifdef __3DS__
    if (p->step == 0x10000 && p->frac == 0) {
        mix_same_rate(p, buf, n, g);
        return;
    }
#endif
    for (i = 0; i < n; i++) {
        p->frac += p->step;
        while (p->frac >= 0x10000) {
            p->frac -= 0x10000;
            if (!next_sample(p)) {
                return;
            }
        }
        for (c = 0; c < 2; c++) {
            int32_t s = p->out[c] * g >> 15;
            buf[i * 2 + c] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
    }
}

#ifdef __3DS__
/* the game thread's: a file that ended closed */
static void settle(ADXT p) {
    int ended;
    FILL_LOCK();
    ended = fill.ended;
    fill.ended = 0;
    FILL_UNLOCK();
    if (ended) {
        close_files(p);
    }
}

static void adx_mix_locked(int16_t* buf, int n) {
    ahead_report();
    settle(&player);
    FILL_LOCK();
    if (!fill.threaded) {
        adx_mix(buf, n);
    }
    FILL_UNLOCK();
    settle(&player);
}

/* the sound thread's: the music for the game's frame, with MVC2_ADX_THREAD, in place of what the game's thread
   mixed; a file not read ahead is still mixed there */
static void adx_mix_late(int16_t* buf, int n) {
    FILL_LOCK();
    if (fill.threaded) {
        memset(buf, 0, (size_t)n * 4);
        fill.sound = 1;
        adx_mix(buf, n);
        fill.sound = 0;
    }
    FILL_UNLOCK();
}

/* the sound thread's: the music for a frame between the game's; 0 when it can't be had from what is read already */
static int adx_extra(int16_t* buf, int n) {
    static Sint32 x_vol = 0;
    static int32_t x_g = 32768;
    ADXT p = &player;
    int ok = 1;
    if (LightLock_TryLock(&fill.lock) != 0) {
        __atomic_fetch_add(&fill.busy, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (p->stat == ADXT_STAT_PLAYING && !p->pause) {
        if (!ah.on || p->step != 0x10000 || p->frac != 0 || !ahead_ready(p, (uint32_t)n)) {
            fill.unready++;
            ok = 0;
        } else {
            if (p->vol != x_vol) {
                x_vol = p->vol;
                x_g = (int32_t)(32768.0 * pow(10.0, x_vol / 200.0));
            }
            fill.extra = 1;
            mix_same_rate(p, buf, n, x_g);
            fill.extra = 0;
        }
    }
    fill.made += (uint32_t)ok;
    LightLock_Unlock(&fill.lock);
    return ok;
}
#endif

/* reads the header: the format, the predictor's coefficients from the cutoff, the loop */
static int open_header(ADXT p) {
    const uint8_t* h = bytes_at(p, 0, GDD_SCT_SIZE);
    double z, a, b, c;
    if (be16(h) != 0x8000 || h[4] != 3 || h[5] != ADX_BLOCK || h[6] != 4 || h[7] < 1 || h[7] > 2) {
        return 0;
    }
    p->data = be16(h + 2) + 4;
    p->ch = h[7];
    p->rate = be32(h + 8);
    p->total = be32(h + 12);
    z = cos(2.0 * M_PI * be16(h + 16) / p->rate);
    a = M_SQRT2 - z;
    b = M_SQRT2 - 1.0;
    c = (a - sqrt((a + b) * (a - b))) / b;
    p->coef1 = (int32_t)floor(c * 8192.0);
    p->coef2 = (int32_t)floor(c * c * -4096.0);
    p->loop = h[0x12] == 3 && be32(h + 0x18) != 0;
    p->loop_start = be32(h + 0x1C);
    p->loop_end = be32(h + 0x24);
    if (p->loop && (p->loop_end > p->total || p->loop_start >= p->loop_end)) {
        p->loop = 0;
    }
    p->step = (uint32_t)(((uint64_t)p->rate << 16) / SND_RATE);
    return 1;
}

void ADXT_Init(void) {
#ifdef __3DS__
    static int once;
    if (!once) {
        once = 1;
        fill.between = getenv("MVC2_ADX_FILL") != NULL && atoi(getenv("MVC2_ADX_FILL")) != 0;
        fill.thread = getenv("MVC2_ADX_THREAD") != NULL && atoi(getenv("MVC2_ADX_THREAD")) != 0;
        if (fill.between || fill.thread) {
            LightLock_Init(&fill.lock);
            fill.on = 1;
        }
        if (fill.between) {
            host_log("adx: the music mixed between the game's frames too, from what is read ahead (MVC2_ADX_FILL)");
        }
        if (fill.thread) {
            host_log("adx: the music read ahead decoded on the sound thread (MVC2_ADX_THREAD)");
        }
    }
    snd_set_mixer(adx_mix_locked);
    snd_set_late_mixer(fill.thread ? adx_mix_late : NULL);
    if (fill.between) {
        snd_set_extra_mixer(adx_extra);
    }
#else
    snd_set_mixer(adx_mix);
#endif
}

void ADXT_Finish(void) {
    halt(&player);
}

ADXT ADXT_Create(Sint32 maxnch, void* work, Sint32 worksize) {
    (void)maxnch;
    (void)work;
    (void)worksize;
    return &player;
}

void ADXT_SetAutoRcvr(ADXT adxt, Sint32 rmode) {
    (void)adxt;
    (void)rmode;
}

void ADXT_ExecServer(void) {
}

Sint32 ADXT_GetStat(ADXT adxt) {
#ifdef __3DS__
    /* without the lock: the game's waits ask it a few million times */
    return adxt != NULL ? __atomic_load_n(&adxt->stat, __ATOMIC_ACQUIRE) : ADXT_STAT_STOP;
#else
    return adxt != NULL ? adxt->stat : ADXT_STAT_STOP;
#endif
}

Sint32 ADXT_IsIbufSafety(ADXT adxt) {
    (void)adxt;
    return 1;
}

void ADXT_Pause(ADXT adxt, Sint32 sw) {
    if (adxt != NULL) {
        FILL_LOCK();
        adxt->pause = sw;
        FILL_UNLOCK();
    }
}

/* the file opened and its header read with the player stopped, so with the sound thread off it */
void ADXT_StartFname(ADXT adxt, char* fname) {
    if (adxt == NULL) {
        return;
    }
    halt(adxt);
    if (!snd_enabled() || (adxt->gd = gdFsOpen(fname, NULL)) == NULL) {
        return;
    }
    adxt->buf_off = adxt->buf_len = 0;
#ifdef __vita__
    adxt->head = dc_gd_head(fname, &adxt->head_len);
#endif
    if (!open_header(adxt)) {
        host_log("adx: %s is not a 4-bit ADX file", fname);
        halt(adxt);
        return;
    }
    memset(adxt->hist, 0, sizeof adxt->hist);
    adxt->sample = 0;
    adxt->dec_left = 0;
    adxt->frac = 0;
    adxt->out[0] = adxt->out[1] = 0;
    adxt->pause = 0;
#ifdef __3DS__
    ahead_start(adxt, fname);
#endif
#ifdef __vita__
    {
        Sint32 fsize = 0;
        if (gdFsGetFileSize(adxt->gd, &fsize) && fsize > 0) {
            pre_start(fname, (uint32_t)fsize);
            adxt->pre_on = pre.size > 0;
        } else {
            host_log("adx: %s: no size to read it ahead by", fname);
        }
    }
#endif
    FILL_LOCK();
    adxt->stat = ADXT_STAT_PLAYING;
#ifdef __3DS__
    fill.threaded = fill.thread && ah.on;
#endif
    FILL_UNLOCK();
    if (getenv("MVC2_SNDLOG")) {
        host_log("adx: %s, %u Hz, %d channels, %u samples, loop %d %u-%u (frame %u)", fname, adxt->rate, adxt->ch,
                 adxt->total, adxt->loop, adxt->loop_start, adxt->loop_end, host_frame());
    }
}

void ADXT_Stop(ADXT adxt) {
    if (adxt != NULL) {
        FILL_LOCK();
        if (adxt->stat == ADXT_STAT_PLAYING && getenv("MVC2_SNDLOG")) {
            host_log("adx: stopped at sample %u (frame %u)", adxt->sample, host_frame());
        }
        FILL_UNLOCK();
        halt(adxt);
    }
}

void ADXT_SetOutVol(ADXT adxt, Sint32 vol) {
    if (adxt != NULL) {
        FILL_LOCK();
        adxt->vol = vol;
        FILL_UNLOCK();
    }
}

Sint32 ADXT_GetOutVol(ADXT adxt) {
    return adxt != NULL ? adxt->vol : 0;
}
