/*
 * aica.h  The AICA: the sound RAM, the 64 channels, the DSP, timers and the sound CPU's interrupt
 */

#ifndef AICA_H
#define AICA_H

#include <stdint.h>

#define AICA_RATE 44100

int aica_init(int ram_8mb);                 /* 8 MB on the NAOMI, 2 MB on the Dreamcast, the CPU in reset; 0: no RAM */
void aica_free(void);                       /* the RAM let go (a restart's board_stop); host_keep_block keeps it */
void aica_render(int16_t* out, int n);      /* n stereo samples, the sound CPU run alongside */
void aica_run_cpu(int cycles);              /* the sound CPU alone, no time passing (the SH4 waiting on it) */
int aica_cpu_running(void);
int aica_tracing(void);                     /* MVC2_SNDTRACE is writing */

/* the SH4's view: registers at 0x00700000, the RAM at 0x00800000 (any of the areas' mirrors) */
uint32_t aica_sh4_read32(uint32_t addr);
void aica_sh4_write32(uint32_t addr, uint32_t v);
uint8_t* aica_sh4_ptr(uint32_t addr);       /* a RAM address's host pointer */
uint32_t aica_ram_size(void);

/* for measuring */
typedef struct {
    uint64_t cpu_cycles;
    unsigned active_channels;
    unsigned dsp_steps;
} AICA_STATS;
void aica_stats(AICA_STATS* s);
#if defined(MVC2_PROF) || defined(__vita__)
enum { AICA_PERF_CHUNK, AICA_PERF_ARM_END, AICA_PERF_MIX_END, AICA_PERF_FIQ, AICA_PERF_FIQ_END };
extern void (*aica_perf)(int at);   /* called at each of the mix's steps, when set */
#endif
void aica_dump(void (*log)(const char* fmt, ...));

/* for snd_drv.c's check: the state the sound CPU's register writes change, and those writes as they come */
typedef struct AICA_STATE AICA_STATE;
AICA_STATE* aica_state_new(void);
void aica_state_save(AICA_STATE* s);
void aica_state_load(const AICA_STATE* s);
extern void (*aica_write_log)(uint32_t off, uint32_t v, uint32_t mask);
extern uint32_t aica_watch_gen;     /* moved by each chunk and each write to a channel's first word, its loop start
                                       or 0x280C: what snd_drv.c's channel watch reads */
void aica_set_dsp(int on);
void aica_set_reverb(int on);     /* a cheap reverb in the DSP's place, when it is off */
void aica_set_filter(int on);
void aica_set_half(int on);         /* the plain chunks mixed at half the rate */
void aica_set_mix_fast(int on);     /* no mixing work the output doesn't use: the same result */
void aica_set_mix_simd(int on);     /* the plain mix in 32x16 multiplies, its gains made one: close, not the same */
void aica_set_timer_fast(int on);   /* the timers' steps counted at once: the same result */
void aica_trace(const char* path);  /* the registers' writes to a file, for tools/sndtrace.py */
void aica_set_chunk(int n);         /* the samples mixed at once, 16 unless set (at most 256) */
void aica_set_cpu_rate(int cycles);

#endif
