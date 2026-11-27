/*
 * rd_vgl.h  The PS Vita's GPU back end (vitaGL): the frame's lists drawn on a render thread of their own, by
 * rd_soft.c's rules, at the screen's 960 by 544
 */

#ifndef RD_VGL_H
#define RD_VGL_H

/* the render thread started and vitaGL with it: 0 when it could not (no libshacccg.suprx, a shader) */
int rd_vgl_init(void);
/* in place of rd_end_frame: the frame the game filled given to the render thread, rd_frame another to fill.
   Waits only while the render thread still reads the one before */
void rd_vgl_present(void);
/* asks the render thread for a picture (ux0:data/mvc_2/shots/) or the next frame's batches in the log */
enum { RD_VGL_SHOT = 1, RD_VGL_DUMP = 2, RD_VGL_PERF = 4, RD_VGL_BURST = 8 };
void rd_vgl_request(int what);
/* the first start's screen while the disc's files are unpacked: 0 to 1000 as far as it has come; below 0 hides it */
void rd_vgl_boot_progress(int permille);
/* waits (3 s at most) for that screen's first drawing, its picture read before the unpacking's writes begin */
void rd_vgl_boot_wait(void);
/* no game data: the warning screen shown from now on */
void rd_vgl_warning(void);
/* the overlay's figures: a thread's CPU share (by its id), the game's frame times from the front end */
void rd_vgl_watch_thread(const char* name, int id);
void rd_vgl_game_stats(float game_ms, float sound_ms, float worst_ms);
void rd_vgl_trace_game(unsigned frame, unsigned game_us, unsigned sound_us);

#endif
