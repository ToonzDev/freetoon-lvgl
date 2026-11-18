#ifndef TOON_CLOCK_STUDIO_H
#define TOON_CLOCK_STUDIO_H

/* Klok Studio (#55): configurable clock faces, usable as a home tile
 * (LT_CLOCKSTUDIO), as a dim block (DB_CLOCKSTUDIO) and as a full-screen view.
 * Written from scratch in LVGL (feature list inspired by the ReToon "Clock
 * Studio" app; no code or assets taken from it).
 *
 * Faces are drawn in one custom-draw object per face (no per-tick object
 * rebuilds): each second only the moved hand areas are invalidated, LED
 * digits redraw only when the minute changes. One shared 4 Hz timer drives
 * every live face that is on the active screen.
 *
 * Two independent configurations (home tile + dim block) are stored in
 * <data>/toonui_clockstudio.cfg as "home.<key>=<value>" / "dim.<key>=...".
 * Changes apply live to every face built from that configuration. */

#include "lvgl/lvgl.h"

enum { CS_HOME = 0, CS_DIM = 1, CS_NCFG };

enum {
    CS_FACE_AUTO = 0,
    CS_FACE_ANALOG,      /* classic analog: minute ticks, numerals, 3 hands */
    CS_FACE_MINIMAL,     /* minimal analog: 12 dots, slim hands */
    CS_FACE_SQUARE,      /* square analog: ticks/numerals on a rounded square */
    CS_FACE_LED,         /* LED digital: chamfered 7-segment digits */
    CS_FACE_STACKED,     /* HH over MM */
    CS_FACE_DIGITAL,     /* plain HH:MM + date */
    CS_FACE_CALENDAR,    /* clock + upcoming agenda items */
    CS_FACE_COUNT
};

enum { CS_THEME_BLUE = 0, CS_THEME_PURPLE, CS_THEME_WHITE, CS_THEME_RED,
       CS_THEME_GREEN, CS_THEME_COUNT };

/* colour slots: -1 = take the theme's colour */
enum { CS_COL_BG = 0, CS_COL_FG, CS_COL_ACCENT, CS_COL_HOUR, CS_COL_MIN,
       CS_COL_SEC, CS_COL_COUNT };

typedef struct {
    int  face;                  /* CS_FACE_* */
    int  theme;                 /* CS_THEME_* */
    int  seconds;               /* second hand / blinking colon / seconds digits */
    int  date;                  /* show the date */
    int  h12;                   /* 12-hour digital time */
    int  scale;                 /* 65..100 % of the available area */
    int  radius;                /* corner radius, design px (0 = square) */
    int  col[CS_COL_COUNT];     /* 0xRRGGBB or -1 */
    char title[24];             /* optional caption, "" = none */
} cs_cfg_t;

extern cs_cfg_t g_cs_cfg[CS_NCFG];

enum { CS_CTX_TILE = 0, CS_CTX_DIM, CS_CTX_FULL, CS_CTX_PREVIEW };

void cs_load(void);
void cs_save(void);

/* Build a face of configuration `which` at x,y,w,h inside `parent`. The
 * returned object is the face's outer box; callers may add flags / click
 * handlers to it — they survive live reconfiguration. Not clickable by
 * default (taps fall through, e.g. to wake the dim screen). */
lv_obj_t * cs_create(lv_obj_t * parent, int which, int ctx,
                     int x, int y, int w, int h);

/* Rebuild every live face of configuration `which` (after a change). */
void cs_apply(int which);

/* Full-screen view of the home configuration (tap on the home tile). */
void cs_open_fullscreen(void);

/* Settings modal on the top layer; `which` = initially selected config. */
void cs_open_settings(int which);
/* Close the settings modal (saves pending changes); no-op when closed.
 * Called with the other top-layer modals on dim / auto-home. */
void cs_close_settings(void);

const char * cs_face_name(int face);

#endif
