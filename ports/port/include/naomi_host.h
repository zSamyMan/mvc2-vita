/*
 * naomi_host.h  What the port supplies in place of the NAOMI board and Sega's libraries
 */

#ifndef NAOMI_HOST_H
#define NAOMI_HOST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* mapped at its arcade address: the game keeps buffers there. The Vita has no memory for a program
   there, so it maps it at the SH-4's P1 mirror, 0x8C000000 (and P2's, 0xAC000000, as an alias), and the
   game's C and tables are built with their RAM addresses moved there (port/tools/vita_rewrite.py) */
#ifdef __vita__
#define NAOMI_RAM_BASE  0x8C000000u
#else
#define NAOMI_RAM_BASE  0x0C000000u
#endif
#define NAOMI_RAM_SIZE  0x02000000u

/* an address the game handed over: on the Vita one in the 0x0C mirror (from a file it loaded) moved to the
   mirror main RAM is mapped at; elsewhere as it is */
#ifdef __vita__
static inline void* host_ram_ptr(const void* p) {
    uintptr_t a = (uintptr_t)p;
    return (void*)(a - 0x0C000000u < NAOMI_RAM_SIZE ? a - 0x0C000000u + NAOMI_RAM_BASE : a);
}
void vita_relocate_file(void* data, size_t size);  /* port/vita/vita_sys.c: a model file's pointers */
#else
#define host_ram_ptr(p) ((void*)(p))
#endif

/* a word of the program's data that points at what the host defines (port/tools/gen_host.py) */
typedef struct {
    char* at;
    char* to;
} HOST_RELOC;

extern const HOST_RELOC host_reloc[];

/* .data and .bss, between port/game/span_begin.c and span_end.c */
typedef struct {
    char* data;
    char* data_end;
    char* bss;
    char* bss_end;
} HOST_SPAN;

/* port/game/program.c */
typedef struct {
    void (*main)(void);
    const HOST_RELOC* reloc;
    HOST_SPAN span;
} HOST_PROGRAM;

extern const HOST_PROGRAM arc_program;
extern const HOST_PROGRAM dc_program;

/* the cartridge's ROM as one address space (naomi_rom.c) */
int naomi_rom_open(const char* path);
extern uint32_t naomi_rom_cache_bytes;      /* the most the ROM cache holds (the 3DS front end sets it) */
const char* naomi_rom_check(const char* path);  /* a file missing or the wrong size, or NULL */
int naomi_rom_read(uint32_t offset, void* dst, uint32_t size);   /* 0 past the end */
uint32_t naomi_rom_size(void);
void naomi_rom_close(void);
/* a front end's own reads of the ROM's 64 KB blocks, from the image naomi_rom_make_image writes */
typedef struct {
    int (*open)(const char* dir);       /* 1: it reads the ROM in dir (else the files are read as usual) */
    int (*read)(uint32_t block, uint32_t last, void* dst);  /* a block the cache lacks, now: 1 when read; the
                                                               request it is in runs to last */
    void (*missed)(uint32_t block, uint32_t last);  /* after one: the request it was in runs to last */
    void (*close)(void);
} NAOMI_ROM_READER;
void naomi_rom_set_reader(const NAOMI_ROM_READER* reader);
int naomi_rom_make_image(const char* dir, const char* path, void (*progress)(uint32_t done, uint32_t total));
const char* naomi_rom_image_name(void);
int naomi_rom_cached(uint32_t block);
void naomi_rom_put(uint32_t block, const void* data);   /* a block read ahead, into the cache */
/* the set's files from its zip (naomi_zip.c) */
typedef struct NAOMI_ZIP NAOMI_ZIP;
NAOMI_ZIP* naomi_zip_open(const char* path);    /* NULL if no zip */
int naomi_zip_has(const NAOMI_ZIP* zip, const char* name, uint32_t size);
int naomi_zip_read(NAOMI_ZIP* zip, const char* name, uint8_t* dst, uint32_t size,
                   void (*step)(uint32_t done));   /* 1 when read, CRC checked; step is called as it goes */
void naomi_zip_close(NAOMI_ZIP* zip);
void naomi_m1_read(uint32_t src, uint8_t* dst, uint32_t size);     /* a packed file (naomi_m1.c) */

/* the board (naomi_board.c) */
int naomi_map_ram(void);
int naomi_init(const HOST_PROGRAM* program, const char* rom_path);
void board_start(const HOST_PROGRAM* program);  /* a program loaded in RAM */
void board_at_stop(void (*fn)(void));
void board_at_frame(void (*fn)(void));  /* before the game runs */
void board_stop(void);              /* the game and what it holds let go, RAM cleared */
void naomi_run_frame(void);         /* until the game waits for the next vertical blank */
void host_poll(void);               /* from the game's busy waits: the interrupts happen here */
void host_stub(const char* name);   /* a library routine the port lacks: says so once */
void host_stub_report(void);
unsigned host_frame(void);
/* the game's pixels seen past each side of its 640 (ASPECT_RATIO=WIDE on the Vita: 103.5; else 0): the models
   culled against the wider window (nl_obj.c), the fight's gauges moved out to the corners */
extern float host_wide_x;
extern uint32_t host_fills;         /* counts the port's writes into the game's memory: ROM and disc reads */
void host_log(const char* fmt, ...);
void host_divzero_install(void);    /* a division by zero gives 0, as SHC's code did */
struct tm;
void host_clock(struct tm* tm);
extern int host_unlock;             /* unlock = all */
extern int host_freeplay;           /* freeplay = on */
void* host_user_eeprom(void);       /* the game's settings, as the EEPROM keeps them (nl_am.c), or NULL */
void* host_user_backup(void);       /* and its book-keeping, as the backup memory does */
void nlaUpdateUserEEPROM(void);     /* each saved to its file */
void nlaUpdateUserBackup(void);

/* the Dreamcast (port/dc), from the disc's files in a folder */
int dc_init(const HOST_PROGRAM* program, const char* disc_path);
int dc_gd_open(const char* disc_path);
const char* dc_disc_check(const char* disc_path);  /* a file missing or the wrong size, or NULL (or an image there) */
const char* dc_disc_missing(const char* disc_path);   /* the same, the files alone (or an unfinished unpacking) */
#define DC_UNPACK_JOURNAL "_unpack.txt"     /* in the disc's folder while its files are unpacked (dc_image.c) */
/* a disc file's path in the disc folder: the root's files in 32 subfolders by a hash of the name when buckets */
void dc_disc_path(char* out, size_t n, const char* dir, const char* name, int buckets);
int dc_disc_buckets(const char* dir);   /* 1 when the folder is laid out so */
const char* dc_image_find(const char* disc_path);     /* a disc image's name there (dc_image.c), or NULL */
/* the disc's files from that image, into disc_path, with progress(done, total) in KB; NULL when done, else why not */
const char* dc_image_unpack(const char* disc_path, void (*progress)(uint32_t done, uint32_t total));
/* the image's own files in dir (the image, a .gdi's or .cue's tracks); the image deleted once unpacked; the image
   moved from one folder to another on the same card */
int dc_image_files(const char* dir, const char* image, char (*out)[256], int max);
const char* dc_image_unpack_to(const char* dir, const char* out_dir, void (*progress)(uint32_t done, uint32_t total));
void dc_image_remove(const char* dir);
int dc_image_move(const char* from, const char* to);
long dc_gd_read_file(const char* name, void* buf, long max);  /* a whole file; its size, or -1 */
const char* dc_gd_path(const char* name);   /* a file's path on the host */
const uint8_t* dc_gd_head(const char* name, uint32_t* len);  /* Vita: a music file's first bytes, kept in memory */

/* the 3DS's read-ahead of one file (ctr_stream.c) */
int ctr_stream_open(const char* path, uint32_t most);
void ctr_stream_drop(void);
void ctr_stream_close(void);
int ctr_stream_queue(uint32_t off, uint32_t len);
int ctr_stream_put(uint32_t off, uint32_t len, const void* bytes);
int ctr_stream_find(uint32_t off, uint32_t n);
const uint8_t* ctr_stream_take(int i, uint32_t* off);
void ctr_stream_free(int i);
uint32_t ctr_stream_times(float* mean, float* most);   /* the reads since the last call; mean and most in ms */
int ctr_rom_dma(uint32_t offset, void* dst, uint32_t size);   /* the game's DMA from the ROM, 1 when read (ctr_rom.c) */
int ctr_rom_packed(uint32_t offset, void* dst, uint32_t size); /* and its packed reads, 1 when given */
/* and its disc reader, one read at a time (MVC2_GD_ASYNC) */
int ctr_read_start(FILE* f, long off, void* to, size_t want);
int ctr_read_busy(void);
size_t ctr_read_end(float* ms);
int ctr_ahead_start(const char* path, size_t most);
int ctr_ahead_has(const char* path);
int ctr_ahead_idle(void);
FILE* ctr_ahead_file(const char* path, long* size);
int ctr_ahead_take(size_t off, void* to, size_t want, float* ms);
void ctr_ahead_drop(int all);
void ctr_close_later(FILE* f);
int ctr_small_start(const char* path, size_t most);
FILE* ctr_small_open(const char* path, long* size);
int ctr_small_take(const char* path, size_t off, void* to, size_t want);
void ctr_small_drop(void);

/* port/sys/restart.c */
void host_keep_start(void);         /* first thing, before any of them runs */
void host_keep_program(const HOST_PROGRAM* program);   /* before it first runs */
void host_restart(void);            /* with the board stopped */
extern void (*host_restart_hook)(int after);   /* the front end's settings in the layers taken before it (0), given back after (1) */
void* host_keep_block(size_t size); /* memory kept through restarts (the sound's RAM), or NULL */

/* the front end (main_sdl.c, main_3ds.c) */
void host_present(void);
int pick_game(int argc, char** argv);   /* start.c: from the data found */
const char* pick_message(void);         /* why none can run ("" when the player quit) */
const char* config_get(const char* key);             /* mvc2.cfg's, or NULL */
const char* host_save_path(const char* name);
void config_set(const char* key, const char* value); /* and saves mvc2.cfg */
int start_game(void);
int switch_game(void);              /* to the other one, when its data was found */
const char* game_title(void);
/* memory the caller can do without: NULL when there is none. On the 3DS any other allocation that fails stops
   the program with host_out_of_memory. */
void* host_try_malloc(size_t bytes);
void* host_try_calloc(size_t n, size_t size);
void host_out_of_memory(const char* what, size_t bytes);   /* says so, and the program ends */

#endif
