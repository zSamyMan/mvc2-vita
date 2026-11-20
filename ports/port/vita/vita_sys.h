/*
 * vita_sys.h  The PS Vita's part of the front end: main RAM at the SH-4's mirrors, the log, the clocks
 */

#ifndef VITA_SYS_H
#define VITA_SYS_H

/* the data folder: mvc2.cfg, disc/, roms/mvsc2/, the saves and mvc2.log (app0: is read-only) */
#define VITA_DATA_DIR "ux0:data/mvc_2/"

void vita_early_init(void);     /* first thing in main(): the folder, the log, the clocks, the abort handler */
int vita_map_ram(void);         /* main RAM at 0x8C000000, aliased at 0xAC000000 (kubridge) */
void vita_frame(void);          /* once a frame: what the abort handler saw goes to the log */
extern int vita_debug;          /* DEBUG_OPTIONS: 1 (the default) the testing tools on, 0 all off */

#endif
