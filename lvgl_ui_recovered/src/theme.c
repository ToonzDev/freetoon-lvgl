/*
 * theme.c — the four FreeToon palettes (see theme.h).
 *
 * Design notes, so later tweaks stay coherent:
 *  - Every theme has the same structure: canvas (bg) < card < row, with the
 *    steps kept small so tiles read as calm surfaces, not boxes with borders.
 *  - One accent per theme. Tile identity colours (Energy purple, Water blue …)
 *    stay as they are; everything interactive uses the accent.
 *  - Text is never pure white on the dark themes: slightly warm/cool off-white
 *    is easier at night and on the Toon's panel.
 *  - Zwart is for a dark bedroom/hallway: true black canvas (backlight bleed
 *    is the only light), the lowest-contrast text that still reads at 2 m.
 */
#define FT_HOST 1
#include "theme.h"
#include "i18n.h"
#include "icons.h"      /* sun_times_local */
#include <time.h>

static const ft_palette_t s_palettes[UI_THEME_COUNT] = {
    [UI_THEME_NIGHT] = {
        .size = sizeof(ft_palette_t), .id = UI_THEME_NIGHT, .is_light = 0,
        .bg = 0x0e1828, .card_bg = 0x182739, .row_bg = 0x1e3048, .row_bg_alt = 0x132134,
        .btn_bg = 0x23354c, .btn_accent = 0x2f4a6a, .off = 0x33445a, .border = 0x2a3d56,
        .text_hi = 0xf2f6fa, .text_dim = 0x8fa8c0, .text_muted = 0x7d91a8,
        .warn_orange = 0xffc94d, .accent_blue = 0x4aa8ff,
        .accent = 0x4aa8ff, .accent_fill = 0x2f74c8, .accent_soft = 0x1f3d60,
        .on_accent = 0xffffff, .icon = 0x9cc3e8,
        .success = 0x45c97e, .success_bg = 0x285c3d,
        .danger = 0xf0625a, .danger_bg = 0x6a3134,
        .warning = 0xf2b33d, .glow = 0xff6a48, .info = 0x66bbff,
        .divider = 0x22344b, .input_bg = 0x0f1b2b, .offline = 0x6b7482,
        .dim_bg = 0x04080f, .dim_text = 0xeef2f7, .dim_text_dim = 0x8396ac,
        .classic_canvas = 0x0e1828, .classic_header = 0x1e3048,
        .classic_title = 0x8fa8c0, .classic_accent = 0x4aa8ff,
    },
    [UI_THEME_LIGHT] = {
        .size = sizeof(ft_palette_t), .id = UI_THEME_LIGHT, .is_light = 1,
        .bg = 0xe6ebf1, .card_bg = 0xffffff, .row_bg = 0xf2f5f9, .row_bg_alt = 0xe9eef4,
        .btn_bg = 0xdae2eb, .btn_accent = 0xcbd9ea, .off = 0xd3dbe4, .border = 0xd3dce6,
        .text_hi = 0x1b2533, .text_dim = 0x5b6d84, .text_muted = 0x66768a,
        .warn_orange = 0xc06a04, .accent_blue = 0x1d5fd1,
        .accent = 0x1d5fd1, .accent_fill = 0x2266d8, .accent_soft = 0xdae6fa,
        .on_accent = 0xffffff, .icon = 0x2563eb,
        .success = 0x13803f, .success_bg = 0x1f9d55,
        .danger = 0xd42f2f, .danger_bg = 0xd93a3a,
        .warning = 0xa85c00, .glow = 0xe8532f, .info = 0x0b7cc1,
        .divider = 0xe1e7ee, .input_bg = 0xf5f7fa, .offline = 0x9aa5b4,
        .dim_bg = 0xf1f4f8, .dim_text = 0x1b2533, .dim_text_dim = 0x5b6d84,
        /* the original Eneco Toon colours */
        .classic_canvas = 0xdcdcdc, .classic_header = 0xf0f0f0,
        .classic_title = 0x565656, .classic_accent = 0xe64f0a,
    },
    [UI_THEME_WARM] = {
        .size = sizeof(ft_palette_t), .id = UI_THEME_WARM, .is_light = 0,
        .bg = 0x1c1c1e, .card_bg = 0x29292c, .row_bg = 0x313135, .row_bg_alt = 0x242427,
        .btn_bg = 0x37373b, .btn_accent = 0x4a4038, .off = 0x3d3c3b, .border = 0x3b3a39,
        .text_hi = 0xf6f2ec, .text_dim = 0xb2aa9f, .text_muted = 0x9a9288,
        .warn_orange = 0xffb257, .accent_blue = 0xff7a2e,
        .accent = 0xff7a2e, .accent_fill = 0xc4511a, .accent_soft = 0x4a2f1f,
        .on_accent = 0xffffff, .icon = 0xffa267,
        .success = 0x6cc787, .success_bg = 0x2f5a3c,
        .danger = 0xf0624f, .danger_bg = 0x652c25,
        .warning = 0xf4b740, .glow = 0xff6a3d, .info = 0x7fb8e0,
        .divider = 0x333336, .input_bg = 0x222225, .offline = 0x77736e,
        .dim_bg = 0x111112, .dim_text = 0xf2ece4, .dim_text_dim = 0x968d83,
        .classic_canvas = 0x1c1c1e, .classic_header = 0x313135,
        .classic_title = 0xb2aa9f, .classic_accent = 0xc4511a,
    },
    [UI_THEME_BLACK] = {
        .size = sizeof(ft_palette_t), .id = UI_THEME_BLACK, .is_light = 0,
        .bg = 0x000000, .card_bg = 0x0f0f11, .row_bg = 0x17171a, .row_bg_alt = 0x0a0a0c,
        .btn_bg = 0x1c1c20, .btn_accent = 0x22303f, .off = 0x26272b, .border = 0x232328,
        .text_hi = 0xe2e4e8, .text_dim = 0x8a8f98, .text_muted = 0x80848c,
        .warn_orange = 0xdcae52, .accent_blue = 0x5e9fd8,
        .accent = 0x5e9fd8, .accent_fill = 0x2b5d91, .accent_soft = 0x13253a,
        .on_accent = 0xf0f4f8, .icon = 0x7fa9d4,
        .success = 0x4fae72, .success_bg = 0x1d3d29,
        .danger = 0xd65a52, .danger_bg = 0x431d1b,
        .warning = 0xd4a23c, .glow = 0xe0603c, .info = 0x5e9fd8,
        .divider = 0x18181b, .input_bg = 0x0a0a0c, .offline = 0x5a5d63,
        .dim_bg = 0x000000, .dim_text = 0xc4c7cc, .dim_text_dim = 0x7a7f87,
        .classic_canvas = 0x000000, .classic_header = 0x17171a,
        .classic_title = 0x8a8f98, .classic_accent = 0xd9682c,
    },
};

const ft_palette_t * ft_theme_cur = &s_palettes[UI_THEME_NIGHT];

const ft_palette_t * theme_palette(int id) {
    return (id >= 0 && id < UI_THEME_COUNT) ? &s_palettes[id] : NULL;
}

const char * theme_name(int id) {
    switch (id) {
        case UI_THEME_LIGHT: return tr("Licht", "Light");
        case UI_THEME_WARM:  return tr("Warm", "Warm");
        case UI_THEME_BLACK: return tr("Zwart", "Black");
        default:             return tr("Nacht", "Night");
    }
}

/* Theme the automatic mode wants now, or -1 when it is off. Periods run
 * day [morning, evening), evening [evening, night), night [night, morning);
 * with the sun, morning = sunrise and evening = sunset. */
int theme_auto_pick(void) {
    if (settings.theme_auto != 1 && settings.theme_auto != 2) return -1;
    int morning = settings.theme_auto_morning, evening = settings.theme_auto_evening;
    if (settings.theme_auto == 2) {
        int rise, set;
        if (sun_times_local(&rise, &set) == 0) { morning = rise; evening = set; }
    }
    int night = settings.theme_auto_night;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    int m = tm.tm_hour * 60 + tm.tm_min;
    /* minutes since `from`, wrapping past midnight */
    #define SINCE(from) ((m - (from) + 1440) % 1440)
    int sd = SINCE(morning), se = SINCE(evening), sn = SINCE(night);
    #undef SINCE
    /* the period whose start is most recent wins */
    int day_theme = (settings.theme_auto_day == UI_THEME_WARM) ? UI_THEME_WARM : UI_THEME_LIGHT;
    if (sd <= se && sd <= sn) return day_theme;
    if (se <= sn)             return UI_THEME_NIGHT;
    return UI_THEME_BLACK;
}

void theme_set(int id) {
    if (id < 0 || id >= UI_THEME_COUNT) id = UI_THEME_NIGHT;
    settings.ui_theme        = id;
    settings.home_light_mode = (id == UI_THEME_LIGHT);   /* plugin ABI: 1 only for Light */
    ft_theme_cur = &s_palettes[id];
}

/* Settings come from toonui.cfg, the web API or a client-mode master, any of
 * which may only know the old home_light_mode switch. Reconcile the two:
 * home_light_mode=1 always means Light; home_light_mode=0 with ui_theme=Light
 * means someone switched light mode off the old way → back to Night. */
void theme_sync_settings(void) {
    int id = settings.ui_theme;
    if (id < 0 || id >= UI_THEME_COUNT) id = UI_THEME_NIGHT;
    if (settings.home_light_mode && id != UI_THEME_LIGHT) id = UI_THEME_LIGHT;
    else if (!settings.home_light_mode && id == UI_THEME_LIGHT) id = UI_THEME_NIGHT;
    theme_set(id);
}
