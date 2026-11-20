/*
 * coro_vita.c  Coroutines for the PS Vita, as threads that take turns
 *
 * coro_arm.c's stack switch can't be used: the Vita's kernel kills a thread that makes a system call (a file
 * read, the log) with its stack pointer outside the stack the kernel gave it. So each coroutine is a thread
 * with its own stack, and a switch wakes the other and waits: only one of them runs at a time, as with the
 * stack switch, and the game's frame costs two thread switches.
 */

#include <stdio.h>
#include <stdlib.h>

#include <psp2/kernel/threadmgr.h>

#include "coro.h"

struct coro {
    SceUID thread;      /* 0: the thread that adopted it */
    SceUID turn;        /* a semaphore: its turn to run */
    void (*fn)(void*);
    void* arg;
    volatile int dying; /* coro_delete: leave when woken */
};

static coro main_coro;
static coro* volatile current;

coro* coro_self(void) {
    if (current == NULL) {
        sceKernelChangeThreadCpuAffinityMask(0, 0x00010000);    /* the host's thread on core 0 too (0: this one) */
        main_coro.turn = sceKernelCreateSema("coro_main", 0, 0, 1, NULL);
        current = &main_coro;
    }
    return current;
}

/* the one waiting on c->turn has it */
static void wait_turn(coro* c) {
    if (sceKernelWaitSema(c->turn, 1, NULL) < 0 || c->dying) {
        sceKernelExitDeleteThread(0);
    }
}

void vita_thread_start(void) __attribute__((weak));     /* port/vita/vita_sys.c: the abort handler */
void rd_vgl_watch_thread(const char* name, int id) __attribute__((weak));   /* the overlay's CPU shares */

static int coro_entry(SceSize args, void* argp) {
    coro* c = *(coro**)argp;
    (void)args;
    if (vita_thread_start != NULL) {
        vita_thread_start();
    }
    if (rd_vgl_watch_thread != NULL) {
        rd_vgl_watch_thread("GAME", sceKernelGetThreadId());
    }
    wait_turn(c);
    c->fn(c->arg);
    fprintf(stderr, "coro: a coroutine returned\n");
    abort();
    return 0;
}

static void start(coro* c, void (*fn)(void*), void* arg, unsigned stack_size) {
    char name[32];
    int prio = sceKernelGetThreadCurrentPriority();
    c->fn = fn;
    c->arg = arg;
    c->dying = 0;
    snprintf(name, sizeof name, "coro_%p", (void*)c);
    c->turn = sceKernelCreateSema(name, 0, 0, 1, NULL);
    /* the game's on core 0 with the host's (only one of them runs at a time); the renderer has core 1 */
    c->thread = sceKernelCreateThread(name, coro_entry, prio, stack_size < 0x4000 ? 0x4000 : stack_size, 0,
                                      0x00010000 /* SCE_KERNEL_CPU_MASK_USER_0 */, NULL);
    if (c->turn < 0 || c->thread < 0 || sceKernelStartThread(c->thread, sizeof c, &c) < 0) {
        fprintf(stderr, "coro_create: no thread for a %u byte stack (%08X %08X)\n", stack_size, c->turn, c->thread);
        abort();
    }
}

coro* coro_create(void (*fn)(void*), void* arg, unsigned stack_size) {
    coro* c = calloc(1, sizeof(coro));
    coro_self();
    if (c == NULL) {
        abort();
    }
    start(c, fn, arg, stack_size);
    return c;
}

/* the thread woken to leave, and waited for */
static void stop(coro* c) {
    c->dying = 1;
    sceKernelSignalSema(c->turn, 1);
    sceKernelWaitThreadEnd(c->thread, NULL, NULL);
    sceKernelDeleteSema(c->turn);
    c->thread = c->turn = 0;
}

void coro_restart(coro* c, void (*fn)(void*), void* arg) {
    SceKernelThreadInfo info;
    if (c == current || c == &main_coro) {
        return;
    }
    info.size = sizeof info;
    sceKernelGetThreadInfo(c->thread, &info);
    stop(c);
    start(c, fn, arg, info.stackSize);
}

void coro_delete(coro* c) {
    if (c != NULL && c != current && c != &main_coro) {
        stop(c);
        free(c);
    }
}

void coro_switch(coro* to) {
    coro* from = coro_self();
    if (to != from) {
        current = to;
        sceKernelSignalSema(to->turn, 1);
        wait_turn(from);
    }
}
