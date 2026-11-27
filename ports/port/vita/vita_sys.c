/*
 * vita_sys.c  The PS Vita's part of the front end
 *
 * Main RAM: the Vita gives a program nothing at the game's 0x0C000000, but kubridge's kernel calls map memory
 * at 0x8C000000 and alias the same pages at 0xAC000000, which are the SH-4's P1 and P2 mirrors of main RAM, so
 * the Dreamcast program's own addresses work as they are and the game's C is built with its 0x0C... addresses
 * moved to 0x8C... (port/tools/vita_rewrite.py). An address the build could not see (a pointer stored in a
 * file the game loads, say) can still be 0x0C...: a data abort there has the register that holds it moved to
 * 0x8C... and the access tried again, and the log names the code that did it, once.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <kubridge.h>

#include <malloc.h>
#include "naomi_host.h"
#include "vita_sys.h"

/* the newlib heap (textures, sound, the software renderer's buffers). 64 MB filled by the fight (64.2 MB in use:
   textures of 128 KB came back NULL and were not drawn) with 92 MB of the system's left over: 112 MB leaves some
   40 MB free. Its block (at 0x82D00000 on this console) must end below the main RAM's at 0x8C000000 */
int _newlib_heap_size_user = 112 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

#define P1_BASE 0x8C000000u
#define P2_BASE 0xAC000000u
#define LOW_BASE 0x0C000000u
#define LOW_END 0x0E000000u

static SceUID log_fd = -1;
static int log_off;             /* DEBUG_OPTIONS=0: no log at all */

/*
 * The log, written by a thread of its own (core 2, low priority): a write to the memory card can take milliseconds,
 * and one on the game's or the renderer's thread was a missed frame. Lines go into a ring under a lightweight
 * mutex; before the writer is started they are written at once. A full ring drops the line and says so later.
 */
#define LOG_RING (128 * 1024)
static char log_ring[LOG_RING];
static volatile unsigned log_head, log_tail, log_dropped;
static SceKernelLwMutexWork log_lock;
static SceUID log_sema = -1;

/* the TTY too until the writer thread starts (a start that dies early is seen there), then only with MVC2_TTY=1:
   once the PC's reader of the TTY stops, a print can hold the writer up, and the log file with it (seen in tests:
   the TTY quiet from 49 s, and in some runs the log and the pictures stopped) */
static int tty_on = -1;

static void log_write_now(const char* s, int n) {
    if (log_fd >= 0) {
        sceIoWrite(log_fd, s, n);
    }
    if (log_sema < 0 || tty_on > 0) {
        sceClibPrintf("%.*s", n, s);
    }
}

static void log_put(const char* s, int n) {
    unsigned used;
    int i;
    if (log_sema < 0) {
        log_write_now(s, n);
        return;
    }
    /* the writers among themselves only, for a copy's time: the log's thread never takes the lock (it was, at the
       lowest priority, held up while holding it, and the game with it) */
    sceKernelLockLwMutex(&log_lock, 1, NULL);
    used = log_head - log_tail;
    if (used + (unsigned)n > LOG_RING) {
        log_dropped++;
    } else {
        for (i = 0; i < n; i++) {
            log_ring[(log_head + (unsigned)i) % LOG_RING] = s[i];
        }
        __sync_synchronize();   /* the bytes before the head that shows them */
        log_head += (unsigned)n;
    }
    sceKernelUnlockLwMutex(&log_lock, 1);
    sceKernelSignalSema(log_sema, 1);
}

static int log_main(SceSize args, void* argp) {
    static char chunk[8192];
    (void)args, (void)argp;
    for (;;) {
        unsigned n = 0;
        sceKernelWaitSema(log_sema, 1, NULL);
        for (;;) {
            unsigned head = log_head;
            __sync_synchronize();
            n = head - log_tail;
            n = n > sizeof chunk ? (unsigned)sizeof chunk : n;
            for (unsigned i = 0; i < n; i++) {
                chunk[i] = log_ring[(log_tail + i) % LOG_RING];
            }
            __sync_synchronize();
            log_tail += n;
            if (n == 0) {
                break;
            }
            log_write_now(chunk, (int)n);
        }
        if (log_dropped) {
            char note[64];
            int k = snprintf(note, sizeof note, "(log: %u lines dropped, the ring full)\n", log_dropped);
            log_dropped = 0;
            log_write_now(note, k);
        }
    }
    return 0;
}

static void log_start(void) {
    SceUID th;
    sceKernelCreateLwMutex(&log_lock, "log_lock", 0, 0, NULL);
    log_sema = sceKernelCreateSema("log_lines", 0, 0, 1 << 20, NULL);
    th = sceKernelCreateThread("log_writer", log_main, 190, 32 * 1024, 0, 0x00040000, NULL);
    if (th >= 0) {
        extern void rd_vgl_watch_thread(const char* name, int id) __attribute__((weak));
        if (rd_vgl_watch_thread) {
            rd_vgl_watch_thread("LOG", th);
        }
    }
    if (log_sema < 0 || th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        char note[96];
        int k = snprintf(note, sizeof note, "vita: no log thread (%08X): the log written as it comes\n", th);
        log_sema = -1;
        log_write_now(note, k);
    }
}

/* before main() and SDL's start-up: on the TTY only, to tell a start that never reached main() */
__attribute__((constructor(101))) static void vita_first_words(void) {
    SceKernelFreeMemorySizeInfo mem = { sizeof mem };
    sceKernelGetFreeMemorySize(&mem);
    sceClibPrintf("mvc2: started, free user %d KB, CDRAM %d KB, phycont %d KB\n", mem.size_user >> 10,
                  mem.size_cdram >> 10, mem.size_phycont >> 10);
}

void vita_log_line(const char* fmt, va_list ap) {
    static uint64_t start;
    if (log_off) {
        return;
    }
    char line[1024];
    uint64_t now = sceKernelGetProcessTimeWide();
    int n, at;
    if (start == 0) {
        start = now;
    }
    /* each line with the seconds since the first, to set beside a test's own times */
    at = snprintf(line, sizeof line, "[%7.2f] ", (now - start) / 1e6);
    n = vsnprintf(line + at, sizeof line - 1 - at, fmt, ap);
    if (n >= 0) {
        n += at;
    }
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof line - 2) {
        n = (int)sizeof line - 2;
    }
    line[n++] = '\n';
    line[n] = 0;
    log_put(line, n);
}

static void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vita_log_line(fmt, ap);
    va_end(ap);
}

/* ---- the abort handler: a 0x0C... address moved to 0x8C... ---- */

#define FIX_SITES 64
static struct {
    unsigned pc, addr, count, told;
} sites[FIX_SITES];
static volatile unsigned fixes, unfixed_sites;
static KuKernelExceptionHandler old_abort;

static void on_data_abort(KuKernelExceptionContext* c) {
    unsigned far = c->FAR, best = 13, i;
    SceUInt32* r = &c->r0;
    if (far >= LOW_BASE && far < LOW_END) {
        /* the base register: the highest value in the low mirror at most 4 KB below the address (a register
           further off, a count or a bound, is not moved: that would only hide the fault) */
        for (i = 0; i <= 12; i++) {
            if (r[i] >= LOW_BASE && r[i] <= far && far - r[i] < 0x1000u && (best == 13 || r[i] > r[best])) {
                best = i;
            }
        }
        if (best < 13) {
            r[best] += P1_BASE - LOW_BASE;
            fixes++;
            for (i = 0; i < FIX_SITES; i++) {
                if (sites[i].pc == c->pc || sites[i].count == 0) {
                    sites[i].pc = c->pc;
                    sites[i].addr = far;
                    sites[i].count++;
                    return;
                }
            }
            unfixed_sites++;    /* the table is full: still fixed, not named */
            return;
        }
    }
    /* not one it can fix: returning would only run the access again, and again. With the handler gone,
       the access faults once more and the system stops the program there, with a core dump */
    sceClibPrintf("vita: data abort at %08X from %08X (lr %08X sp %08X), not fixed; r0-r3 %08X %08X %08X %08X "
                  "r4-r7 %08X %08X %08X %08X r8-r12 %08X %08X %08X %08X %08X\n", far, c->pc, c->lr, c->sp, r[0],
                  r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12]);
    if (old_abort != NULL) {
        old_abort(c);
        return;
    }
    kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT);
}

/* for each thread that runs the game's code (port/sys/coro_vita.c): kubridge's handler may be the thread's */
void vita_thread_start(void) {
    KuKernelExceptionHandler old = NULL;
    int rc = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, on_data_abort, &old, NULL);
    if (old != NULL && old != on_data_abort) {
        old_abort = old;
    }
    say("vita: abort handler on thread %08X (%08X)", sceKernelGetThreadId(), rc);
}

/* MVC2_SNDPROF=1: the sound's time split, its emulated ARM7 (the driver), the channels' mix and the rest (the
   output stage), a line every 2 s */
#include <stdint.h>
#include "../snd/aica.h"
static uint64_t sp_at, sp_part[3], sp_line_at;
static unsigned sp_chunks, sp_frames;
static int sp_last;
static void snd_prof(int at) {
    uint64_t t = sceKernelGetProcessTimeWide();
    switch (at) {
    case AICA_PERF_CHUNK:
        if (sp_at != 0 && sp_last == AICA_PERF_MIX_END) {
            sp_part[2] += t - sp_at;    /* the last chunk's output stage */
        }
        sp_chunks++;
        break;
    case AICA_PERF_ARM_END:
        sp_part[0] += t - sp_at;
        break;
    case AICA_PERF_MIX_END:
        sp_part[1] += t - sp_at;
        break;
    default:
        return;
    }
    sp_at = t, sp_last = at;
}

void vita_frame(void) {
    unsigned i;
    static int sp_on = -1;
    if (sp_on < 0) {
        sp_on = getenv("MVC2_SNDPROF") != NULL && atoi(getenv("MVC2_SNDPROF")) != 0;
        if (sp_on) {
            aica_perf = snd_prof;
        }
    }
    if (sp_on) {
        uint64_t t = sceKernelGetProcessTimeWide();
        sp_frames++;
        sp_last = -1;   /* what follows the last chunk until the next frame isn't the sound's */
        if (sp_line_at == 0) {
            sp_line_at = t;
        } else if (t - sp_line_at >= 2000000) {
            say("sound: a frame's ARM7 %.3f ms, channels %.3f ms, output %.3f ms (%u chunks)", sp_part[0] / 1000.0 / sp_frames,
                sp_part[1] / 1000.0 / sp_frames, sp_part[2] / 1000.0 / sp_frames, sp_chunks / sp_frames);
            memset(sp_part, 0, sizeof sp_part);
            sp_chunks = sp_frames = 0;
            sp_line_at = t;
        }
    }
    for (i = 0; i < FIX_SITES && sites[i].count != 0; i++) {
        if (!sites[i].told) {
            sites[i].told = 1;
            say("vita: a 0x0C address (%08X) from code at %08X, moved to the P1 mirror", sites[i].addr, sites[i].pc);
        }
    }
}

/* ---- the disc's model files ---- */

static int low_ram(uint32_t a) {
    return a - LOW_BASE < NAOMI_RAM_SIZE;
}

/*
 * A model file (OBJ_FILE, src/tex_load.c's obj_file_reloc) holds pointers to where it was built to sit, in the
 * 0x0C mirror: its object table (header word 0, the header's 16 bytes before it), the table's entries, its
 * texture headers (word 2) and their texels. Moved to the 0x8C mirror they point at the same bytes. Anything
 * that doesn't check out as one, all of it inside the file, is left alone.
 */
void vita_relocate_file(void* data, size_t size) {
    uint32_t* p = data;
    uint32_t built, cnt, i, thd_off, ntex = 0;
    uint32_t* tbl;
    uint8_t* thd;
    if (size < 32 || ((uintptr_t)data & 3) != 0 || !low_ram(p[0]) || !low_ram(p[2])) {
        return;
    }
    built = p[0] - 16;
    cnt = p[1];
    thd_off = p[2] - built;
    if (cnt == 0 || cnt > 4096 || 16 + (size_t)cnt * 4 > size || thd_off < 16 || thd_off + 16 > size ||
        (thd_off & 3) != 0) {
        return;
    }
    tbl = p + 4;
    for (i = 0; i < cnt; i++) {
        if (!low_ram(tbl[i]) || tbl[i] - built >= size) {
            return;
        }
    }
    /* the texture headers (NLthd: width, height, type, address, index), ended by a width of 0 */
    thd = (uint8_t*)data + thd_off;
    while (thd + 16 <= (uint8_t*)data + size && *(int16_t*)thd != 0) {
        uint32_t addr = *(uint32_t*)(thd + 8);
        if (*(int16_t*)thd < 0 || *(int16_t*)thd > 1024 || (addr != 0 && !low_ram(addr) && addr - NAOMI_RAM_BASE >= NAOMI_RAM_SIZE)) {
            return;
        }
        thd += 16;
        ntex++;
    }
    p[0] += NAOMI_RAM_BASE - LOW_BASE;
    p[2] += NAOMI_RAM_BASE - LOW_BASE;
    for (i = 0; i < cnt; i++) {
        tbl[i] += NAOMI_RAM_BASE - LOW_BASE;
    }
    thd = (uint8_t*)data + thd_off;
    for (i = 0; i < ntex; i++, thd += 16) {
        uint32_t* addr = (uint32_t*)(thd + 8);
        if (low_ram(*addr)) {
            *addr += NAOMI_RAM_BASE - LOW_BASE;
        }
    }
    say("vita: a model file at %p moved to the P1 mirror: %u objects, %u textures", data, cnt, ntex);
}

/* ---- start-up ---- */

int vita_map_ram(void) {
    SceKernelAllocMemBlockKernelOpt opt;
    KuKernelMemCommitOpt commit;
    void* p2 = (void*)P2_BASE;
    SceUID block, alias;
    int rc;

    memset(&opt, 0, sizeof opt);
    opt.size = sizeof opt;
    opt.attr = 0x1;                 /* SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_VBASE: field_C is the address */
    opt.field_C = P1_BASE;
    block = kuKernelAllocMemBlock("mvc2_ram", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, NAOMI_RAM_SIZE, &opt);
    if (block < 0) {
        say("vita: main RAM at %08X: error %08X (is kubridge 0.3 or later in ur0:tai/config.txt?)", P1_BASE, block);
        return 0;
    }
    alias = kuKernelMemReserve(&p2, NAOMI_RAM_SIZE, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    if (alias < 0 || p2 != (void*)P2_BASE) {
        say("vita: the P2 mirror at %08X: error %08X", P2_BASE, alias);
        return 0;
    }
    memset(&commit, 0, sizeof commit);
    commit.size = sizeof commit;
    commit.attr = KU_KERNEL_MEM_COMMIT_ATTR_HAS_BASE;
    commit.baseBlock = block;
    commit.baseOffset = 0;
    rc = kuKernelMemCommit(p2, NAOMI_RAM_SIZE, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, &commit);
    if (rc < 0) {
        say("vita: the P2 mirror's pages: error %08X", rc);
        return 0;
    }
    memset((void*)P1_BASE, 0, NAOMI_RAM_SIZE);
    *(volatile uint32_t*)P1_BASE = 0x4D564332;
    if (*(volatile uint32_t*)P2_BASE != 0x4D564332) {
        say("vita: the P2 mirror is not the same memory");
        return 0;
    }
    *(volatile uint32_t*)P1_BASE = 0;
    {
        /* how fast each mirror reads (a texture's source is read from the game's RAM as it is loaded) */
        static const struct {
            const char* name;
            uintptr_t at;
        } where[2] = { { "P1", P1_BASE }, { "P2", P2_BASE } };
        void* heap = malloc(2 << 20);
        int w;
        for (w = 0; w < 3; w++) {
            const volatile uint32_t* q = w < 2 ? (const uint32_t*)(where[w].at + (4u << 20)) : (const uint32_t*)heap;
            uint64_t t0 = sceKernelGetProcessTimeWide(), us;
            uint32_t sum = 0;
            int i;
            if (q == NULL) {
                continue;
            }
            for (i = 0; i < (1 << 18); i += 4) {
                sum += q[i] + q[i + 1] + q[i + 2] + q[i + 3];
            }
            us = sceKernelGetProcessTimeWide() - t0;
            say("vita: reading 1 MB from %s: %.2f ms (%u MB/s)%s", w < 2 ? where[w].name : "the heap", us / 1000.0,
                us ? (unsigned)((1u << 20) / us) : 0, sum == 12345 ? "" : "");
            /* the next MB, prefetched 256 bytes ahead */
            q += 1 << 18;
            t0 = sceKernelGetProcessTimeWide();
            for (i = 0; i < (1 << 18); i += 8) {
                __builtin_prefetch((const void*)(q + i + 64));
                sum += q[i] + q[i + 1] + q[i + 2] + q[i + 3] + q[i + 4] + q[i + 5] + q[i + 6] + q[i + 7];
            }
            us = sceKernelGetProcessTimeWide() - t0;
            say("vita: reading 1 MB from %s, prefetched: %.2f ms (%u MB/s)%s", w < 2 ? where[w].name : "the heap",
                us / 1000.0, us ? (unsigned)((1u << 20) / us) : 0, sum == 12345 ? "" : "");
        }
        free(heap);
    }
    rc = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, on_data_abort, &old_abort, NULL);
    say("vita: main RAM at %08X and %08X, 0x0C addresses fixed on a fault (%08X)", P1_BASE, P2_BASE, rc);
    return 1;
}

/*
 * env.txt takes only these. The player's: ASPECT_RATIO, CLOCKS, DEBUG_OPTIONS (0 or 1, 1 unless set: 0 turns off every
 * tool made for testing, for a players' build: the log, the overlay, the pictures, dumps and bursts and their SELECT
 * keys, the debug menu, the TTY and the watchdog). With DEBUG_OPTIONS=1 also the test runs' MVC2_STAGE, MVC2_BOSS and
 * MVC2_TEST. Any other line is noted in the log and left out: the program's own settings keep their defaults, and the
 * tools DEBUG_OPTIONS turns on are on with it (the software renderer's picture beside each capture)
 */
int vita_debug = 1;
static const char* const player_vars[] = { "ASPECT_RATIO", "CLOCKS", "DEBUG_OPTIONS" };
static const char* const debug_vars[] = { "MVC2_STAGE", "MVC2_BOSS", "MVC2_TEST" };

static int listed_in(const char* name, const char* const* list, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(name, list[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* MVC2_TEST=bits (testing only): 1 the computer's players at 1 vital, 2 the human's hyper gauge kept full, 4 a note of
   the computer's vital each second, 8 the human's vital kept full */
static int test_flags = -1;
int host_test_flags(void) {
    if (test_flags < 0) {
        test_flags = getenv("MVC2_TEST") != NULL ? atoi(getenv("MVC2_TEST")) : 0;
    }
    return test_flags;
}

/* the debug menu's switches over them */
void host_test_flags_set(int f) {
    test_flags = f;
}

/* MVC2_BOSS=n (testing only): a side joining the arcade starts at its fight n (7: the last, Abyss) */
int host_first_stage(void) {
    const char* v = getenv("MVC2_BOSS");
    int n = v != NULL ? atoi(v) : 0;
    return n < 0 ? 0 : n > 7 ? 7 : n;
}

void vita_early_init(void) {
    SceKernelFreeMemorySizeInfo mem = { sizeof mem };
    static char env_lines[32][128];
    int nenv = 0, i;
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/mvc_2", 0777);
    /* env.txt's NAME=value lines (a Vita app has no command line): the listed ones as environment variables. Read
       before the log, which DEBUG_OPTIONS may turn off */
    static char env_left[32][128];
    int nleft = 0;
    {
        FILE* f = fopen(VITA_DATA_DIR "env.txt", "r");
        static char names[32][64], values[32][128];
        char line[512];
        int n = 0;
        while (f != NULL && fgets(line, sizeof line, f) != NULL && n < 32) {
            char* eq = strchr(line, '=');
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] != '#' && eq != NULL) {
                *eq = 0;
                snprintf(names[n], sizeof names[0], "%s", line);
                snprintf(values[n], sizeof values[0], "%s", eq + 1);
                n++;
            }
        }
        if (f != NULL) {
            fclose(f);
        }
        for (i = 0; i < n; i++) {
            if (strcmp(names[i], "DEBUG_OPTIONS") == 0) {
                vita_debug = atoi(values[i]) != 0;
            }
        }
        for (i = 0; i < n; i++) {
            int take = listed_in(names[i], player_vars, (int)(sizeof player_vars / sizeof player_vars[0])) ||
                       (vita_debug && listed_in(names[i], debug_vars, (int)(sizeof debug_vars / sizeof debug_vars[0])));
            if (take) {
                setenv(names[i], values[i], 1);
                if (nenv < 32) {
                    snprintf(env_lines[nenv++], sizeof env_lines[0], "%s=%s", names[i], values[i]);
                }
            } else if (nleft < 32) {
                snprintf(env_left[nleft++], sizeof env_left[0], "%s=%s", names[i], values[i]);
            }
        }
    }
    if (!vita_debug) {
        log_off = 1;
    } else {
        setenv("MVC2_SHOT_SOFT", "1", 1);   /* each capture with the software renderer's picture of its frame */
        log_fd = sceIoOpen(VITA_DATA_DIR "mvc2.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        /* what the front end writes to stderr itself (SDL's errors) */
        if (freopen(VITA_DATA_DIR "stderr.txt", "w", stderr) != NULL) {
            setvbuf(stderr, NULL, _IONBF, 0);
        }
        log_start();
    }
    say("Marvel vs. Capcom 2 for the PS Vita, built " __DATE__ " " __TIME__);
    for (i = 0; i < nenv; i++) {
        say("vita: env.txt: %s", env_lines[i]);
    }
    for (i = 0; i < nleft; i++) {
        say("vita: env.txt: %s left out (not one env.txt takes)", env_left[i]);
    }
    /* MVC2_RENDERER=soft: the software renderer at 320 by 240 with the nearest texel (about three times faster
       than exact); either way a texture loaded again in a frame from the same source is not read again */
    if (getenv("MVC2_RENDERER") != NULL && strcmp(getenv("MVC2_RENDERER"), "soft") == 0) {
        setenv("MVC2_RDQUALITY", "fast+half", 0);
    }
    setenv("MVC2_TEX_SKIP", "1", 0);
    tty_on = getenv("MVC2_TTY") != NULL && atoi(getenv("MVC2_TTY")) != 0;
    say("vita: renderer %s, soft picture %s, texture skip %s", getenv("MVC2_RENDERER") ? getenv("MVC2_RENDERER") : "gpu",
        getenv("MVC2_RDQUALITY") ? getenv("MVC2_RDQUALITY") : "exact", getenv("MVC2_TEX_SKIP"));
    /* CLOCKS (env.txt): 333 (the default: ARM 333, GPU 222, crossbar 111 MHz, bus 166) or 444
       (ARM 444, bus 222, GPU 222, crossbar 166: more heat and battery, for comparison) */
    if (getenv("CLOCKS") != NULL && atoi(getenv("CLOCKS")) == 444) {
        scePowerSetArmClockFrequency(444);
        scePowerSetBusClockFrequency(222);
        scePowerSetGpuClockFrequency(222);
        scePowerSetGpuXbarClockFrequency(166);
    } else {
        /* the GPU at 222 (ARM, bus and crossbar the Vita's own): Abyss's triple hyper finish, its screen of large
           translucent layers, kept the GPU past a frame's 16.7 ms at 166 (the swap waited up to 20 ms for it) */
        scePowerSetArmClockFrequency(333);
        scePowerSetBusClockFrequency(166);
        scePowerSetGpuClockFrequency(222);
        scePowerSetGpuXbarClockFrequency(111);
    }
    say("vita: clocks ARM %d, bus %d, GPU %d, crossbar %d MHz", scePowerGetArmClockFrequency(),
        scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    sceKernelGetFreeMemorySize(&mem);
    say("vita: free memory: user %d KB, CDRAM %d KB, phycont %d KB", mem.size_user >> 10, mem.size_cdram >> 10,
        mem.size_phycont >> 10);
    {
        /* the heap's own block, and the most it hands out in one piece now */
        void* probe = malloc(16);
        SceKernelMemBlockInfo bi;
        SceUID hb = probe != NULL ? sceKernelFindMemBlockByAddr(probe, 0) : -1;
        size_t most = 0, step;
        memset(&bi, 0, sizeof bi);
        bi.size = sizeof bi;
        if (hb >= 0) {
            sceKernelGetMemBlockInfoByAddr(probe, &bi);
        }
        for (step = (size_t)128 << 20; step >= ((size_t)1 << 20); step >>= 1) {
            void* q = malloc(most + step);
            if (q != NULL) {
                free(q);
                most += step;
            }
        }
        say("vita: heap block %08X at %p, %u KB; the most malloc gives in one piece at start: %u KB", hb, bi.mappedBase,
            (unsigned)(bi.mappedSize >> 10), (unsigned)(most >> 10));
        free(probe);
    }
}
