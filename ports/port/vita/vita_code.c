/*
 * vita_code.c  Memory that code can be written into and run from, for the sound CPU's translator (arm7_jit.c,
 * arm7_jit2.c), on the PS Vita: a block of the app's own memory made readable, writable and executable by kubridge
 * (kuKernelMemProtect), its caches kept by kuKernelFlushCaches (the data written out, the instructions read again).
 * A probe writes a word of code, runs it, writes it again and runs it again; the translator is off if that fails.
 * MVC2_ARMJIT=1 (or MVC2_ARMJIT2=mode) turns the translator on (snd_nl.c).
 */

#include <stdint.h>
#include <psp2/kernel/sysmem.h>
#include <kubridge.h>

#include "naomi_host.h"
#include "../snd/arm7_jit.h"

#define CODE_SIZE (1024 * 1024)

static uint8_t* code;
static int ready;

static const char* probe(int* ran) {
    volatile uint32_t* w = (volatile uint32_t*)code;
    int (*fn)(void) = (int (*)(void))(void*)code;   /* an even address: blx enters it in ARM state */
    int n;
    for (n = 1; n <= 2; n++) {
        w[0] = 0xE3A00000u | (uint32_t)n;           /* mov r0, #n */
        w[1] = 0xE12FFF1Eu;                         /* bx lr */
        kuKernelFlushCaches(code, 8);
        ran[n - 1] = fn();
        if (ran[n - 1] != n) {
            return "code written again didn't run";
        }
    }
    return NULL;
}

void* host_code(uint32_t* size) {
    if (!ready) {
        const char* why = NULL;
        int ran[2] = { 0, 0 };
        SceUID blk = sceKernelAllocMemBlock("arm7_code", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, CODE_SIZE, NULL);
        host_log("sound CPU: trying the translator");   /* a crash in the calls below stops after this line */
        if (blk < 0 || sceKernelGetMemBlockBase(blk, (void**)&code) < 0) {
            why = "no memory for code";
        } else if (kuKernelMemProtect(code, CODE_SIZE, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE |
                                                       KU_KERNEL_PROT_EXEC) < 0) {
            why = "no executable memory (kubridge)";
        } else {
            why = probe(ran);
        }
        host_log("sound CPU: %s%s (the probe ran %d then %d)", why ? "interpreted: " : "translated", why ? why : "",
                 ran[0], ran[1]);
        ready = why ? -1 : 1;
    }
    if (ready < 0) {
        return NULL;
    }
    *size = CODE_SIZE;
    return code;
}

void host_code_flush(void* p, uint32_t size) {
    kuKernelFlushCaches(p, size);
}
