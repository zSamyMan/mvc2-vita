/*
 * The debug menu (DEBUG_OPTIONS=1 only): L + DOWN + SELECT opens and closes it. UP/DOWN pick a line, LEFT/RIGHT
 * change a setting, CROSS does a line's action, CIRCLE closes. While it is open the game gets no buttons; the game
 * goes on. Its actions are asked of the game's side (src/dc/port_debug.c), done at the start of a game frame; its
 * lines are drawn by the renderer (rd_vgl.c) over the picture.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <psp2/ctrl.h>
#include "vita_debug_menu.h"

extern int vita_debug;
extern int host_test_flags(void);
extern void host_test_flags_set(int f);

/* the game's side asks for these each frame (the program keeps its own names: it calls out, nothing calls in) */
static volatile int pending, game_state[4];
static int port_dbg_stage = -1;

int host_dbg_take(void) {
    int a = pending;
    pending = 0;
    return a;
}

int host_dbg_stage(void) {
    static int read;
    if (!read) {
        /* MVC2_STAGE=n (testing): the stage the menu starts with, so a run can go straight to one stage */
        const char* e = getenv("MVC2_STAGE");
        read = 1;
        if (e != NULL && atoi(e) >= 0 && atoi(e) <= 16) {
            port_dbg_stage = atoi(e);
        }
    }
    return port_dbg_stage;
}

void host_dbg_report(int task, int scene, int stage, int fight) {
    game_state[0] = task, game_state[1] = scene, game_state[2] = stage, game_state[3] = fight;
}

void host_dbg_burst(void) {
    extern void rd_vgl_request(int what);
    rd_vgl_request(8);      /* RD_VGL_BURST */
}

static void port_dbg_request(int action, int arg) {
    (void)arg;
    pending = action;
}

static void port_dbg_state(int* task, int* scene, int* stage, int* fight) {
    *task = game_state[0], *scene = game_state[1], *stage = game_state[2], *fight = game_state[3];
}

/* where the game is, for the status line */
static void where(char* buf, int n) {
    static const char* task[] = { "BOOT", "ATTRACT", "GAME", "MENU" };
    static const char* scene[] = { "START", "SELECT", "FIGHT", "CONTINUE", "STAFF", "OVER", "END" };
    int t, s, st, f;
    port_dbg_state(&t, &s, &st, &f);
    if (t == 2 && s >= 0 && s < 7) {
        snprintf(buf, n, "GAME %s  STAGE %d  FIGHT %d", scene[s], st, f);
    } else {
        snprintf(buf, n, "%s %d", t >= 0 && t < 4 ? task[t] : "?", s);
    }
}
extern void rd_vgl_overlay_toggle(void);
extern int rd_vgl_overlay_shown(void);

enum { IT_LIFE, IT_CPU1, IT_GAUGE, IT_STAGE, IT_BOSS, IT_STAFF, IT_LOGO, IT_ATTRACT, IT_OVERLAY, IT_CLOSE, ITEMS };

static int open, sel;
static unsigned prev;
static volatile unsigned version = 1;
static char note[40];       /* the last action's word */

static const char* on_off(int on) {
    return on ? "ON" : "OFF";
}

static void line_of(int i, char* buf, int n) {
    int f = host_test_flags();
    switch (i) {
    case IT_LIFE:
        snprintf(buf, n, "INFINITE LIFE (HUMAN)   %s", on_off(f & 8));
        break;
    case IT_CPU1:
        snprintf(buf, n, "CPU AT 1 VITAL          %s", on_off(f & 1));
        break;
    case IT_GAUGE:
        snprintf(buf, n, "HYPER GAUGE FULL        %s", on_off(f & 2));
        break;
    case IT_STAGE:
        /* one line: LEFT/RIGHT pick the stage, X applies it (to the character select, the next fight on it) */
        if (port_dbg_stage < 0) {
            snprintf(buf, n, "STAGE  < GAME'S >     X: APPLY");
        } else {
            snprintf(buf, n, "STAGE  < %2d >         X: APPLY", port_dbg_stage);
        }
        break;
    case IT_BOSS:
        snprintf(buf, n, "NEXT FIGHT: ABYSS");
        break;
    case IT_STAFF:
        snprintf(buf, n, "GO TO: STAFF ROLL");
        break;
    case IT_LOGO:
        snprintf(buf, n, "GO TO: END LOGO (CAPCOM)");
        break;
    case IT_ATTRACT:
        snprintf(buf, n, "GO TO: ATTRACT / TITLE");
        break;
    case IT_OVERLAY:
        snprintf(buf, n, "PERFORMANCE OVERLAY     %s", on_off(rd_vgl_overlay_shown()));
        break;
    case IT_CLOSE:
        snprintf(buf, n, "CLOSE");
        break;
    }
}

/* the handheld's buttons as the menu wants them, read as the renderer's SELECT shortcuts are (Ext2: the shoulder
   buttons L1, and vita-agent-use's injected L the trigger) */
unsigned dbg_menu_raw(void) {
    static const struct { unsigned sce, bit; } map[] = {
        { SCE_CTRL_UP, DBG_UP }, { SCE_CTRL_DOWN, DBG_DOWN }, { SCE_CTRL_LEFT, DBG_LEFT }, { SCE_CTRL_RIGHT, DBG_RIGHT },
        { SCE_CTRL_CROSS, DBG_CROSS }, { SCE_CTRL_CIRCLE, DBG_CIRCLE }, { SCE_CTRL_L1, DBG_L },
        { SCE_CTRL_LTRIGGER, DBG_L }, { SCE_CTRL_SELECT, DBG_SELECT },
    };
    SceCtrlData pad;
    unsigned raw = 0, i;
    if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) < 0) {
        return 0;
    }
    for (i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (pad.buttons & map[i].sce) {
            raw |= map[i].bit;
        }
    }
    return raw;
}

int dbg_menu_open(void) {
    return open;
}

/* the panel's lines (at most max, DBG_LINE_MAX bytes each), the picked one in *pick; the lines' version */
unsigned dbg_menu_text(char (*lines)[DBG_LINE_MAX], int max, int* n, int* pick) {
    int k = 0, i;
    if (k < max) snprintf(lines[k++], DBG_LINE_MAX, "DEBUG MENU      L+DOWN+SELECT");
    if (k < max) where(lines[k++], DBG_LINE_MAX);
    if (k < max) snprintf(lines[k++], DBG_LINE_MAX, "%s", note);
    for (i = 0; i < ITEMS && k < max; i++) {
        line_of(i, lines[k++], DBG_LINE_MAX);
    }
    *n = k;
    *pick = 3 + sel;
    return version;
}

static void act(int i, int dir) {
    int f = host_test_flags();
    note[0] = 0;
    switch (i) {
    case IT_LIFE:
        host_test_flags_set(f ^ 8);
        break;
    case IT_CPU1:
        host_test_flags_set(f ^ 1);
        break;
    case IT_GAUGE:
        host_test_flags_set(f ^ 2);
        break;
    case IT_STAGE:
        if (dir != 0) {
            port_dbg_stage += dir;      /* LEFT/RIGHT: the stage the next fight loads (a fight's own loads only:
                                           forcing one mid-fight crashed) */
            port_dbg_stage = port_dbg_stage < -1 ? 16 : port_dbg_stage > 16 ? -1 : port_dbg_stage;
        } else {
            port_dbg_request(1, 0);     /* X: to the character select, the next fight on that stage */
            open = 0;                   /* closed: the select's buttons are the game's again */
            snprintf(note, sizeof note, "TO THE SELECT (IN A GAME ONLY)");
        }
        break;
    case IT_BOSS:
        port_dbg_request(2, 0);
        snprintf(note, sizeof note, "THE NEXT ARCADE FIGHT IS ABYSS");
        break;
    case IT_STAFF:
        port_dbg_request(3, 0);
        snprintf(note, sizeof note, "STAFF ROLL (IN A GAME ONLY)");
        break;
    case IT_LOGO:
        port_dbg_request(4, 0);
        snprintf(note, sizeof note, "END LOGO (IN A GAME ONLY)");
        break;
    case IT_ATTRACT:
        port_dbg_request(5, 0);
        break;
    case IT_OVERLAY:
        rd_vgl_overlay_toggle();
        break;
    case IT_CLOSE:
        open = 0;
        break;
    }
}

/* each poll of the buttons (DBG_* bits): 1 while the menu is open, the game's buttons then to be dropped */
int dbg_menu_poll(unsigned raw) {
    const unsigned combo = DBG_L | DBG_DOWN | DBG_SELECT;
    unsigned edge = raw & ~prev;
    if (!vita_debug) {
        return 0;
    }
    prev = raw;
    if ((raw & combo) == combo && (edge & combo)) {
        open = !open;
        sel = 0;    /* always from its first line: a test's presses land where they mean to */
        note[0] = 0;
        version++;
        return 1;
    }
    if (!open) {
        return 0;
    }
    if (edge & DBG_UP) {
        sel = (sel + ITEMS - 1) % ITEMS;
    }
    if (edge & DBG_DOWN) {
        sel = (sel + 1) % ITEMS;
    }
    if ((edge & (DBG_LEFT | DBG_RIGHT)) && (sel <= IT_STAGE || sel == IT_OVERLAY)) {    /* settings, not actions */
        act(sel, edge & DBG_LEFT ? -1 : 1);
    }
    if (edge & DBG_CROSS) {
        act(sel, 0);
    }
    if (edge & DBG_CIRCLE) {
        open = 0;
    }
    version++;
    return 1;
}
