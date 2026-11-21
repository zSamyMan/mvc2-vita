/*
 * dc_gd.c  The Dreamcast's disc (Shinobi's gdFs) over a folder of its files, read at once in whole 2048-byte
 * sectors; a request is complete when gdFsGetStat next looks, and the drive's door never opens. On the 3DS with
 * MVC2_GD_ASYNC=1, gdFsReqRd32's reads are done on another thread, and complete after as many frames as a drive
 * would take (MVC2_GD_SEEK, GD_RATE), waited for then if need be: the same frames on every run, whatever the SD card
 * does. With MVC2_GD_AHEAD=1 too, a ...POL.BIN file so read has its ...TEX.BIN read ahead, for the game's gdFsRead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "naomi_host.h"
#include "prof.h"
#include "shinobi.h"

#define GD_MAX_OPEN 16
#define GDD_DRVSTAT_PAUSE 1

struct GDS_FS_HANDLE {
    FILE* f;
    Sint32 size;
    long pos;           /* where the next sector is read from */
    Sint32 stat;
#ifdef __3DS__
    char name[32];
#endif
#ifdef __vita__
    char* first;        /* the file read from its start into first, on to next */
    char* next;
    char vname[32];     /* a lazily opened file's name (f is LAZY until it is opened) */
    const uint8_t* mem; /* a file read into memory: its bytes, copied from here (fmemopen's reads went in pieces) */
#endif
};

static char root[1024];
static struct GDS_FS_HANDLE files[GD_MAX_OPEN];
static struct GDS_FS_HANDLE sys_handle = { NULL, 0, 0, GDD_STAT_COMPLETE };

/* the USA disc is told by its program's size and a few of the files it reads first */
static const struct {
    const char* name;
    long size;
} disc_files[] = {
    { "1ST_READ.BIN", 1809817 }, { "AICADRV.BIN", 59264 },   { "FONT.BIN", 73792 },     { "SELTEX.BIN", 1591990 },
    { "PL00_DAT.BIN", 557408 },  { "PL3A_DAT.BIN", 464448 }, { "STG00POL.BIN", 173976 }, { "ADX_SELC.BIN", 417792 },
};

/* the first of those files that is missing from dir or the wrong size, or NULL when all are there, or when
   there is an image of the disc to unpack them from */
const char* dc_disc_check(const char* dir) {
    const char* missing = dc_disc_missing(dir);
    return missing != NULL && dc_image_find(dir) != NULL ? NULL : missing;
}

const char* dc_disc_missing(const char* dir) {
    char path[1200];
    struct stat st;
    size_t i;
    for (i = 0; i < sizeof disc_files / sizeof disc_files[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, disc_files[i].name);
        if (stat(path, &st) != 0 || st.st_size != disc_files[i].size) {
            return disc_files[i].name;
        }
    }
    return NULL;
}

#ifdef __vita__
static void cache_start(void);
#endif

int dc_gd_open(const char* disc_path) {
    snprintf(root, sizeof root, "%s", disc_path);
#ifdef __vita__
    cache_start();
#endif
    return 1;
}

#ifdef __3DS__
#include <3ds.h>

/* MVC2_GD_NOBUF=1: the files read without stdio's buffer, each read one call to the SD card's service */
static int no_buf = -1;
static int trace = -1;

static uint64_t read_begin(void) {
    return svcGetSystemTick();
}

/* with MVC2_TRACE_GD: a read's time */
static void read_end(const char* what, uint64_t t0, size_t bytes) {
    if (trace) {
        host_log("gd %s %u KB in %.1f ms (frame %u)", what, (unsigned)(bytes >> 10),
                 (double)(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11, host_frame());
    }
}

/* MVC2_GD_ASYNC=1: gdFsReqRd32's reads done on ctr_stream.c's reader, the handle READ for the frames a drive of
   GD_RATE bytes a second would take, so that the game's loader (am_load.c) keeps its frames going as a file is read */
#define GD_RATE (4u << 20)
static unsigned gd_seek;    /* frames before a read's first byte, MVC2_GD_SEEK (5) */
static int async_on = -1;
static GDFS async_gd;       /* the handle whose read is on the reader */
static unsigned async_due;  /* the game's frame it is complete in */
static int async_job;       /* 0: taken from the read-ahead at once */

static int gd_async(void) {
    if (async_on < 0) {
        const char* e = getenv("MVC2_GD_SEEK");
        async_on = getenv("MVC2_GD_ASYNC") != NULL && atoi(getenv("MVC2_GD_ASYNC")) != 0;
        gd_seek = e != NULL && atoi(e) >= 0 ? (unsigned)atoi(e) : 5;
        if (async_on) {
            host_log("gd: the game's requested reads done on another thread (MVC2_GD_ASYNC), a read's first byte "
                     "%u frames after it is asked for (MVC2_GD_SEEK), the rest at %u KB a second", gd_seek, GD_RATE >> 10);
        }
    }
    return async_on;
}

static void ahead_after(const char* name);
static void next_after(const char* name, int sync);

/* the reader's read finished, waited for if need be */
static void async_end(void) {
    PROF_START(t0);
    float ms = 0.0f;
    int waited = async_job && ctr_read_busy();
    size_t got = async_job ? ctr_read_end(&ms) : 0;
    host_fills++;
    async_gd->stat = GDD_STAT_COMPLETE;
    if (trace) {
        if (async_job) {
            host_log("gd read %u KB in %.1f ms on the reader (frame %u%s)", (unsigned)(got >> 10), (double)ms,
                     host_frame(), waited ? ", waited for" : "");
        }
    }
    ahead_after(async_gd->name);
    next_after(async_gd->name, 0);
    async_gd = NULL;
    PROF_STOP(PZ_GD_READ, t0);
}

#define async_wait(gd) ((gd) == async_gd && (gd) != NULL ? async_end() : (void)0)

/* MVC2_GD_AHEAD=1: the files opened lately, and the texture file read ahead after its models' */
#define AHEAD_MOST 0x1A0000
static int ahead_on = -1;
static char opened[16][32];
static unsigned opened_n;

static int gd_ahead(void) {
    if (ahead_on < 0) {
        ahead_on = getenv("MVC2_GD_AHEAD") != NULL && atoi(getenv("MVC2_GD_AHEAD")) != 0;
        if (ahead_on) {
            host_log("gd: a models file's textures read ahead (MVC2_GD_AHEAD)");
        }
    }
    return ahead_on;
}

static void ahead_after(const char* name) {
    size_t n = strlen(name);
    char tex[32];
    unsigned i;
    if (!gd_ahead() || n < 7 || n >= sizeof tex || strcmp(name + n - 7, "POL.BIN") != 0) {
        return;
    }
    memcpy(tex, name, n - 7);
    strcpy(tex + n - 7, "TEX.BIN");
    for (i = 0; i < 16; i++) {
        if (strcmp(opened[i], tex) == 0) {
            return;                     /* read lately already */
        }
    }
    if (ctr_ahead_start(dc_gd_path(tex), AHEAD_MOST) && trace) {
        host_log("gd %s read ahead (frame %u)", tex, host_frame());
    }
}

/* MVC2_GD_NEXT=1: the textures the menus read whole with gdFsRead, in the frame they are wanted, read ahead after
   the file the game reads before them, when that one came from the reader or the read-ahead (there is time then)
   and nothing else is read ahead: the main menu's after the title's sounds, the mode select's after the main menu's.
   And the main menu's again once the opening's music is read, in place of anything read ahead: the title follows */
static const struct {
    const char *after, *next;
    int first;          /* in place of what is read ahead, after a read here too */
} next_files[] = {
    { "SE_SYUK.BIN", "DM07TEX.BIN", 0 }, { "DM07TEX.BIN", "DM0CTEX.BIN", 0 }, { "ADX_OPEN.BIN", "DM07TEX.BIN", 1 },
};
static int next_on = -1;

/* after name is read: on the reader or from the read-ahead, else here (sync) */
static void next_after(const char* name, int sync) {
    unsigned i;
    if (next_on < 0) {
        next_on = gd_ahead() && getenv("MVC2_GD_NEXT") != NULL && atoi(getenv("MVC2_GD_NEXT")) != 0;
        if (next_on) {
            host_log("gd: the menus' textures read ahead after the files before them (MVC2_GD_NEXT)");
        }
    }
    if (!next_on) {
        return;
    }
    for (i = 0; i < sizeof next_files / sizeof next_files[0]; i++) {
        if (strcmp(name, next_files[i].after) != 0 || (sync && !next_files[i].first) ||
            (!next_files[i].first && !ctr_ahead_idle()) || ctr_ahead_has(dc_gd_path(next_files[i].next))) {
            continue;
        }
        if (ctr_ahead_start(dc_gd_path(next_files[i].next), AHEAD_MOST) && trace) {
            host_log("gd %s read ahead after %s (frame %u)", next_files[i].next, name, host_frame());
        }
    }
}

/* the file's sectors from the read-ahead, if they are there (and read already, when done) */
static int ahead_take(GDFS gd, Sint32 nsct, void* buf, int done) {
    size_t want = (size_t)nsct * GDD_SCT_SIZE;
    float ms;
    uint64_t t0;
    if (ahead_on <= 0 || ctr_ahead_has(dc_gd_path(gd->name)) < 1 + done) {
        return 0;
    }
    t0 = svcGetSystemTick();
    if (!ctr_ahead_take((size_t)gd->pos, buf, want, &ms)) {
        PROF_ADD(PZ_GD_READ, svcGetSystemTick() - t0);
        return 0;
    }
    host_fills++;
    gd->pos += (long)want;
    gd->stat = GDD_STAT_COMPLETE;
    if (trace) {
        host_log("gd %u KB taken from the read-ahead (read in %.1f ms), %.1f ms here (frame %u)",
                 (unsigned)(want >> 10), (double)ms, (double)(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11,
                 host_frame());
    }
    next_after(gd->name, 0);
    PROF_ADD(PZ_GD_READ, svcGetSystemTick() - t0);
    return 1;
}

/* MVC2_GD_KEEP=1: the files the game closes kept open, the last KEEP_MOST, for when it opens one again; the one
   pushed out closed on the reader. A file read ahead is opened as the reader left it */
#define KEEP_MOST 8
static int keep_on = -1;
static struct {
    char name[32];
    FILE* f;
    Sint32 size;
    unsigned used;
} kept[KEEP_MOST];
static unsigned keep_clock, keep_hits, keep_opens;

static FILE* keep_take(const char* name, Sint32* size) {
    unsigned i;
    if (keep_on < 0) {
        keep_on = getenv("MVC2_GD_KEEP") != NULL && atoi(getenv("MVC2_GD_KEEP")) != 0;
        if (keep_on) {
            host_log("gd: the last %d files closed kept open for the game's next opens (MVC2_GD_KEEP)", KEEP_MOST);
        }
    }
    if (!keep_on) {
        return NULL;
    }
    keep_opens++;
    for (i = 0; i < KEEP_MOST; i++) {
        if (kept[i].f != NULL && strcmp(kept[i].name, name) == 0) {
            FILE* f = kept[i].f;
            kept[i].f = NULL;
            *size = kept[i].size;
            keep_hits++;
            if (trace) {
                host_log("gd %s open still (%u of %u opens so)", name, keep_hits, keep_opens);
            }
            return f;
        }
    }
    return NULL;
}

/* 1 when gd's file is kept */
static int keep_put(GDFS gd) {
    unsigned i, at = 0;
    if (keep_on <= 0) {
        return 0;
    }
    for (i = 0; i < KEEP_MOST; i++) {
        if (kept[i].f == NULL) {
            at = i;
            break;
        }
        if (kept[i].used < kept[at].used) {
            at = i;
        }
    }
    if (kept[at].f != NULL) {
        ctr_close_later(kept[at].f);
    }
    snprintf(kept[at].name, sizeof kept[at].name, "%s", gd->name);
    kept[at].f = gd->f;
    kept[at].size = gd->size;
    kept[at].used = ++keep_clock;
    return 1;
}

/* MVC2_GD_PROGS=1: the six players' programs (S_PLxxA to S_PLxxF), read at once as a fight begins, opened and read on
   the reader once their names are known: the players' voices (PLxx_VOI) come just before, the six in the players'
   order when no character is there twice */
static int progs_on = -1;
static char voices[6][3];
static unsigned nvoices;

static void progs_after_open(const char* name) {
    unsigned k;
    if (progs_on < 0) {
        progs_on = getenv("MVC2_GD_PROGS") != NULL && atoi(getenv("MVC2_GD_PROGS")) != 0;
        if (progs_on) {
            host_log("gd: the players' programs read ahead once their voices are read (MVC2_GD_PROGS)");
        }
    }
    if (!progs_on) {
        return;
    }
    if (strncmp(name, "PL", 2) != 0 || strlen(name) != 12 || strcmp(name + 4, "_VOI.BIN") != 0) {
        nvoices = 0;
        return;
    }
    if (nvoices == 0) {
        ctr_small_drop();
    }
    if (nvoices < 6) {
        memcpy(voices[nvoices], name + 2, 2);
        voices[nvoices++][2] = 0;
    }
    if (nvoices == 6) {
        for (k = 0; k < 6; k++) {
            char prog[32];
            snprintf(prog, sizeof prog, "S_PL%s%c.BIN", voices[k], (char)('A' + k));
            if (ctr_small_start(dc_gd_path(prog), 64 << 10) && trace) {
                host_log("gd %s read ahead (frame %u)", prog, host_frame());
            }
        }
        nvoices = 0;
    }
}

static void keep_close_all(void) {
    unsigned i;
    for (i = 0; i < KEEP_MOST; i++) {
        if (kept[i].f != NULL) {
            fclose(kept[i].f);
            kept[i].f = NULL;
        }
    }
}
#elif defined(__vita__)
#include <psp2/kernel/processmgr.h>

/* the disc's files on the memory card: each open and read timed, and a frame whose took more than 20 ms said in the
   log (MVC2_TRACE_GD=1: every one) */
static SceUID vreader = 0, cache_thread;    /* the reader's and the cache's threads (named in the trace) */
static int trace = -1;
static unsigned gd_frame, gd_reads, gd_opens;
static char gd_names[64];      /* the files opened in the frame, the first few */
static uint64_t gd_us, gd_open_us, gd_bytes;

static uint64_t read_begin(void) {
    return sceKernelGetProcessTimeWide();
}

static void gd_account(const char* what, uint64_t t0, size_t bytes, int open) {
    uint64_t us = sceKernelGetProcessTimeWide() - t0;
    if (trace < 0) {
        trace = getenv("MVC2_TRACE_GD") != NULL;
    }
    if (host_frame() != gd_frame) {
        if (gd_us + gd_open_us > 20000) {
            host_log("gd: frame %u: %u reads, %u KB in %.1f ms; %u opens in %.1f ms (%s)", gd_frame, gd_reads,
                     (unsigned)(gd_bytes >> 10), gd_us / 1000.0, gd_opens, gd_open_us / 1000.0, gd_names);
        }
        gd_frame = host_frame();
        gd_us = gd_open_us = gd_bytes = 0;
        gd_reads = gd_opens = 0;
        gd_names[0] = 0;
    }
    if (open) {
        size_t at = strlen(gd_names);
        if (at < sizeof gd_names - 16) {
            snprintf(gd_names + at, sizeof gd_names - at, "%s%s", at ? " " : "", what);
        }
        gd_opens++;
        gd_open_us += us;
    } else {
        gd_reads++;
        gd_us += us;
        gd_bytes += bytes;
    }
    if (trace) {
        SceUID me = sceKernelGetThreadId();
        host_log("gd %s %u KB in %.1f ms (frame %u, %s)", what, (unsigned)(bytes >> 10), us / 1000.0, host_frame(),
                 me == vreader ? "reader" : me == cache_thread ? "cache" : "game");
    }
}

#define read_end(what, t0, bytes) gd_account(what, t0, bytes, 0)

/* the game's requested reads (gdFsReqRd32: am_load.c's, the fighters' data) done by a thread on core 2, the handle
   READ until it is done, so that the game keeps its frames going as it would on the console's drive */
#include <psp2/kernel/threadmgr.h>
static SceUID vreq;
static GDFS volatile vjob_gd;
static volatile Sint32 vjob_nsct;
static void* volatile vjob_buf;
static volatile int vjob_busy;
static void read_sectors(GDFS gd, Sint32 nsct, void* buf);

static int vreader_main(SceSize args, void* argp) {
    (void)args, (void)argp;
    for (;;) {
        sceKernelWaitSema(vreq, 1, NULL);
        read_sectors(vjob_gd, vjob_nsct, vjob_buf);
        vjob_busy = 0;
    }
    return 0;
}

static int vreader_start(void) {
    if (vreader == 0) {
        vreq = sceKernelCreateSema("gd_req", 0, 0, 1, NULL);
        vreader = sceKernelCreateThread("gd_reader", vreader_main, 165, 32 * 1024, 0, 0x00040000, NULL);
        {
            extern void rd_vgl_watch_thread(const char* name, int id) __attribute__((weak));
            if (rd_vgl_watch_thread && vreader >= 0) {
                rd_vgl_watch_thread("RDR", vreader);
            }
        }
        if (vreader < 0 || sceKernelStartThread(vreader, 0, NULL) < 0) {
            vreader = -1;
        }
    }
    return vreader > 0;
}

static void async_wait(GDFS gd) {
    uint64_t t0 = 0;
    while (vjob_busy && vjob_gd == gd) {
        if (t0 == 0) {
            t0 = sceKernelGetProcessTimeWide();
        }
        sceKernelDelayThread(500);
    }
    if (t0 != 0 && sceKernelGetProcessTimeWide() - t0 > 20000) {
        host_log("gd: the game waited %.1f ms for the reader's read of %s (frame %u)",
                 (sceKernelGetProcessTimeWide() - t0) / 1000.0, gd->vname[0] ? gd->vname : "a file", host_frame());
    }
}

/*
 * The disc's files in memory. The game reads its screens', menus' and stages' files whole and waits for them (a
 * title or a select screen held up 200 to 450 ms by the memory card). So a thread on core 2 reads them into a
 * block of memory while the intro plays: the screens' and menus' files first, the stages' last; the fighters' (PL,
 * read by am_load.c on its own while the game goes on) and the music (ADX, read ahead by dc_adx.c) are left out. An
 * open of a file read in is a stream over its bytes (fmemopen), so the rest of this file reads it as any other.
 * MVC2_GD_CACHE=0: none; =MB: at most that many.
 */
#include <psp2/io/dirent.h>
#include <psp2/kernel/sysmem.h>
#define CACHE_FILES 768
static struct {
    char name[24];
    uint32_t off, size;
    uint8_t heap;               /* in the heap's block (a fighter's data, PLxx_DAT), else the memory block's */
    uint8_t* mem;               /* its bytes, once the blocks are had */
    volatile int ready;
    uint8_t late;               /* read after the rest, into the late block (late_main) */
    uint64_t opened;            /* when the game last opened it (a late one is not dropped soon after) */
} cached[CACHE_FILES];
static uint8_t* cache_heap;
/* a fighter or a stage the game just asked a file of: the cache reads its other files next (a fighter's sounds,
   voices and face, a stage's textures), which the versus loads one after another; boost_kind 1 a fighter (boost_id
   its id, "0C"), 2 a stage ("01") */
static char boost_id[3];
static volatile int boost_kind;
static volatile unsigned boost_seq;

static void cache_boost(const char* name) {
    int kind = 0;
    if (strncmp(name, "PL", 2) == 0 && name[2] && name[3] && name[4] == '_') {
        kind = 1, boost_id[0] = name[2], boost_id[1] = name[3];
    } else if (strncmp(name, "STG", 3) == 0 && name[3] && name[4]) {
        kind = 2, boost_id[0] = name[3], boost_id[1] = name[4];
    }
    if (kind) {
        boost_id[2] = 0;
        boost_kind = kind;
        __sync_synchronize();
        boost_seq++;
    }
}     /* the fighters' data: the game's heap, half of it idle (58 of 114 MB at most) */
static int ncached;
static uint8_t* cache_mem;
static uint32_t cache_size;

/* every file the listing saw, with its size: an open of one not in memory needs no card access then (an open
   takes 20 to 150 ms on the memory card, the game's thread waiting in gdFsOpen): the file is opened by the
   reader with its first requested read, or by the first read here. MVC2_GD_LAZY=0: opened in gdFsOpen */
#define LISTED_MOST 1024
static struct {
    char name[24];
    uint32_t size;
} listed[LISTED_MOST];
static volatile int nlisted;
static FILE lazy_mark;
#define LAZY (&lazy_mark)

static int listed_size(const char* name, Sint32* size) {
    int i, n = nlisted;
    static int on = -1;
    if (on < 0) {
        on = getenv("MVC2_GD_LAZY") == NULL || atoi(getenv("MVC2_GD_LAZY")) != 0;
    }
    for (i = 0; on && i < n; i++) {
        if (strcmp(listed[i].name, name) == 0) {
            *size = (Sint32)listed[i].size;
            return 1;
        }
    }
    return 0;
}

/* the game's own card accesses (its opens and reads, here or on the reader) ahead of the threads that read for later
   (the files into memory, the songs' heads): those wait while one is under way and for FG_QUIET after, and read
   in BG_PIECE pieces, so that a game's open never waits behind seconds of their reading on a slow memory card */
#define FG_QUIET 40000      /* us */
#define BG_PIECE (256u << 10)
static volatile int fg_n;
static volatile uint64_t fg_t;
static uint64_t bg_waited;

static void fg_begin(void) {
    __sync_fetch_and_add(&fg_n, 1);
}

static void fg_end(void) {
    fg_t = sceKernelGetProcessTimeWide();
    __sync_synchronize();
    __sync_fetch_and_sub(&fg_n, 1);
}

static void bg_wait(void) {
    uint64_t t0 = 0;
    while (fg_n > 0 || sceKernelGetProcessTimeWide() - fg_t < FG_QUIET) {
        if (t0 == 0) {
            t0 = sceKernelGetProcessTimeWide();
        }
        sceKernelDelayThread(5000);
    }
    if (t0 != 0) {
        bg_waited += sceKernelGetProcessTimeWide() - t0;
    }
}

/* a file whole into dst, in pieces, giving way to the game's accesses before each */
static int bg_read(FILE* f, uint8_t* dst, uint32_t size) {
    uint32_t done = 0;
    while (done < size) {
        uint32_t n = size - done < BG_PIECE ? size - done : BG_PIECE;
        bg_wait();
        if (fread(dst + done, 1, n, f) != n) {
            return 0;
        }
        done += n;
    }
    return 1;
}

/* the music files' first HEAD_BYTES, read before the rest (each open costs the card 20 to 150 ms, and the game's
   thread opened a song and read its start as it began playing): its header and the song's first second or so
   from memory, while dc_adx.c's reader reads the whole file */
#define HEAD_BYTES (256u << 10)   /* some 5 s of a song: dc_adx.c's reader is well ahead by then */
#define HEADS_MOST 96
static struct {
    char name[24];
    uint8_t* data;
    uint32_t len;
} heads[HEADS_MOST];
static volatile int nheads;

const uint8_t* dc_gd_head(const char* name, uint32_t* len) {
    int i, n = nheads;
    for (i = 0; i < n; i++) {
        if (strcmp(heads[i].name, name) == 0) {
            *len = heads[i].len;
            return heads[i].data;
        }
    }
    return NULL;
}

/* one song's first HEAD_BYTES into memory, giving way to the game's reads; 1 when read */
static int read_head(const char* name, uint32_t size) {
    char path[1200];
    FILE* f;
    uint32_t want = size < HEAD_BYTES ? size : HEAD_BYTES;
    uint8_t* d;
    int ok;
    if (nheads >= HEADS_MOST || size == 0 || (d = malloc(want)) == NULL) {
        return 0;
    }
    snprintf(path, sizeof path, "%s/%s", root, name);
    bg_wait();
    if ((f = fopen(path, "rb")) == NULL) {
        free(d);
        return 0;
    }
    setvbuf(f, NULL, _IONBF, 0);
    ok = bg_read(f, d, want);
    fclose(f);
    if (!ok) {
        free(d);
        return 0;
    }
    snprintf(heads[nheads].name, sizeof heads[0].name, "%s", name);
    heads[nheads].data = d;
    heads[nheads].len = want;
    __sync_synchronize();
    nheads++;
    return 1;
}

/* the files in the order the game first asks for them (the attract's, then the arcade's first fight): read into
   memory first. On a memory card that answers each request in 0.5 s or more, a file still on the card when the
   game wants it held a frame up by a second or two; read in this order the cache is ahead of it */
static const char* const use_order[] = {
    "DM11POL.BIN", "DM11TEX.BIN", "DM09TEX.BIN", "DM09POL.BIN", "ADX_CAPL.BIN", "DM08TEX.BIN", "DM08POL.BIN",
    "DM08CAB.BIN", "DM08CHR.BIN", "SE_SYUK.BIN", "DM07TEX.BIN", "ADX_OPEN.BIN", "DM07POL.BIN", "STG00TEX.BIN",
    "EFKYPOL.BIN", "EFKYTEX.BIN", "STG00POL.BIN", "ADX_S000.BIN", "DM06POL.BIN", "DM06TEX.BIN", "ADX_RANK.BIN",
    /* the menus' and the select's, as a game starts from the title (the game read each on its own thread: two to
       six seconds held up on a slow card entering the menu and the select) */
    "DM0CPOL.BIN", "DM0CTEX.BIN", "SELTEX.BIN", "ADX_MENU.BIN", "DM01POL.BIN", "DM0FPOL.BIN", "DM0FTEX.BIN",
    "DM01TEX.BIN", "SELVMU.BIN", "DM02TEX.BIN", "DM02POL.BIN", "DM00TEX.BIN", "DM00POL.BIN", "ADX_SELC.BIN",
    /* the start's: the game reads them itself as it starts, before the cache could, so they come after */
    "AICADRV.BIN", "SE_COMN.BIN", "DM04TEX.BIN", "DM04POL.BIN", "FONT.BIN", "DEBUG.BIN", "VS4.BIN", "ERR_MES.BIN",
};

static int cache_rank(const char* n) {
    static const char* const first[] = { "DM", "S_", "SE", "EN", "2_", "S2", "S1", "AS" };
    int i;
    if (strncmp(n, "PL", 2) == 0) {
        return 10;      /* the fighters' voices, faces and win poses: after the screens', before the stages' */
    }
    if (strncmp(n, "ST", 2) == 0) {
        return 90;
    }
    for (i = 0; i < (int)(sizeof first / sizeof first[0]); i++) {
        if (strncmp(n, first[i], 2) == 0) {
            return i;
        }
    }
    return 50;
}

/* the attract's four demos (s_play.c's demo_pl_tbl, the Dreamcast's): each one's six fighters, on stage d */
static const uint8_t demo_fighters[4][6] = {
    { 0x02, 0x00, 0x01, 0x04, 0x22, 0x05 }, { 0x15, 0x14, 0x13, 0x12, 0x10, 0x11 },
    { 0x06, 0x16, 0x17, 0x0b, 0x0f, 0x0a }, { 0x07, 0x08, 0x09, 0x0c, 0x0e, 0x0d },
};

/* a file's place in the reading: its place in use_order; then each demo's fighters (their data, voices and win
   poses) and stage, demo by demo, as the attract plays them; then the rest by cache_rank (a song's head with them) */
static int read_rank(const char* n) {
    int i, d;
    unsigned id;
    char kind[4];
    for (i = 0; i < (int)(sizeof use_order / sizeof use_order[0]); i++) {
        if (strcmp(n, use_order[i]) == 0) {
            return i;
        }
    }
    /* demo 0's set at 100, then every fighter's data (120), the songs' heads (130), demos 1 to 3 (200 on), the rest */
    if (sscanf(n, "PL%2X_%3s", &id, kind) == 2) {
        for (d = 0; d < 4; d++) {
            for (i = 0; i < 6; i++) {
                if (demo_fighters[d][i] == id) {
                    return (d ? 200 + (d - 1) * 20 : 100) + i * 2 + (strcmp(kind, "DAT") != 0);
                }
            }
        }
        if (strcmp(kind, "DAT") == 0) {
            return 120;     /* any fighter's data: the select shows them, the versus loads them */
        }
        if (strcmp(kind, "VOI") == 0) {
            return 125;     /* and voices, which the versus loads with them */
        }
    }
    if (sscanf(n, "S_PL%2X", &id) == 1) {   /* a fighter's sounds, one of six places (S_PL02A...): with the fighter */
        for (d = 0; d < 4; d++) {
            for (i = 0; i < 6; i++) {
                if (demo_fighters[d][i] == id) {
                    return (d ? 200 + (d - 1) * 20 : 100) + 12 + (i & 1);
                }
            }
        }
        return 125;
    }
    if (sscanf(n, "STG%2X", &id) == 1 && id < 8) {
        return (id % 4 ? 200 + (int)(id % 4 - 1) * 20 : 100) + 14 + (int)(id / 4);
    }
    if (strncmp(n, "ADX", 3) == 0) {
        return 130;         /* the songs' heads: a song's start read on the game's thread without one */
    }
    return 400 + cache_rank(n);
}

static int cache_cmp(const void* a, const void* b) {
    const char *x = ((const char*)a), *y = ((const char*)b);
    int rx = read_rank(x), ry = read_rank(y);
    return rx != ry ? rx - ry : strcmp(x, y);
}

/* the disc folder's listing kept in one file (_index.bin: "MVC2IDX1", the count, then name and size of each): a
   listing asks the card once a file, 18 to 36 s on a slow card before anything could be read; this, once */
#define INDEX_NAME "_index.bin"
static int index_load(void) {
    char path[1200];
    FILE* f;
    char magic[8];
    uint32_t n = 0, i;
    snprintf(path, sizeof path, "%s/" INDEX_NAME, root);
    if ((f = fopen(path, "rb")) == NULL) {
        return 0;
    }
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "MVC2IDX1", 8) != 0 || fread(&n, 4, 1, f) != 1 || n == 0 ||
        n > LISTED_MOST) {
        fclose(f);
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (fread(listed[i].name, 1, sizeof listed[0].name, f) != sizeof listed[0].name ||
            fread(&listed[i].size, 4, 1, f) != 1) {
            fclose(f);
            return 0;
        }
        listed[i].name[sizeof listed[0].name - 1] = 0;
    }
    fclose(f);
    __sync_synchronize();
    nlisted = (int)n;
    return 1;
}

static void index_save(void) {
    char path[1200];
    FILE* f;
    uint32_t n = (uint32_t)nlisted;
    int i;
    snprintf(path, sizeof path, "%s/" INDEX_NAME, root);
    if ((f = fopen(path, "wb")) == NULL) {
        return;
    }
    fwrite("MVC2IDX1", 1, 8, f);
    fwrite(&n, 4, 1, f);
    for (i = 0; i < nlisted; i++) {
        fwrite(listed[i].name, 1, sizeof listed[0].name, f);
        fwrite(&listed[i].size, 4, 1, f);
    }
    fclose(f);
}

/* one file of the list into its memory, giving way to the game's reads; 1 when read */
static int cache_read(int i, uint32_t* done) {
    char path[1200];
    FILE* f;
    int ok = 0;
    snprintf(path, sizeof path, "%s/%s", root, cached[i].name);
    bg_wait();
    if ((f = fopen(path, "rb")) == NULL) {
        return 0;
    }
    setvbuf(f, NULL, _IONBF, 0);
    if (bg_read(f, cached[i].mem, cached[i].size)) {
        __sync_synchronize();
        cached[i].ready = 1;
        *done += cached[i].size;
        ok = 1;
    }
    fclose(f);
    return ok;
}

/*
 * After the list is read: the files left out of it (over the budget: the last stage's, the bosses', a fighter's
 * sounds) of a stage or a fighter the game has just opened a file of, read while the versus plays into a block of
 * their own, so the fight's start does not wait for the card (the last stage's textures held it up 2 s). The oldest
 * make room, unless the game opened them in the last LATE_KEEP_US. MVC2_GD_LATE=0: none; =MB: the block's size.
 */
#define LATE_FILE_MOST (2u << 20)
#define LATE_KEEP_US 5000000ull
static uint8_t* late_mem;
static uint32_t late_size, late_pos;

static int late_room(uint32_t sz, uint32_t* off) {
    uint32_t at = late_pos + sz <= late_size ? late_pos : 0;
    uint64_t now = sceKernelGetProcessTimeWide();
    int i;
    if (sz > late_size) {
        return 0;
    }
    for (i = 0; i < ncached; i++) {     /* the late files in the way: none opened just now */
        uint32_t o;
        if (!cached[i].late || cached[i].mem == NULL) {
            continue;
        }
        o = (uint32_t)(cached[i].mem - late_mem);
        if (o < at + sz && at < o + cached[i].size && cached[i].ready && now - cached[i].opened < LATE_KEEP_US) {
            return 0;
        }
    }
    for (i = 0; i < ncached; i++) {
        uint32_t o;
        if (!cached[i].late || cached[i].mem == NULL) {
            continue;
        }
        o = (uint32_t)(cached[i].mem - late_mem);
        if (o < at + sz && at < o + cached[i].size) {
            cached[i].ready = 0;        /* dropped: opened from the card again if asked for */
            __sync_synchronize();
            cached[i].name[0] = 0;
        }
    }
    *off = at;
    late_pos = (at + sz + 63u) & ~63u;
    return 1;
}

static void late_main(void) {
    const char* e = getenv("MVC2_GD_LATE");
    unsigned seen = boost_seq;
    if (e != NULL && atoi(e) <= 0) {
        return;
    }
    late_size = (e != NULL ? (uint32_t)atoi(e) : 6u) << 20;
    for (;;) {
        char a[8], b[8], id[3];
        int kind, j;
        sceKernelDelayThread(20000);
        if (boost_seq == seen) {
            continue;
        }
        seen = boost_seq;
        __sync_synchronize();
        kind = boost_kind;
        memcpy(id, boost_id, sizeof id);
        if (kind == 2) {
            snprintf(a, sizeof a, "STG%s", id);
            snprintf(b, sizeof b, "STG%s", id);
        } else {
            snprintf(a, sizeof a, "PL%s_", id);
            snprintf(b, sizeof b, "S_PL%s", id);
        }
        for (j = 0; j < nlisted && boost_seq == seen; j++) {
            uint64_t t0;
            uint32_t sz = listed[j].size, off, done = 0;
            char path[1200];
            FILE* f;
            int i, have = 0;
            if (sz == 0 || sz > LATE_FILE_MOST ||
                (strncmp(listed[j].name, a, strlen(a)) != 0 && strncmp(listed[j].name, b, strlen(b)) != 0)) {
                continue;
            }
            for (i = 0; i < ncached && !have; i++) {
                have = strcmp(cached[i].name, listed[j].name) == 0;
            }
            if (have || ncached >= CACHE_FILES) {
                continue;
            }
            if (late_mem == NULL) {
                SceUID blk = sceKernelAllocMemBlock("gd_late", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, late_size, NULL);
                if (blk < 0 || sceKernelGetMemBlockBase(blk, (void**)&late_mem) < 0) {
                    host_log("gd: no block for the late files (%08X)", blk);
                    return;
                }
            }
            if (!late_room(sz, &off)) {
                continue;
            }
            t0 = sceKernelGetProcessTimeWide();
            snprintf(path, sizeof path, "%s/%s", root, listed[j].name);
            bg_wait();
            if ((f = fopen(path, "rb")) == NULL) {
                continue;
            }
            setvbuf(f, NULL, _IONBF, 0);
            i = ncached;
            snprintf(cached[i].name, sizeof cached[0].name, "%s", listed[j].name);
            cached[i].size = sz;
            cached[i].off = off;
            cached[i].heap = 0;
            cached[i].late = 1;
            cached[i].opened = 0;
            cached[i].mem = late_mem + off;
            cached[i].ready = 0;
            if (bg_read(f, cached[i].mem, sz)) {
                __sync_synchronize();
                cached[i].ready = 1;
                __sync_synchronize();
                ncached = i + 1;
                done = sz;
            }
            fclose(f);
            host_log("gd: %s read ahead for the fight, %u KB in %.1f ms%s", listed[j].name, sz >> 10,
                     (sceKernelGetProcessTimeWide() - t0) / 1000.0, done ? "" : " (failed)");
        }
    }
}

/* the list (the directory's entries: each one can take milliseconds here, so not on the game's thread), the
   memory, then the files; a file can be opened from memory once its ready is set */
static int cache_main(SceSize args, void* argp) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    const char* e = getenv("MVC2_GD_CACHE");
    uint32_t budget = (e != NULL ? (uint32_t)atoi(e) : 90u) << 20, total = 0, done = 0;
    static char names[CACHE_FILES][24];
    static uint32_t sizes[CACHE_FILES], offs[CACHE_FILES];
    SceIoDirent ent;
    SceUID dir, block = -1;
    int i, n = 0, got = 0;
    (void)args, (void)argp;
    if (!index_load()) {
        if ((dir = sceIoDopen(root)) < 0) {
            host_log("gd: could not list %s (%08X): no files in memory", root, dir);
            return 0;
        }
        while (sceIoDread(dir, &ent) > 0) {
            if (!SCE_S_ISDIR(ent.d_stat.st_mode) && ent.d_name[0] != '_' && strlen(ent.d_name) < sizeof listed[0].name &&
                nlisted < LISTED_MOST) {
                snprintf(listed[nlisted].name, sizeof listed[0].name, "%s", ent.d_name);
                listed[nlisted].size = (uint32_t)ent.d_stat.st_size;
                __sync_synchronize();
                nlisted++;
            }
        }
        sceIoDclose(dir);
        index_save();
        host_log("gd: %d files listed in %.1f s, kept in %s", nlisted, (sceKernelGetProcessTimeWide() - t0) / 1e6,
                 INDEX_NAME);
    }
    for (i = 0; i < nlisted && n < CACHE_FILES; i++) {
        const char* name = listed[i].name;
        /* not the music (read ahead by dc_adx.c: its head only, below); every fighter's data (PLxx_DAT: the select
           and the versus waited half a second to a second for each on a slow card), the demos' in the memory block,
           the others in the heap */
        if (strncmp(name, "ADX", 3) == 0 || listed[i].size == 0 || listed[i].size > (8u << 20)) {
            continue;
        }
        snprintf(cached[n].name, sizeof cached[0].name, "%s", name);
        cached[n].size = listed[i].size;
        n++;
    }
    qsort(cached, (size_t)n, sizeof cached[0], cache_cmp);
    {
        /* the fighters' data outside the demos in the heap, the rest in a memory block of what the system has, by
           rank: what doesn't fit (the last wanted) left out */
        uint32_t heap_total = 0, heap_most = 40u << 20;
        int k = 0, j;
        for (;;) {
            total = heap_total = 0;
            for (j = k = 0; j < n; j++) {
                uint32_t sz = (cached[j].size + 63u) & ~63u;
                int heap = strncmp(cached[j].name, "PL", 2) == 0 && strstr(cached[j].name, "_DAT") != NULL &&
                           read_rank(cached[j].name) == 120;
                if (heap ? heap_total + sz > heap_most : total + sz > budget) {
                    continue;
                }
                cached[j].heap = (uint8_t)heap;
                cached[j].off = heap ? heap_total : total;
                *(heap ? &heap_total : &total) += sz;
                if (j != k) {
                    cached[k] = cached[j];
                }
                k++;
            }
            n = k;
            block = total > 0 ? sceKernelAllocMemBlock("gd_cache", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                                      (total + 0xFFFFFu) & ~0xFFFFFu, NULL) : -1;
            if (block >= 0 || total == 0 || budget < (8u << 20)) {
                break;
            }
            budget = budget / 4 * 3;    /* not that much: a quarter less */
        }
        if (block < 0 || sceKernelGetMemBlockBase(block, (void**)&cache_mem) < 0) {
            host_log("gd: no memory to keep the disc's files in (%08X)", block);
            return 0;
        }
        if (heap_total > 0 && (cache_heap = host_try_malloc(heap_total)) == NULL) {
            host_log("gd: no room in the heap for the fighters' data (%u KB): read from the card", heap_total >> 10);
        }
        for (j = k = 0; j < n; j++) {
            if (cached[j].heap && cache_heap == NULL) {
                continue;
            }
            cached[j].mem = (cached[j].heap ? cache_heap : cache_mem) + cached[j].off;
            if (j != k) {
                cached[k] = cached[j];
            }
            k++;
        }
        n = k;
        host_log("gd: %u KB of the fighters' data in the heap", cache_heap != NULL ? heap_total >> 10 : 0);
    }
    cache_size = total;
    __sync_synchronize();
    ncached = n;        /* the list shown to gdFsOpen, none ready yet */
    host_log("gd: reading %d files, %u KB, into memory, listed in %.1f s (MVC2_GD_CACHE)", n, (unsigned)(total >> 10),
             (sceKernelGetProcessTimeWide() - t0) / 1e6);
    {
        /* the songs' heads in among the files, by the same order (the opening's song before the next scene's
           models, the rest after) */
        static int song[HEADS_MOST];
        int nsong = 0, k = 0, j, heads_got = 0;
        unsigned boost_done = boost_seq;
        for (j = 0; j < nlisted && nsong < HEADS_MOST; j++) {
            if (strncmp(listed[j].name, "ADX", 3) == 0 && listed[j].size > 0) {
                song[nsong++] = j;
            }
        }
        for (j = 1; j < nsong; j++) {   /* by rank (few: an insertion sort) */
            int v = song[j], m = j;
            while (m > 0 && read_rank(listed[song[m - 1]].name) > read_rank(listed[v].name)) {
                song[m] = song[m - 1];
                m--;
            }
            song[m] = v;
        }
        for (i = 0; i < n || k < nsong;) {
            if (k < nsong && (i >= n || read_rank(listed[song[k]].name) <= read_rank(cached[i].name))) {
                heads_got += read_head(listed[song[k]].name, listed[song[k]].size);
                k++;
                continue;
            }
            if (boost_seq != boost_done) {
                /* the fighter the game asked for: its files still on the card first */
                char a[8], b[8];
                int j;
                boost_done = boost_seq;
                if (boost_kind == 2) {
                    snprintf(a, sizeof a, "STG%s", boost_id);
                    snprintf(b, sizeof b, "STG%s", boost_id);
                } else {
                    snprintf(a, sizeof a, "PL%s_", boost_id);
                    snprintf(b, sizeof b, "S_PL%s", boost_id);
                }
                for (j = i; j < n; j++) {
                    if (!cached[j].ready && (strncmp(cached[j].name, a, strlen(a)) == 0 ||
                                             strncmp(cached[j].name, b, strlen(b)) == 0)) {
                        got += cache_read(j, &done);
                    }
                }
            }
            if (!cached[i].ready) {
                got += cache_read(i, &done);
            }
            i++;
        }
        host_log("gd: the first %u KB of %d music files read with them", HEAD_BYTES >> 10, heads_got);
    }
    (void)names, (void)sizes, (void)offs;
    host_log("gd: %d files, %u KB, read into memory in %.1f s (%.1f s of it giving way to the game's reads)", got,
             (unsigned)(done >> 10), (sceKernelGetProcessTimeWide() - t0) / 1e6, bg_waited / 1e6);
    {
        /* what the budget left out, for the late reads */
        int j, i, left = 0;
        uint32_t kb = 0;
        for (j = 0; j < nlisted; j++) {
            int have = 0;
            for (i = 0; i < ncached && !have; i++) {
                have = strcmp(cached[i].name, listed[j].name) == 0;
            }
            if (!have && strncmp(listed[j].name, "ADX", 3) != 0) {
                left++, kb += listed[j].size >> 10;
            }
        }
        host_log("gd: %d files (%u KB, the music aside) left on the card, read ahead as the game comes to them", left, kb);
    }
    late_main();
    return 0;
}

static void cache_start(void) {
    const char* e = getenv("MVC2_GD_CACHE");
    if (cache_thread != 0 || (e != NULL && atoi(e) == 0)) {
        return;
    }
    cache_thread = sceKernelCreateThread("gd_cache", cache_main, 185, 64 * 1024, 0, 0x00040000, NULL);
        {
            extern void rd_vgl_watch_thread(const char* name, int id) __attribute__((weak));
            if (rd_vgl_watch_thread && cache_thread >= 0) {
                rd_vgl_watch_thread("CACHE", cache_thread);
            }
        }
    if (cache_thread < 0 || sceKernelStartThread(cache_thread, 0, NULL) < 0) {
        host_log("gd: no thread to read the disc's files into memory (%08X)", cache_thread);
    }
}

/* a stream over a file read into memory, or NULL; its bytes in *mem */
static FILE* cache_open(const char* name, const uint8_t** mem) {
    int i;
    for (i = 0; i < ncached; i++) {
        if (cached[i].ready && strcmp(cached[i].name, name) == 0) {
            *mem = cached[i].mem;
            cached[i].opened = sceKernelGetProcessTimeWide();
            return fmemopen(cached[i].mem, cached[i].size, "rb");
        }
    }
    return NULL;
}
#else
#define read_begin() 0
#define read_end(what, t0, bytes) ((void)(t0))
#define async_wait(gd) ((void)0)
#endif

static void cache_boost(const char* name);
static FILE* open_file(const char* name) {
    char path[1200];
    FILE* f;
#ifdef __vita__
    uint64_t t_open = read_begin();
#endif
    snprintf(path, sizeof path, "%s/%s", root, name);
#ifdef __vita__
    cache_boost(name);
    fg_begin();
#endif
    f = fopen(path, "rb");
#ifdef __vita__
    fg_end();
    gd_account(name, t_open, 0, 1);
    /* unbuffered, so newlib reads a request whole into the game's memory in one call: through stdio's buffer a
       large read was cut into small ones, at about 3.4 MB/s (MVC2_GD_NOBUF=0: buffered) */
    if (f != NULL && (getenv("MVC2_GD_NOBUF") == NULL || atoi(getenv("MVC2_GD_NOBUF")) != 0)) {
        setvbuf(f, NULL, _IONBF, 0);
    }
#endif
#ifdef __3DS__
    if (no_buf < 0) {
        no_buf = getenv("MVC2_GD_NOBUF") != NULL && atoi(getenv("MVC2_GD_NOBUF")) != 0;
        trace = getenv("MVC2_TRACE_GD") != NULL;
        if (no_buf) {
            host_log("gd: the files read without stdio's buffer (MVC2_GD_NOBUF)");
        }
    }
    if (f != NULL && no_buf) {
        setvbuf(f, NULL, _IONBF, 0);
    }
#endif
    return f;
}

#if defined(__3DS__) || defined(__vita__)
const char* dc_gd_path(const char* name) {
    static char path[1200];
    snprintf(path, sizeof path, "%s/%s", root, name);
    return path;
}
#endif

long dc_gd_read_file(const char* name, void* buf, long max) {
    FILE* f = open_file(name);
    uint64_t t0 = read_begin();
    long n;
    if (f == NULL) {
        return -1;
    }
    n = (long)fread(buf, 1, (size_t)max, f);
    read_end(name, t0, (size_t)n);
    fclose(f);
    return n;
}

Sint32 gdFsInit(Sint32 max_open, void* work, Sint32 max_dirent, void* dirbuf) {
    (void)max_open;
    (void)work;
    (void)max_dirent;
    (void)dirbuf;
    return GDD_ERR_OK;
}

void gdFsFinish(void) {
    int i;
#ifdef __vita__
    async_wait(vjob_gd);
#else
    async_wait(async_gd);
#endif
#ifdef __3DS__
    ctr_ahead_drop(1);
    ctr_small_drop();
    keep_close_all();
#endif
    for (i = 0; i < GD_MAX_OPEN; i++) {
        if (files[i].f != NULL) {
#ifdef __vita__
            if (files[i].f != LAZY)
#endif
            fclose(files[i].f);
            files[i].f = NULL;
        }
    }
}

GDFS gdFsOpen(const char* fname, void* dirrec) {
    int i;
    (void)dirrec;
    for (i = 0; i < GD_MAX_OPEN; i++) {
        if (files[i].f == NULL) {
            GDFS gd = &files[i];
#ifdef __3DS__
            PROF_START(t0);
            long n = 0;
            if ((gd->f = ctr_small_open(dc_gd_path(fname), &n)) != NULL ||
                (keep_on > 0 && (gd->f = ctr_ahead_file(dc_gd_path(fname), &n)) != NULL)) {
                gd->size = (Sint32)n;
            } else if ((gd->f = keep_take(fname, &gd->size)) == NULL && (gd->f = open_file(fname)) != NULL) {
                fseek(gd->f, 0, SEEK_END);
                gd->size = (Sint32)ftell(gd->f);
            }
            PROF_STOP(PZ_GD_OPEN, t0);
            progs_after_open(fname);
            snprintf(gd->name, sizeof gd->name, "%s", fname);
            snprintf(opened[opened_n++ % 16], sizeof opened[0], "%s", fname);
#elif defined(__vita__)
            gd->vname[0] = 0;
            gd->mem = NULL;
            if ((gd->f = cache_open(fname, &gd->mem)) == NULL) {
                gd->mem = NULL;
                if (vreader_start() && listed_size(fname, &gd->size)) {
                    snprintf(gd->vname, sizeof gd->vname, "%s", fname);
                    gd->f = LAZY;
                } else {
                    gd->f = open_file(fname);
                }
            }
#else
            gd->f = open_file(fname);
#endif
            if (gd->f == NULL) {
                host_log("gdFsOpen: no %s on the disc", fname);
                return NULL;
            }
#ifdef __vita__
            if (gd->f != LAZY)
#endif
#ifndef __3DS__
            {
                fseek(gd->f, 0, SEEK_END);
                gd->size = (Sint32)ftell(gd->f);
            }
#endif
            if (getenv("MVC2_TRACE_GD")) {
                host_log("gd %s %X (frame %u)", fname, gd->size, host_frame());
            }
            gd->pos = 0;
            gd->stat = GDD_STAT_IDLE;
            return gd;
        }
    }
    host_log("gdFsOpen: more than %d files open", GD_MAX_OPEN);
    return NULL;
}

void gdFsClose(GDFS gd) {
    async_wait(gd);
    if (gd != NULL && gd->f != NULL) {
        PROF_START(t0);
#ifdef __3DS__
        if (!keep_put(gd)) {
            fclose(gd->f);
        }
#else
#ifdef __vita__
        if (gd->f != LAZY)
#endif
        fclose(gd->f);
#endif
        gd->f = NULL;
        PROF_STOP(PZ_GD_OPEN, t0);
    }
}

Sint32 gdFsGetFileSize(GDFS gd, Sint32* fsize) {
    if (gd == NULL || gd->f == NULL) {
        return 0;
    }
    *fsize = gd->size;
    return 1;
}

/* a file's close off the game's thread: on a slow card a close is a request of its own (a second, the game held
   up at a fight's start); the reader's own closes are done at once, the others by a thread of their own */
#define CLOSE_MOST 16
static FILE* volatile close_ring[CLOSE_MOST];
static volatile unsigned close_head, close_tail;
static SceUID closer = 0, close_sema;

static int closer_main(SceSize args, void* argp) {
    (void)args, (void)argp;
    for (;;) {
        sceKernelWaitSema(close_sema, 1, NULL);
        while (close_tail != close_head) {
            FILE* f = close_ring[close_tail % CLOSE_MOST];
            __sync_synchronize();
            close_tail++;
            bg_wait();
            fclose(f);
        }
    }
    return 0;
}

static void later_close(FILE* f) {
    if (sceKernelGetThreadId() == vreader) {
        fclose(f);
        return;
    }
    if (closer == 0) {
        close_sema = sceKernelCreateSema("gd_close", 0, 0, CLOSE_MOST, NULL);
        closer = sceKernelCreateThread("gd_closer", closer_main, 190, 16 * 1024, 0, 0x00040000, NULL);
        if (closer < 0 || sceKernelStartThread(closer, 0, NULL) < 0) {
            closer = -1;
        }
    }
    if (closer < 0 || close_head - close_tail >= CLOSE_MOST) {
        fclose(f);      /* no thread, or its ring full: here */
        return;
    }
    close_ring[close_head % CLOSE_MOST] = f;
    __sync_synchronize();
    close_head++;
    sceKernelSignalSema(close_sema, 1);
}

static void read_sectors(GDFS gd, Sint32 nsct, void* buf) {
    size_t want = (size_t)nsct * GDD_SCT_SIZE, got = 0;
    PROF_START(t1);
    host_fills++;
#ifdef __vita__
    if (gd->f == LAZY) {
        FILE* f = open_file(gd->vname);     /* on the reader, for a requested read */
        if (f == NULL) {
            host_log("gd: %s could not be opened for its read", gd->vname);
            memset(buf, 0, (size_t)nsct * GDD_SCT_SIZE);
            gd->pos += (long)nsct * GDD_SCT_SIZE;
            gd->stat = GDD_STAT_COMPLETE;
            return;
        }
        gd->f = f;
    }
#endif
    if (gd->pos < gd->size) {
        uint64_t t0 = read_begin();
#ifdef __3DS__
        /* not past the file's end: a read that reaches it costs the SD card another access */
        size_t n = (size_t)(gd->size - gd->pos) < want ? (size_t)(gd->size - gd->pos) : want;
#else
        size_t n = want;
#endif
#ifdef __vita__
        if (gd->mem != NULL) {
            got = (size_t)(gd->size - gd->pos) < n ? (size_t)(gd->size - gd->pos) : n;
            memcpy(buf, gd->mem + gd->pos, got);
        } else
#endif
        {
#ifdef __vita__
            fg_begin();
#endif
            fseek(gd->f, gd->pos, SEEK_SET);
            got = fread(buf, 1, n, gd->f);
#ifdef __vita__
            fg_end();
#endif
        }
        read_end("read", t0, got);
    }
    memset((char*)buf + got, 0, want - got);
#ifdef __vita__
    /* a whole file read into one place: a model file's pointers say 0x0C..., the Vita's RAM is at 0x8C... */
    if (gd->pos == 0) {
        gd->first = buf;
    } else if ((char*)buf != gd->next) {
        gd->first = NULL;
    }
    gd->next = (char*)buf + want;
    if (gd->first != NULL && gd->pos < gd->size && gd->pos + (long)want >= gd->size) {
        vita_relocate_file(gd->first, (size_t)gd->size);
    }
#endif
    gd->pos += (long)want;
#ifdef __vita__
    /* a file opened for its read and now read to its end closed here, on the thread that read it: the game's close
       was a request of the card's of its own on the game's thread (half a second held up on a slow card, each time
       a versus screen's read was done); opened again if it is read again */
    if (gd->vname[0] && gd->mem == NULL && gd->f != LAZY && gd->f != NULL && gd->pos >= gd->size) {
        later_close(gd->f);
        gd->f = LAZY;
    }
#endif
    gd->stat = GDD_STAT_COMPLETE;
    PROF_STOP(PZ_GD_READ, t1);
}

Sint32 gdFsRead(GDFS gd, Sint32 nsct, void* buf) {
    if (gd == NULL || gd->f == NULL) {
        return -1;
    }
    async_wait(gd);
#ifdef __3DS__
    if (ahead_take(gd, nsct, buf, 0)) {
        return GDD_ERR_OK;
    }
    {
        PROF_START(t0);
        if (ctr_small_take(dc_gd_path(gd->name), (size_t)gd->pos, buf, (size_t)nsct * GDD_SCT_SIZE)) {
            host_fills++;
            gd->pos += (long)nsct * GDD_SCT_SIZE;
            gd->stat = GDD_STAT_COMPLETE;
            PROF_STOP(PZ_GD_READ, t0);
            if (trace) {
                host_log("gd %s taken as read ahead (frame %u)", gd->name, host_frame());
            }
            return GDD_ERR_OK;
        }
    }
    read_sectors(gd, nsct, buf);
    next_after(gd->name, 1);
#else
    read_sectors(gd, nsct, buf);
#endif
    return GDD_ERR_OK;
}

Sint32 gdFsReqRd32(GDFS gd, Sint32 nsct, void* buf) {
    if (gd == NULL || gd->f == NULL) {
        return -1;
    }
#ifdef __3DS__
    if (gd_async() && gd->pos < gd->size) {
        size_t want = (size_t)nsct * GDD_SCT_SIZE;
        if (async_gd != NULL) {
            return 0;                   /* the drive busy: asked again */
        }
        uint32_t bytes = (uint32_t)(gd->size - gd->pos) < want ? (uint32_t)(gd->size - gd->pos) : (uint32_t)want;
        unsigned due = host_frame() + gd_seek + (unsigned)((bytes * 60ull + GD_RATE - 1) / GD_RATE);
        if (ahead_take(gd, nsct, buf, 1)) {
            async_job = 0;              /* in already, but complete when the drive would be */
        } else {
            if (ctr_ahead_has(dc_gd_path(gd->name))) {
                ctr_ahead_drop(0);      /* still being read: read here instead */
            }
            memset((char*)buf + bytes, 0, want - bytes);
            if (!ctr_read_start(gd->f, gd->pos, buf, bytes)) {
                read_sectors(gd, nsct, buf);
                return nsct;
            }
            async_job = 1;
            gd->pos += (long)want;
        }
        async_due = due;
        gd->stat = GDD_STAT_READ;
        async_gd = gd;
        return nsct;
    }
#endif
#ifdef __vita__
    if (vreader_start()) {
        if (vjob_busy) {
            return 0;                   /* the drive busy: asked again */
        }
        gd->stat = GDD_STAT_READ;
        vjob_gd = gd, vjob_nsct = nsct, vjob_buf = buf;
        vjob_busy = 1;
        sceKernelSignalSema(vreq, 1);
        return nsct;
    }
#endif
    read_sectors(gd, nsct, buf);
    return nsct;
}

/* moves to a sector: from the file's start (origin 0), from where it is (1) or from its end (2) (the ADX player) */
Sint32 gdFsSeek(GDFS gd, Sint32 offset, Sint32 origin) {
    long base;
    if (gd == NULL || gd->f == NULL) {
        return -1;
    }
    async_wait(gd);
    base = origin == 1 ? gd->pos : origin == 2 ? (gd->size + GDD_SCT_SIZE - 1) / GDD_SCT_SIZE * GDD_SCT_SIZE : 0;
    gd->pos = base + (long)offset * GDD_SCT_SIZE;
    return GDD_ERR_OK;
}

Sint32 gdFsGetStat(GDFS gd) {
#ifdef __3DS__
    if (gd != NULL && gd == async_gd && (int)(host_frame() - async_due) >= 0) {
        async_end();
    }
#endif
    return gd != NULL ? gd->stat : GDD_STAT_IDLE;
}

GDFS gdFsGetSysHn(void) {
    return &sys_handle;
}

Sint32 gdFsGetDrvStat(void) {
    return GDD_DRVSTAT_PAUSE;
}

Sint32 gdFsReqDrvStat(void) {
    return GDD_ERR_OK;
}

void gdFsEntryErrFuncAll(GDFS_ERRFUNC func, void* obj) {
    (void)func;
    (void)obj;
}
