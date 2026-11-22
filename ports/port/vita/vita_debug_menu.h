/* the debug menu (vita_debug_menu.c): the buttons it reads, as the input's poll hands them over */
#ifndef VITA_DEBUG_MENU_H
#define VITA_DEBUG_MENU_H

#define DBG_UP      0x001
#define DBG_DOWN    0x002
#define DBG_LEFT    0x004
#define DBG_RIGHT   0x008
#define DBG_CROSS   0x010
#define DBG_CIRCLE  0x020
#define DBG_L       0x040
#define DBG_SELECT  0x080

#define DBG_LINE_MAX 48

int dbg_menu_poll(unsigned raw);    /* 1 while open: the game's buttons dropped */
int dbg_menu_open(void);
unsigned dbg_menu_raw(void);        /* the handheld's buttons, DBG_* bits */
unsigned dbg_menu_text(char (*lines)[DBG_LINE_MAX], int max, int* n, int* pick);

#endif
