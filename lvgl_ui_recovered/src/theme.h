#ifndef TOON_THEME_H
#define TOON_THEME_H

/*
 * theme.h — FreeToon colour themes and design tokens.
 *
 * Four themes (settings.ui_theme, Settings > Display):
 *   UI_THEME_NIGHT  "Nacht"  refined dark navy (the original FreeToon look)
 *   UI_THEME_LIGHT  "Licht"  light tiles on a cool grey canvas
 *   UI_THEME_WARM   "Warm"   anthracite with an orange accent, a nod to the
 *                            original Eneco Toon
 *   UI_THEME_BLACK  "Zwart"  true black for OLED-like nights, dimmed text
 *
 * Code never writes a colour literal: it uses a THEME_* token, which reads the
 * active palette (ft_theme_cur). Changing theme = pointing ft_theme_cur at
 * another palette and rebuilding the screens (theme_set() + ui_refresh_theme()).
 *
 * Marketplace plugins (.so) include this header too, so the palette is part of
 * the plugin ABI (see plugin_abi.h):
 *   - ft_palette_t is APPEND-ONLY. New tokens go at the end; never reorder or
 *     remove a field. `size` tells a plugin which fields this toonui has.
 *   - Plugins reach the palette through the weak symbol ft_theme_cur. On an
 *     older toonui without it, or for a token newer than the host's palette,
 *     THEME_* falls back to the old two-colour pair keyed on
 *     settings.home_light_mode — so a plugin still needs
 *     FT_SETTINGS_FIELD(home_light_mode) in its ABI declaration.
 *   - settings.home_light_mode stays valid: 1 exactly when the Light theme is
 *     active (older plugins only know light/dark).
 */

#include <stddef.h>
#include <stdint.h>
#include "settings.h"

enum {
    UI_THEME_NIGHT = 0,
    UI_THEME_LIGHT = 1,
    UI_THEME_WARM  = 2,
    UI_THEME_BLACK = 3,
    UI_THEME_COUNT
};

typedef struct {
    uint32_t size;          /* sizeof(ft_palette_t) in the toonui that owns it */
    uint32_t id;            /* UI_THEME_* */
    uint32_t is_light;      /* 1 = dark text on light surfaces */

    /* --- surfaces ------------------------------------------------------ */
    uint32_t bg;            /* screen background */
    uint32_t card_bg;       /* tiles, cards, modal panels */
    uint32_t row_bg;        /* list rows on a card */
    uint32_t row_bg_alt;    /* alternate / recessed rows */
    uint32_t btn_bg;        /* neutral button */
    uint32_t btn_accent;    /* raised / selected neutral button */
    uint32_t off;           /* "off" state fill (toggles, lamps) */
    uint32_t border;        /* quiet 1px outlines, chart grid */

    /* --- text ----------------------------------------------------------- */
    uint32_t text_hi;       /* primary text */
    uint32_t text_dim;      /* secondary text, tile titles */
    uint32_t text_muted;    /* hints, disabled */

    /* --- legacy accents (names kept for plugins) ------------------------ */
    uint32_t warn_orange;   /* warm highlight: big temperature readouts */
    uint32_t accent_blue;   /* the theme accent (blue in Nacht, orange in Warm) */

    /* ===== appended in FreeToon 1.1 (themes) ============================ */
    uint32_t accent;        /* accent for text, icons, thin bars */
    uint32_t accent_fill;   /* accent as a button fill (with on_accent text) */
    uint32_t accent_soft;   /* tinted surface for a selected item */
    uint32_t on_accent;     /* text/icon on accent_fill */
    uint32_t icon;          /* icon tint on cards (settings grid, devices) */
    uint32_t success;       /* "ok / on / connected" text + icons */
    uint32_t success_bg;    /* confirm / connected button fill */
    uint32_t danger;        /* errors, alarm text */
    uint32_t danger_bg;     /* destructive / remove button fill */
    uint32_t warning;       /* amber warnings */
    uint32_t glow;          /* heat: burner flame, heating state */
    uint32_t info;          /* secondary cool accent (water, cooling) */
    uint32_t divider;       /* hairline separators inside cards */
    uint32_t input_bg;      /* text fields, keyboards */
    uint32_t offline;       /* unreachable devices */
    uint32_t dim_bg;        /* dim (screensaver) screen background */
    uint32_t dim_text;      /* dim screen primary text */
    uint32_t dim_text_dim;  /* dim screen secondary text */
    uint32_t classic_canvas;/* classic Toon look: canvas behind the cards */
    uint32_t classic_header;/* classic Toon look: tile header strip */
    uint32_t classic_title; /* classic Toon look: tile title text */
    uint32_t classic_accent;/* classic Toon look: the orange accent */
} ft_palette_t;

/* The active palette. Weak, so a plugin built against this header still loads
 * on a toonui that predates themes (it then uses the fallback pairs below). */
#ifdef FT_HOST
extern const ft_palette_t * ft_theme_cur;
#else
extern const ft_palette_t * ft_theme_cur __attribute__((weak));
#endif

/* FT_TOK(field, light_fallback, dark_fallback) */
#ifdef FT_HOST
/* toonui itself: ft_theme_cur is always set (theme.c). */
#define FT_TOK(f, l, d) (ft_theme_cur->f)
#else
#define FT_TOK(f, l, d)                                                         \
    ((&ft_theme_cur != NULL && ft_theme_cur != NULL &&                          \
      offsetof(ft_palette_t, f) < ft_theme_cur->size)                           \
         ? ft_theme_cur->f                                                      \
         : (settings.home_light_mode ? (uint32_t)(l) : (uint32_t)(d)))
#endif

/* --- colour tokens -------------------------------------------------------
 * The fallback pairs are the pre-1.1 light/dark values; only plugins on an
 * old toonui ever see them. */
#define THEME_BG            FT_TOK(bg,          0xe4eaf2, 0x0f1a2a)
#define THEME_CARD_BG       FT_TOK(card_bg,     0xffffff, 0x1a2a44)
#define THEME_ROW_BG        FT_TOK(row_bg,      0xf0f4f8, 0x1f3050)
#define THEME_ROW_BG_ALT    FT_TOK(row_bg_alt,  0xe8eef5, 0x152033)
#define THEME_BTN_BG        FT_TOK(btn_bg,      0xd0d8e2, 0x223344)
#define THEME_BTN_ACCENT    FT_TOK(btn_accent,  0xccdaea, 0x335577)
#define THEME_TEXT_HI       FT_TOK(text_hi,     0x1e2838, 0xffffff)
#define THEME_TEXT_DIM      FT_TOK(text_dim,    0x687c94, 0x88aabb)
#define THEME_TEXT_MUTED    FT_TOK(text_muted,  0x8898aa, 0x556677)
#define THEME_BORDER        FT_TOK(border,      0xc8d4e2, 0x335577)
#define THEME_WARN_ORANGE   FT_TOK(warn_orange, 0xd97706, 0xffcc44)
#define THEME_ACCENT_BLUE   FT_TOK(accent_blue, 0x1d4ed8, 0x44aaff)
#define THEME_OFF           FT_TOK(off,         0xd0d8e2, 0x3a4658)

#define THEME_ACCENT        FT_TOK(accent,      0x1d4ed8, 0x44aaff)
#define THEME_ACCENT_FILL   FT_TOK(accent_fill, 0x2563eb, 0x3377cc)
#define THEME_ACCENT_SOFT   FT_TOK(accent_soft, 0xdbeafe, 0x223e6a)
#define THEME_ON_ACCENT     FT_TOK(on_accent,   0xffffff, 0xffffff)
#define THEME_ICON          FT_TOK(icon,        0x2563eb, 0x9fc4e6)
#define THEME_SUCCESS       FT_TOK(success,     0x16a34a, 0x36c46b)
#define THEME_SUCCESS_BG    FT_TOK(success_bg,  0x16a34a, 0x2e6e3a)
#define THEME_DANGER        FT_TOK(danger,      0xdc2626, 0xd4574f)
#define THEME_DANGER_BG     FT_TOK(danger_bg,   0xdc2626, 0x6e3a3a)
#define THEME_WARNING       FT_TOK(warning,     0xd97706, 0xd9a23a)
#define THEME_GLOW          FT_TOK(glow,        0xe8532f, 0xff6644)
#define THEME_INFO          FT_TOK(info,        0x0284c7, 0x66bbff)
#define THEME_DIVIDER       FT_TOK(divider,     0xe1e7ee, 0x223344)
#define THEME_INPUT_BG      FT_TOK(input_bg,    0xf5f7fa, 0x0e1a2a)
#define THEME_OFFLINE       FT_TOK(offline,     0x94a3b8, 0x6e6e6e)
#define THEME_DIM_BG        FT_TOK(dim_bg,      0x000000, 0x000000)
#define THEME_DIM_TEXT      FT_TOK(dim_text,    0xffffff, 0xffffff)
#define THEME_DIM_TEXT_DIM  FT_TOK(dim_text_dim,0xbbbbbb, 0xbbbbbb)
#define THEME_CLASSIC_CANVAS FT_TOK(classic_canvas, 0xdcdcdc, 0xdcdcdc)
#define THEME_CLASSIC_HEADER FT_TOK(classic_header, 0xf0f0f0, 0xf0f0f0)
#define THEME_CLASSIC_TITLE  FT_TOK(classic_title,  0x565656, 0x565656)
#define THEME_CLASSIC_ACCENT FT_TOK(classic_accent, 0xe64f0a, 0xe64f0a)

/* 1 when the active theme puts dark text on light surfaces. For the few
 * places that pick a different asset (icon variant), not a colour. */
#define THEME_IS_LIGHT      FT_TOK(is_light, 1, 0)
/* Active theme id (UI_THEME_*). */
#define THEME_ID            FT_TOK(id, UI_THEME_LIGHT, UI_THEME_NIGHT)

/* --- shape + spacing tokens ---------------------------------------------
 * One radius per level of hierarchy, one gap. Design px (1024x600); Toon 1
 * code scales where it already does. */
#define THEME_RADIUS_PANEL  18   /* modal panels, dialogs */
#define THEME_RADIUS_TILE   14   /* home tiles, settings tiles, cards */
#define THEME_RADIUS_BTN    10   /* buttons, rows */
#define THEME_RADIUS_SMALL   6   /* chips, small badges */
#define THEME_GAP            8   /* between tiles */
#define THEME_PAD           12   /* inside a tile */
#define THEME_BAR_H          3   /* tile accent bar */

/* --- type scale ------------------------------------------------------------
 * A ~1.25 modular scale on the fonts that are compiled in (display.h SF()):
 * 14 / 18 / 22 / 28 / 36 / 48, plus the 64/96 readout subsets. Needs
 * display.h. */
#define THEME_FONT_CAPTION  SF(14)   /* units, hints, secondary lines */
#define THEME_FONT_BODY     SF(18)   /* tile titles, row labels */
#define THEME_FONT_TITLE    SF(22)   /* values on rows, button labels */
#define THEME_FONT_HEADING  SF(28)   /* screen titles, tile values */
#define THEME_FONT_LARGE    SF(36)
#define THEME_FONT_DISPLAY  SF(48)   /* big tile readouts */

/* Host-only API (theme.c). */
#ifdef FT_HOST
const ft_palette_t * theme_palette(int id);   /* NULL if out of range */
const char * theme_name(int id);              /* localised display name */
void theme_set(int id);        /* sets ui_theme + home_light_mode + palette */
void theme_sync_settings(void);/* re-derive after settings were loaded/merged */
#endif

#endif /* TOON_THEME_H */
