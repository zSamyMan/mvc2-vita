/*
 * input_sdl.c  The players' controls: the keyboard and up to two game controllers, kept in mvc2.cfg
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdl_front.h"
#ifdef __vita__
#include <strings.h>
#include <psp2/touch.h>
#include "vita_debug_menu.h"
#endif

#include "naomi_host.h"
#include "naomi_input.h"
#include "input_sdl.h"

const INPUT_ACTION input_actions[ACTIONS] = {
    { "up", JVS_UP },      { "down", JVS_DOWN }, { "left", JVS_LEFT }, { "right", JVS_RIGHT },
    { "b1", JVS_B1 },      { "b2", JVS_B2 },     { "b3", JVS_B3 },     { "b4", JVS_B4 },
    { "b5", JVS_B5 },      { "b6", JVS_B6 },     { "start", JVS_START }, { "coin", 0 },
};

BINDING bindings[2][ACTIONS][MAX_BINDINGS];

static SDL_Gamepad* pads[2];

#define KEY(k) { BIND_KEY, SDL_SCANCODE_##k, 0 }
#define BUTTON(b) { BIND_BUTTON, SDL_GAMEPAD_BUTTON_##b, 0 }
#define AXIS(a, dir) { BIND_AXIS, SDL_GAMEPAD_AXIS_##a, dir }

/* shoulders and triggers last, so an arcade stick's two rows match the cabinet's */
static const BINDING pad_defaults[ACTIONS][4] = {
    { BUTTON(DPAD_UP), AXIS(LEFTY, -1) },
    { BUTTON(DPAD_DOWN), AXIS(LEFTY, 1) },
    { BUTTON(DPAD_LEFT), AXIS(LEFTX, -1) },
    { BUTTON(DPAD_RIGHT), AXIS(LEFTX, 1) },
    { BUTTON(WEST) },
    { BUTTON(NORTH) },
#ifdef __vita__
    /* the Vita has no triggers: assist 1 on L, assist 2 on R */
    { BUTTON(LEFT_SHOULDER) },
    { BUTTON(SOUTH) },
    { BUTTON(EAST) },
    { BUTTON(RIGHT_SHOULDER) },
#else
    { BUTTON(RIGHT_SHOULDER), BUTTON(LEFT_SHOULDER) },
    { BUTTON(SOUTH) },
    { BUTTON(EAST) },
    { AXIS(RIGHT_TRIGGER, 1), AXIS(LEFT_TRIGGER, 1) },
#endif
    { BUTTON(START) },
    { BUTTON(BACK) },
};

static const BINDING key_defaults[2][ACTIONS] = {
    { KEY(UP), KEY(DOWN), KEY(LEFT), KEY(RIGHT), KEY(A), KEY(S), KEY(D), KEY(Z), KEY(X), KEY(C), KEY(1), KEY(5) },
    { KEY(I), KEY(K), KEY(J), KEY(L), KEY(R), KEY(T), KEY(Y), KEY(F), KEY(G), KEY(H), KEY(2), { BIND_NONE } },
};

void input_defaults(int pl, int action) {
    int i, n = 0;
    memset(bindings[pl][action], 0, sizeof bindings[pl][action]);
    if (key_defaults[pl][action].type != BIND_NONE) {
        bindings[pl][action][n++] = key_defaults[pl][action];
    }
    for (i = 0; i < 4 && pad_defaults[action][i].type != BIND_NONE && n < MAX_BINDINGS; i++) {
        bindings[pl][action][n++] = pad_defaults[action][i];
    }
}

void input_init(void) {
    int pl, a;
    for (pl = 0; pl < 2; pl++) {
        for (a = 0; a < ACTIONS; a++) {
            input_defaults(pl, a);
        }
    }
    /* layouts SDL lacks */
    SDL_AddGamepadMappingsFromFile(host_save_path("gamecontrollerdb.txt"));
    input_load();
}

/* key:NAME, pad:BUTTON, or pad:AXIS+ or - */
const char* binding_name(const BINDING* b, char* buf, size_t size) {
    switch (b->type) {
    case BIND_KEY:
        snprintf(buf, size, "key:%s", SDL_GetScancodeName(b->code));
        break;
    case BIND_BUTTON:
        snprintf(buf, size, "pad:%s", SDL_GetGamepadStringForButton(b->code));
        break;
    case BIND_AXIS:
        snprintf(buf, size, "pad:%s%c", SDL_GetGamepadStringForAxis(b->code), b->dir < 0 ? '-' : '+');
        break;
    default:
        snprintf(buf, size, "none");
    }
    return buf;
}

static int binding_parse(const char* s, size_t n, BINDING* b) {
    char name[64];
    if (n < 5 || n - 4 >= sizeof name) {
        return 0;
    }
    memcpy(name, s + 4, n - 4);
    name[n - 4] = 0;
    if (strncmp(s, "key:", 4) == 0) {
        b->type = BIND_KEY;
        b->code = SDL_GetScancodeFromName(name);
        b->dir = 0;
        return b->code != SDL_SCANCODE_UNKNOWN;
    }
    if (strncmp(s, "pad:", 4) != 0) {
        return 0;
    }
    if (name[n - 5] == '+' || name[n - 5] == '-') {
        b->type = BIND_AXIS;
        b->dir = name[n - 5] == '-' ? -1 : 1;
        name[n - 5] = 0;
        b->code = SDL_GetGamepadAxisFromString(name);
        return b->code != SDL_GAMEPAD_AXIS_INVALID;
    }
    b->type = BIND_BUTTON;
    b->code = SDL_GetGamepadButtonFromString(name);
    b->dir = 0;
    return b->code != SDL_GAMEPAD_BUTTON_INVALID;
}

static void setting_name(int pl, int action, char* buf, size_t size) {
    snprintf(buf, size, "p%d %s", pl + 1, input_actions[action].name);
}

void input_load(void) {
    int pl, a;
    for (pl = 0; pl < 2; pl++) {
        for (a = 0; a < ACTIONS; a++) {
            char key[32];
            const char* v;
            int n = 0;
            setting_name(pl, a, key, sizeof key);
            v = config_get(key);
            if (v == NULL) {
                continue;
            }
            memset(bindings[pl][a], 0, sizeof bindings[pl][a]);
            /* split at ", " before key: or pad:, since a key's name can hold either */
            while (*v != 0 && n < MAX_BINDINGS) {
                const char* end = v;
                while ((end = strstr(end + 1, ", ")) != NULL && strncmp(end + 2, "key:", 4) != 0 &&
                       strncmp(end + 2, "pad:", 4) != 0) {
                }
                if (end == NULL) {
                    end = v + strlen(v);
                }
                if (binding_parse(v, end - v, &bindings[pl][a][n])) {
                    n++;
                } else {
                    memset(&bindings[pl][a][n], 0, sizeof bindings[pl][a][n]);
                }
                v = *end != 0 ? end + 2 : end;
            }
        }
    }
}

/* those changed, and those mvc2.cfg has already */
void input_save(int pl) {
    int a, i;
    for (a = 0; a < ACTIONS; a++) {
        char key[32], value[MAX_BINDINGS * 40], one[40];
        BINDING now[MAX_BINDINGS];
        size_t len = 0;
        setting_name(pl, a, key, sizeof key);
        memcpy(now, bindings[pl][a], sizeof now);
        input_defaults(pl, a);
        i = memcmp(now, bindings[pl][a], sizeof now) == 0;
        memcpy(bindings[pl][a], now, sizeof now);
        if (i && config_get(key) == NULL) {
            continue;
        }
        value[0] = 0;
        for (i = 0; i < MAX_BINDINGS && bindings[pl][a][i].type != BIND_NONE; i++) {
            len += snprintf(value + len, sizeof value - len, "%s%s", i > 0 ? ", " : "",
                            binding_name(&bindings[pl][a][i], one, sizeof one));
        }
        config_set(key, i > 0 ? value : "none");
    }
}

/* a key replaces the action's keys, a button or axis its controller's */
void input_set(int pl, int action, const BINDING* b) {
    int a, i, j;
    int pad = b->type != BIND_KEY;
    for (a = 0; a < ACTIONS; a++) {
        for (i = j = 0; i < MAX_BINDINGS && bindings[pl][a][i].type != BIND_NONE; i++) {
            const BINDING* o = &bindings[pl][a][i];
            int same = o->type == b->type && o->code == b->code && o->dir == b->dir;
            if (!same && !(a == action && (o->type != BIND_KEY) == pad)) {
                bindings[pl][a][j++] = *o;
            }
        }
        for (; j < MAX_BINDINGS; j++) {
            memset(&bindings[pl][a][j], 0, sizeof bindings[pl][a][j]);
        }
    }
    for (i = 0; i < MAX_BINDINGS - 1 && bindings[pl][action][i].type != BIND_NONE; i++) {
    }
    bindings[pl][action][i] = *b;
}

void input_event(const SDL_Event* e) {
    int i;
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) {
        SDL_JoystickID id = pad_added_id(e);
        for (i = 0; i < 2; i++) {
            if (pads[i] != NULL && SDL_GetGamepadID(pads[i]) == id) {
                return;
            }
        }
        for (i = 0; i < 2 && pads[i] != NULL; i++) {
        }
        if (i < 2) {
            pads[i] = pad_open_added(e);
            if (pads[i] != NULL) {
                fprintf(stderr, "player %d: %s\n", i + 1, SDL_GetGamepadName(pads[i]));
            }
        }
    } else if (e->type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (i = 0; i < 2; i++) {
            if (pads[i] != NULL && SDL_GetGamepadID(pads[i]) == e->gdevice.which) {
                fprintf(stderr, "player %d: controller gone\n", i + 1);
                SDL_CloseGamepad(pads[i]);
                pads[i] = NULL;
            }
        }
    }
}

SDL_Gamepad* input_pad(int pl) {
    return pads[pl];
}

int input_pad_player(SDL_JoystickID id) {
    int i;
    for (i = 0; i < 2; i++) {
        if (pads[i] != NULL && SDL_GetGamepadID(pads[i]) == id) {
            return i;
        }
    }
    return -1;
}

int input_menu_combo(int pl) {
#ifdef __vita__
    (void)pl;
    return 0;       /* the controls screen is drawn by SDL's renderer, which the Vita's GPU build has not */
#endif
    return pads[pl] != NULL && SDL_GetGamepadButton(pads[pl], SDL_GAMEPAD_BUTTON_BACK) &&
           SDL_GetGamepadButton(pads[pl], SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
}

#ifdef __vita__
/* the rear panel's upper half, its left and right (as seen from the front: the left the left hand's fingers), read
   once a frame */
static int rear_zone[2], rear_used;    /* rear_used: the scheme has the rear panel (PS2, GRIP) */

static void rear_touch_read(void) {
    static int ready, mid_x = 960, mid_y = 500;
    SceTouchData d;
    int i;
    if (!ready) {
        SceTouchPanelInfo pi;
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
        if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &pi) >= 0 && pi.maxAaX > pi.minAaX) {
            mid_x = (pi.minAaX + pi.maxAaX) / 2;
            mid_y = (pi.minAaY + pi.maxAaY) / 2;
        }
        ready = 1;
    }
    rear_zone[0] = rear_zone[1] = 0;
    if (!rear_used || sceTouchPeek(SCE_TOUCH_PORT_BACK, &d, 1) < 1) {
        return;
    }
    for (i = 0; i < (int)d.reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
        if (d.report[i].y < mid_y) {
            rear_zone[d.report[i].x < mid_x ? 0 : 1] = 1;
        }
    }
}

static void put(int pl, int a, BINDING b) {
    int i;
    for (i = 0; i < MAX_BINDINGS && bindings[pl][a][i].type != BIND_NONE; i++) {
    }
    if (i < MAX_BINDINGS) {
        bindings[pl][a][i] = b;
    }
}

/*
 * The front touch screen on the game's menus (not in a fight, unless it is paused): a drag past 40 pixels from where
 * the finger came down is the D-pad that way (again after 20 frames held, then every 8); a tap is X (light kick, the
 * menus' decide), a long press (40 frames) START, a two-finger tap O (heavy kick, back)
 */
static uint32_t touch_menu_bits(void) {
    enum { DRAG = 40, PULSE = 3, LONG = 40 };
    static int ready, down, sx, sy, moved, dir, rep, held, two, long_sent, pulse_left;
    static uint32_t pulse_bit;
    static unsigned fight_seen, pause_seen, fight_quiet = 999, pause_quiet = 999;
    SceTouchData d;
    uint32_t out = 0;
    int n;
    if (!ready) {
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
        ready = 1;
    }
    /* a fight drawn in the last 10 frames, and no pause: the screen left alone */
    fight_quiet = host_fight_frames != fight_seen ? 0 : fight_quiet + 1;
    pause_quiet = host_pause_frames != pause_seen ? 0 : pause_quiet + 1;
    fight_seen = host_fight_frames, pause_seen = host_pause_frames;
    if (fight_quiet < 10 && pause_quiet >= 10) {
        down = 0, pulse_left = 0;
        return 0;
    }
    n = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &d, 1) > 0 ? (int)d.reportNum : 0;
    if (n > 0) {
        int x = d.report[0].x / 2, y = d.report[0].y / 2, dx, dy, ax, ay, way;
        if (!down) {
            down = 1, sx = x, sy = y, moved = 0, dir = -1, rep = 0, held = 0, two = 0, long_sent = 0;
        }
        two |= n >= 2;
        held++;
        dx = x - sx, dy = y - sy, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        if ((ax > ay ? ax : ay) > DRAG) {
            way = ax > ay ? (dx > 0 ? 3 : 2) : (dy > 0 ? 1 : 0);    /* the actions: up, down, left, right */
            moved = 1;
            if (way != dir) {
                dir = way, rep = 0;
            }
            if (rep < PULSE || (rep >= 20 && (rep - 20) % 8 < PULSE)) {
                out |= input_actions[dir].bit;
            }
            rep++;
        } else {
            dir = -1;
        }
        if (!moved && !two && !long_sent && held >= LONG) {
            long_sent = 1, pulse_bit = input_actions[10].bit, pulse_left = PULSE + 1;     /* START */
        }
    } else if (down) {
        down = 0;
        if (!moved && !long_sent && held < LONG) {
            pulse_bit = input_actions[two ? 8 : 7].bit, pulse_left = PULSE + 1;          /* O or X */
        }
    }
    if (pulse_left > 0) {
        out |= pulse_bit;
        pulse_left--;
    }
    return out;
}

#define TOUCH(z) { BIND_TOUCH, z, 0 }

/*
 * The schemes (the settings menu's pictures: ports/port/vita/assets' PS2, Vita and Grip_Controls.png). The face buttons the same in
 * all: square light punch, triangle heavy punch, cross light kick, circle heavy kick. A tag is light punch and light
 * kick together (partner 1), heavy punch and heavy kick (partner 2): src/pl_com.c's pl_change_ck.
 *   VITA  L assist 1, R assist 2; the right stick up light and heavy punch, down light and heavy kick
 *   PS2   L assist 1, the rear's left assist 2, R tags partner 1, the rear's right partner 2; the right stick as VITA's
 *   GRIP  L assist 1, R assist 2; the rear's right light and heavy punch, its left light and heavy kick; the right
 *         stick up tags partner 1, down partner 2
 */
void input_scheme(const char* scheme, int right_stick) {
    enum { LP = 4, HP, A1, LK, HK, A2 };
    static const BINDING face[6] = { BUTTON(WEST), BUTTON(NORTH), { BIND_NONE }, BUTTON(SOUTH), BUTTON(EAST),
                                     { BIND_NONE } };
    const BINDING l = BUTTON(LEFT_SHOULDER), r = BUTTON(RIGHT_SHOULDER), up = AXIS(RIGHTY, -1), down = AXIS(RIGHTY, 1);
    const BINDING rear_l = TOUCH(0), rear_r = TOUCH(1);
    int ps2 = scheme != NULL && strcasecmp(scheme, "PS2") == 0;
    int grip = scheme != NULL && strcasecmp(scheme, "GRIP") == 0;
    int pl, a;
    rear_used = ps2 || grip;
    for (pl = 0; pl < 2; pl++) {
        for (a = 0; a < ACTIONS; a++) {
            input_defaults(pl, a);
        }
        for (a = LP; a <= A2; a++) {
            memset(bindings[pl][a], 0, sizeof bindings[pl][a]);
            if (key_defaults[pl][a].type != BIND_NONE) {
                put(pl, a, key_defaults[pl][a]);
            }
            if (face[a - LP].type != BIND_NONE) {
                put(pl, a, face[a - LP]);
            }
        }
        put(pl, A1, l);
        if (grip) {
            put(pl, A2, r);
            put(pl, LP, rear_r), put(pl, HP, rear_r);
            put(pl, LK, rear_l), put(pl, HK, rear_l);
            if (right_stick) {
                put(pl, LP, up), put(pl, LK, up);
                put(pl, HP, down), put(pl, HK, down);
            }
            continue;
        }
        if (ps2) {
            put(pl, A2, rear_l);
            put(pl, LP, r), put(pl, LK, r);
            put(pl, HP, rear_r), put(pl, HK, rear_r);
        } else {
            put(pl, A2, r);
        }
        if (right_stick) {
            put(pl, LP, up), put(pl, HP, up);
            put(pl, LK, down), put(pl, HK, down);
        }
    }
    host_log("input: control scheme %s, right stick %s", grip ? "GRIP" : ps2 ? "PS2" : "VITA", right_stick ? "on" : "off");
}
#endif

int input_held(int pl, const BINDING* b) {
    switch (b->type) {
#ifdef __vita__
    case BIND_TOUCH:
        return pl == 0 && b->code >= 0 && b->code < 2 && rear_zone[b->code];
#endif
    case BIND_KEY:
        return key_held(b->code);
    case BIND_BUTTON:
        return pads[pl] != NULL && SDL_GetGamepadButton(pads[pl], b->code);
    case BIND_AXIS:
        return pads[pl] != NULL && SDL_GetGamepadAxis(pads[pl], b->code) * b->dir > 16384;
    }
    return 0;
}

void input_read(HOST_INPUT* in) {
    static int coin_held;
    int pl, a, i, coin = 0;
#ifdef __vita__
    rear_touch_read();
    in->player[0] |= touch_menu_bits();
#endif
    for (pl = 0; pl < 2; pl++) {
        for (a = 0; a < ACTIONS; a++) {
            for (i = 0; i < MAX_BINDINGS && bindings[pl][a][i].type != BIND_NONE; i++) {
                if (input_held(pl, &bindings[pl][a][i])) {
                    if (input_actions[a].bit != 0) {
                        in->player[pl] |= input_actions[a].bit;
                    } else {
                        coin = 1;
                    }
                }
            }
        }
    }
    for (pl = 0; pl < 2; pl++) {
        if (input_menu_combo(pl)) {
            coin_held = 1;          /* not from the controls screen's buttons */
        }
    }
    in->coin = coin && !coin_held;
    coin_held = coin;
#ifdef __vita__
    /* the debug menu (DEBUG_OPTIONS=1): L + DOWN + SELECT; while it is open the game gets no buttons */
    if (dbg_menu_poll(dbg_menu_raw())) {
        in->player[0] = in->player[1] = 0;
        in->coin = 0;
    }
#endif
}
