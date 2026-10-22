/*
 * Home screen — tile grid matching qt-gui's layout.
 * Big thermostat tile top-left (240*2+gap = 500 wide, 220*2+gap = 460 tall).
 * Smaller tiles laid out 4 per row to its right and below.
 *
 * Tap a tile to navigate to its detail screen.
 */
#include "screens.h"
#include "i18n.h"
#include "display.h"   /* SX()/SY() scaling for Toon 1 (800x480) vs Toon 2 (1024x600) */
#include "video.h"
#include "inbox.h"
#include "boxtalk.h"
#ifdef WASM_BUILD
#  include <emscripten.h>   /* EM_ASM (Life360 map JS-fetch path) */
#endif
#include "icons.h"
#include "homewizard.h"
#include "energy.h"
#include "meteradapter.h"
#include "settings.h"
#include "weather.h"
#include "wastecollection.h"
#include "ventilation.h"
#include "homeassistant.h"
#include "client_link.h"
#include "news.h"
#include "calendar.h"
#include "packages.h"
#include "tile_slots.h"
#include "layout.h"
#include "update_check.h"
#include "pin_modal.h"
#include "update_modal.h"
#include "efanlamp.h"
#include "sonos.h"
#include "domoticz.h"
#include "templogger.h"
#include "screen_chart_24h.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define COL_BG          (settings.home_light_mode ? 0xe4eaf2 : 0x0f1a2a)
#define COL_TILE_BG     (settings.home_light_mode ? 0xffffff : 0x1a2a44)
#define COL_TILE_ACCENT (settings.home_light_mode ? 0xccdaea : 0x335577)
#define COL_TEXT_HI     (settings.home_light_mode ? 0x1e2838 : 0xffffff)
#define COL_TEXT_DIM    (settings.home_light_mode ? 0x687c94 : 0x88aabb)
#define COL_TEMP_YELLOW (settings.home_light_mode ? 0xd97706 : 0xffcc44)
#define COL_BURNER_RED  0xff6644

static lv_obj_t * scr_root = NULL;

/* Clock / thermostat tile widgets — updated by refresh timer */
static lv_obj_t * lbl_t_clock_h = NULL;
static lv_obj_t * lbl_t_clock_colon = NULL;   /* pulsing ":" overlay */
static lv_obj_t * lbl_t_clock_m = NULL;
static lv_obj_t * lbl_t_temp;
static int th_is_compact = 0;
static lv_obj_t * lbl_t_setpoint;
static lv_obj_t * lbl_t_burner;
static lv_obj_t * lbl_t_pressure;
static lv_obj_t * lbl_t_humidity;
static lv_obj_t * lbl_t_ppm;
static lv_obj_t * lbl_t_tvoc;
static lv_obj_t * lbl_t_date;
static lv_obj_t * moon_phase_img;  /* small moon-phase widget next to date */
static lv_obj_t * lbl_t_aq;
static lv_obj_t * lbl_waste_date;
static lv_obj_t * lbl_waste_type;
static lv_obj_t * lbl_waste_date_2;
static lv_obj_t * lbl_waste_type_2;
static lv_obj_t * lbl_waste_date_3 = NULL;
static lv_obj_t * lbl_waste_type_3 = NULL;
static lv_obj_t * waste_icon_1;
static lv_obj_t * waste_icon_2;
static lv_obj_t * waste_icon_3 = NULL;
/* Pressure-warning banner — covers the top of the Heater tile when the
 * boiler's CH water pressure falls into the "low" / "very low" zones.
 * Hidden when pressure is OK or unknown. */
static lv_obj_t * pressure_banner = NULL;
static lv_obj_t * pressure_banner_lbl = NULL;
static lv_obj_t * lbl_t_program;     /* small "Home (temp)" hint under buttons */
/* Mode toggle on the heater tile — two side-by-side buttons. Manual taps
 * to permanent hold; Program taps to resume schedule. Active mode gets a
 * white border so the user can read state at a glance. */
static lv_obj_t * tile_btn_mode_manual  = NULL;
static lv_obj_t * tile_btn_mode_program = NULL;
/* Four small one-tap preset buttons sitting just below the mode toggle —
 * Comfort / Home / Sleep / Away. Each fires boxtalk_set_program(N) so the
 * Toon jumps straight to that preset (and the schedule keeps progressing
 * from there). Active preset gets a white border. */
static lv_obj_t * tile_btn_preset[4] = {0};
static lv_obj_t * tile_img_flame;
static lv_obj_t * tile_img_faucet;
static lv_obj_t * tile_img_drop;
/* Live water-flow indicator — small drop + "X L/min" on the home tile,
 * visible whenever any tap is open (HomeWizard reports flow > 0.05). */
static lv_obj_t * tile_img_water = NULL;
static lv_obj_t * tile_lbl_water = NULL;
/* Domoticz switch buttons on LT_LIGHTS tile */
static lv_obj_t * tile_lights_lbl_default = NULL;
static lv_obj_t * tile_lights_btn1 = NULL;
static lv_obj_t * tile_lights_lbl1_name = NULL;
static lv_obj_t * tile_lights_sw1 = NULL;
static lv_obj_t * tile_lights_lbl1_st = NULL;
static lv_obj_t * tile_lights_btn2 = NULL;
static lv_obj_t * tile_lights_lbl2_name = NULL;
static lv_obj_t * tile_lights_sw2 = NULL;
static lv_obj_t * tile_lights_lbl2_st = NULL;
static char       g_current_rot_id[48] = "";

/* Bottom-row labels (Energy/Weather/Waste) — updated by refresh_cb */
static lv_obj_t * lbl_bot_energy;
static lv_obj_t * lbl_bot_weather;
static lv_obj_t * lbl_bot_waste;
static lv_obj_t * lbl_inbox_main;
static lv_obj_t * lbl_inbox_sub;
static lv_obj_t * water_spinner;
static lv_obj_t * forecast_box;
static lv_obj_t * lbl_outside_main;
static lv_obj_t * lbl_outside_sub;
static lv_obj_t * fc_day_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_temp_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_wind_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_desc_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_wind_arrow[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_icon[WEATHER_FORECAST_DAYS];
/* Set when the forecast tile is too short for the (un-zoomable) weather icons
 * — e.g. a grid-placed "Weer" tile only 1 row tall on the 480-px Toon 1 panel.
 * In that case the column renders compact/text-only (time + temp + wind, no
 * icon) so nothing overlaps. set_forecast_icon() honours this to keep the
 * icon hidden across refreshes. */
static int fc_compact = 0;
static lv_obj_t * envelope_btn;
static lv_obj_t * envelope_badge;
static lv_obj_t * envelope_badge_lbl;
static lv_obj_t * lbl_forecast_city = NULL;
static lv_obj_t * lbl_life360_a = NULL;
static lv_obj_t * lbl_life360_b = NULL;
static lv_obj_t * lbl_life360_c = NULL;
/* Bicolor partly-cloudy overlay — yellow sun layered on top of the
 * white cloud for 'b'/'j' Buienradar codes. Hidden for all other codes;
 * set_forecast_icon() manages visibility + src + recolor. */
static lv_obj_t * fc_icon_sun[WEATHER_FORECAST_DAYS];

/* Buienradar uses a single-letter code for daytime weather and a
 * double-letter code ("aa", "bb", "rr", "nn") for the same condition
 * at night. We detect the double-letter as "night-time" and substitute
 * a moon-flavoured rendering: clear nights become a plain moon icon,
 * partly-cloudy nights become moon-behind-cloud (instead of sun-behind-
 * cloud). Other conditions keep their daytime appearance — at night,
 * rain and snow still LOOK like rain and snow. */
static int is_night_code(const char * letter) {
    if (!letter || !letter[0] || !letter[1]) return 0;
    return letter[0] == letter[1];   /* "aa","bb","rr","nn" → night variant */
}

static void set_forecast_icon(lv_obj_t * cloud, lv_obj_t * sun,
                              const char * letter) {
    if (!cloud) return;
    int is_night   = is_night_code(letter);
    int is_partly  = letter && (letter[0] == 'b' || letter[0] == 'j');
    int is_thunder = letter && (letter[0] == 'g' || letter[0] == 'm');
    int is_clear   = letter && letter[0] == 'a';

    /* Clear sky at night → moon icon, no overlay needed. */
    if (is_night && is_clear) {
        lv_img_set_src(cloud, moon_phase_icon(40));
        lv_obj_set_style_img_recolor(cloud, lv_color_hex(settings.home_light_mode ? 0x485868 : 0xe8edf2), 0);   /* moon white / moon slate */
        if (sun) lv_obj_add_flag(sun, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (is_partly) {
        /* Partly cloudy — white cloud + sun overlay (day) or moon overlay
         * (night). The night variant ("bb") swaps the yellow sun for a
         * pale moon so the slot doesn't render a sun glyph at midnight. */
        lv_img_set_src(cloud, weather_icon_for("d"));            /* plain cloud */
        lv_obj_set_style_img_recolor(cloud, lv_color_hex(settings.home_light_mode ? 0x64748b : 0xf0f4f8), 0);
        if (sun) {
            if (is_night) {
                lv_img_set_src(sun, moon_phase_icon(40));
                lv_obj_set_style_img_recolor(sun, lv_color_hex(settings.home_light_mode ? 0x485868 : 0xe8edf2), 0);
            } else {
                lv_img_set_src(sun, weather_icon_for("a"));      /* plain sun */
                lv_obj_set_style_img_recolor(sun, lv_color_hex(settings.home_light_mode ? 0xd97706 : 0xffd24a), 0);
            }
            lv_obj_set_style_img_recolor_opa(sun, 255, 0);
            lv_obj_clear_flag(sun, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (is_thunder) {
        /* Thunder — dark-gray storm cloud + yellow bolt overlay.
         * Single-recolor would have made the whole pictogram amber and
         * lost the "stormy" reading. Use the bolt-only icon (no cloud
         * portion) overlaid on a separately-recolored gray cloud. */
        lv_img_set_src(cloud, weather_icon_for("d"));            /* plain cloud */
        lv_obj_set_style_img_recolor(cloud, lv_color_hex(settings.home_light_mode ? 0x475569 : 0x6a7888), 0);   /* stormy gray */
        if (sun) {
            lv_img_set_src(sun, &icon_wx_bolt);
            lv_obj_set_style_img_recolor(sun, lv_color_hex(settings.home_light_mode ? 0xd97706 : 0xffd24a), 0);
            lv_obj_set_style_img_recolor_opa(sun, 255, 0);
            lv_obj_clear_flag(sun, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_img_set_src(cloud, weather_icon_for(letter));
        uint32_t c = weather_icon_color_for(letter);
        if (settings.home_light_mode && (c == 0xc8d4e0 || c == 0xe8f0ff || c == 0xf0f4f8 || c == 0x9aa6b3)) {
            c = (c == 0xe8f0ff) ? 0x3b82f6 : 0x64748b;
        } else if (settings.home_light_mode && (c == 0xffd24a || c == 0xf2c66a)) {
            c = 0xd97706;
        }
        lv_obj_set_style_img_recolor(cloud, lv_color_hex(c), 0);
        if (sun) lv_obj_add_flag(sun, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Nearest enabled Montserrat font for a target pixel height (enabled sizes:
 * 12,14,18,20,22,…). Scales the forecast text when the tile is resized smaller
 * than the native full-width strip. Clamped to [12,22]. */
static const lv_font_t * font_for(int px) {
    if (px >= 22) return &lv_font_montserrat_22;
    if (px >= 20) return &lv_font_montserrat_20;
    if (px >= 18) return &lv_font_montserrat_18;
    if (px >= 14) return &lv_font_montserrat_14;
    return &lv_font_montserrat_12;
}

/* Whole-tile pointers so refresh_cb can flip their offline labels or
 * hide them entirely based on the matching enable_* flag +
 * settings.hide_offline_tiles. NULL until the screen is built. */
static lv_obj_t * tile_water    = NULL;
static lv_obj_t * tile_family   = NULL;
static lv_obj_t * tile_vent     = NULL;
static lv_obj_t * tile_energy   = NULL;
static lv_obj_t * tile_waste    = NULL;
/* Custom-layout-only info tiles (content labels; NULL unless created). */
static lv_obj_t * tile_news_sum_lbl = NULL;
static lv_obj_t * tile_cal_dates    = NULL;
static lv_obj_t * tile_cal_times    = NULL;
static lv_obj_t * tile_cal_titles   = NULL;
static lv_obj_t * lights_handle     = NULL;
static lv_obj_t * lights_handle_lbl = NULL;
static lv_obj_t * lights_handle_img = NULL;   /* backend logo (HA / Domoticz) */

/* Swipeable tile pages. The five built-in tiles (Waste/Energy/Vent/Family/
 * Water) are "page 0". Swiping left in the tile zone reveals page 1 — an
 * overlay with extra configurable slots — and swiping right returns. We keep
 * page 0 absolutely positioned and just toggle visibility (no reparenting). */
static lv_obj_t * home_page1      = NULL;   /* page-1 overlay container */
static lv_obj_t * home_dot[2]     = {0};    /* pagination dots */
static int        home_tile_page  = 0;
/* Page-1 tiles render whatever marketplace integration is bound to slots
 * TILE_SLOT_P1_0..3 (or a "tap to assign" placeholder when empty). */
static lv_obj_t * p1_title[TILE_SLOT_P1_N] = {0};
static lv_obj_t * p1_main[TILE_SLOT_P1_N]  = {0};
static lv_obj_t * p1_sub[TILE_SLOT_P1_N]   = {0};
static int        g_p1_count = 4;   /* active page-2 slots (4 fixed, or N from a custom layout) */

/* RSS news ticker (above the forecast). Tap → headline list; tapping a headline
 * shows its article summary (from the RSS body) in the reading pane. */
static lv_obj_t * news_ticker    = NULL;
static char       news_last_first[NEWS_TITLE_MAX] = "";
static int        news_last_nn   = 0;
static lv_obj_t * news_box       = NULL;
static lv_obj_t * news_modal     = NULL;
static lv_obj_t * news_pane      = NULL;   /* scrollable right reading pane */
static lv_obj_t * news_body_lbl  = NULL;   /* reading pane article text */
static lv_obj_t * news_body_title = NULL;
static lv_obj_t * news_list      = NULL;   /* collapsible per-feed headline list */
static bool       news_collapsed[NEWS_MAX_FEEDS];  /* per-feed collapse state */
static int        news_modal_seq = -1;     /* last news_seq() rendered in news_modal */
static int        news_had_items = 0;      /* whether news_modal had items on last render */
static void news_list_rebuild(void);
static void news_show_body(int i);
/* Small banner at the top of the home screen — shown when the update
 * checker finds a newer freetoon-lvgl release. Tapping it opens a modal
 * with the release notes + the one-line install command. */
static lv_obj_t * update_banner     = NULL;
static lv_obj_t * update_banner_lbl = NULL;
static lv_obj_t * update_env_btn   = NULL; /* minimized-update envelope (top-right) */

/* Smaller tile widgets */
static lv_obj_t * lbl_humid_val;       /* removed widget — kept as NULL for old refs */
static lv_obj_t * lbl_energy_w;
static lv_obj_t * lbl_energy_gas;
static lv_obj_t * lbl_energy_today;
static lv_obj_t * lbl_nilm_event = NULL;
static lv_obj_t * lbl_boiler_state;
static lv_obj_t * lbl_boiler_pressure;
static lv_obj_t * vent_fan_img = NULL;
static int        vent_anim_period_ms = -1;

static lv_timer_t * refresh_timer = NULL;

/* Energy readings come from energy.c, which resolves settings.energy_source
 * (Toon meteradapter / HomeWizard P1 / ESPHome P1 / Home Assistant). */

/* ---------- tile builder helpers ---------- */
typedef struct {
    lv_obj_t * tile;
    lv_obj_t * title;
    lv_obj_t * value;  /* main big-ish value */
    lv_obj_t * sub;    /* sub-line under value */
} tile_t;

/* Generic tile: w x h pixels, given title, optional click handler.
 * `ltype` is the layout_tile_type_t for this tile; when custom_layout_enabled
 * is set, the position/size/visibility come from the grid layout instead of
 * the passed x/y/w/h (a tile absent from the layout is created hidden). When
 * the flag is off, x/y/w/h are used verbatim — original behaviour unchanged. */
static void make_tile(lv_obj_t * parent, int x, int y, int w, int h, int ltype,
                      const char * title, uint32_t accent_color,
                      lv_event_cb_t click_cb, tile_t * out) {
    int start_hidden = 0;
    if (settings.custom_layout_enabled && ltype != LT_NONE) {
        const layout_tile_t * L = layout_find(ltype);
        if (L) {
            layout_cell_px(L->col, L->row, L->w, L->h, &x, &y, &w, &h);
            x += 4; y += 4; w -= 8; h -= 8;     /* inter-tile margin */
            if (!L->visible) start_hidden = 1;
        } else {
            start_hidden = 1;   /* not in the user's layout → don't show */
        }
    }
    lv_obj_t * t = lv_obj_create(parent);
    lv_obj_set_size(t, w, h);
    lv_obj_set_pos(t, x, y);
    if (start_hidden) lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(t, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_radius(t, 14, 0);
    lv_obj_set_style_pad_all(t, 10, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    if (click_cb) {
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, click_cb, LV_EVENT_CLICKED, NULL);
    }

    /* Thin accent bar on top */
    lv_obj_t * bar = lv_obj_create(t);
    lv_obj_set_size(bar, w - 20, 4);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent_color), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl_title = lv_label_create(t);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_title, SF(18), 0);
    lv_label_set_text(lbl_title, title);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 0, 14);

    out->tile = t;
    out->title = lbl_title;
    out->value = NULL;
    out->sub = NULL;
}

/* ---------- navigation handlers ---------- */
static void open_thermostat(lv_event_t * e) {
    (void)e;
    ui_push(screen_thermostat_create());
}
/* +/- on the tile: child clicks don't bubble to parent, so these
   don't navigate. They just adjust setpoint in place. */
/* Wrapped via pin_gate so the +/- nudges prompt for the PIN when set.
 * The inner *_apply callbacks do the actual BoxTalk call; pin_gate runs
 * them directly when PIN is off and via the modal when it's on. */
static void sp_up_apply(void * ctx)   { (void)ctx; boxtalk_setpoint_increase(); }
static void sp_down_apply(void * ctx) { (void)ctx; boxtalk_setpoint_decrease(); }
static void on_tile_sp_up(lv_event_t * e)   { (void)e; pin_gate(sp_up_apply,   NULL); }
static void on_tile_sp_down(lv_event_t * e) { (void)e; pin_gate(sp_down_apply, NULL); }

/* ---------- program-picker modal ---------- */
typedef struct {
    int        state_value;   /* 0..3 = named scheme, -1 = Manual */
    lv_obj_t * modal;
} picker_entry_t;
static picker_entry_t picker_entries[5];

static void picker_close_cb(lv_event_t * e) {
    lv_obj_t * modal = lv_event_get_user_data(e);
    if (modal) lv_obj_del(modal);
}

/* Inner action — runs after pin_gate clears (or immediately when PIN
 * is off). The modal is closed by picker_close_cb on the same tap event,
 * so by the time pin_gate's modal opens picker_entries[].modal is the
 * picker that we want to dismiss. We dismiss it before the BoxTalk call
 * to keep the visible UI in sync. */
static void picker_apply_action(void * ctx) {
    picker_entry_t * pe = ctx;
    if (!pe) return;
    if (pe->modal) { lv_obj_del(pe->modal); pe->modal = NULL; }
    if (pe->state_value < 0) boxtalk_set_manual();
    else                     boxtalk_set_program(pe->state_value);
}
static void picker_apply_cb(lv_event_t * e) {
    picker_entry_t * pe = lv_event_get_user_data(e);
    pin_gate(picker_apply_action, pe);
}

/* Mode-toggle taps. Kept for program modal / compatibility. */
static void mode_manual_apply(void * ctx)  __attribute__((unused));
static void mode_manual_apply(void * ctx)  { (void)ctx; boxtalk_set_manual();      }
static void mode_program_apply(void * ctx) __attribute__((unused));
static void mode_program_apply(void * ctx) { (void)ctx; boxtalk_resume_schedule(); }
static void on_mode_manual(lv_event_t * e)  __attribute__((unused));
static void on_mode_manual(lv_event_t * e)  { (void)e; pin_gate(mode_manual_apply,  NULL); }
static void on_mode_program(lv_event_t * e) __attribute__((unused));
static void on_mode_program(lv_event_t * e) { (void)e; pin_gate(mode_program_apply, NULL); }

/* Direct preset tap on one of the four pill-side buttons. */
static void preset_apply(void * ctx) __attribute__((unused));
static void preset_apply(void * ctx) {
    boxtalk_set_program((int)(intptr_t)ctx);
}
static void on_tile_preset(lv_event_t * e) __attribute__((unused));
static void on_tile_preset(lv_event_t * e) {
    int s = (int)(intptr_t)lv_event_get_user_data(e);
    pin_gate(preset_apply, (void *)(intptr_t)s);
}

static void on_program_tap(lv_event_t * e) {
    (void)e;
    lv_obj_t * modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, 200, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(modal);
    lv_label_set_text(title, tr("Programma kiezen", "Select program"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, SY(50));

    const char* names[]  = {tr("Comfort", "Comfort"), tr("Thuis", "Home"),
                            tr("Slapen", "Sleep"), tr("Weg", "Away"),
                            tr("Handmatig", "Manual")};
    int         values[] = {0, 1, 2, 3, -1};
    uint32_t    colors[] = {0xff8866, 0x66cc88, 0x4466cc, 0xaa66ff, 0xffaa44};
    for (int i = 0; i < 5; i++) {
        picker_entries[i].state_value = values[i];
        picker_entries[i].modal       = modal;
        lv_obj_t * btn = lv_btn_create(modal);
        lv_obj_set_size(btn, SX(170), SY(220));
        lv_obj_set_pos(btn, SX(30 + i * 195), SY(160));
        lv_obj_set_style_bg_color(btn, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_add_event_cb(btn, picker_apply_cb, LV_EVENT_CLICKED, &picker_entries[i]);
        lv_obj_t * lbl = lv_label_create(btn);
        lv_label_set_text(lbl, names[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl, SF(28), 0);
        lv_obj_center(lbl);
    }

    lv_obj_t * cancel = lv_btn_create(modal);
    lv_obj_set_size(cancel, SX(220), SY(70));
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, SY(-40));
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(cancel, 14, 0);
    lv_obj_add_event_cb(cancel, picker_close_cb, LV_EVENT_CLICKED, modal);
    lv_obj_t * cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, tr("Annuleren", "Cancel"));
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cancel_lbl, SF(22), 0);
    lv_obj_center(cancel_lbl);
}
/* Long-press on any of the four right-column tiles. We don't yet know
 * which tile triggered this — the modal lets the user pick the slot
 * anyway — so the handler just opens the shared picker. Cheaper UI and
 * no fragile tile-→-slot mapping. */
static void on_tile_longpress(lv_event_t * e) {
    (void)e;
    screen_settings_open_tile_slots_modal();
}

static void open_family_map(lv_event_t * e);

static int open_integration_by_id(const char * b) {
    if (!b || !b[0]) return 0;
    const integration_meta_t * meta = tile_slots_integration_by_id(b);
    if (!strcmp(b, "local:agenda") || !strcmp(b, "agenda") ||
        (meta && (!strcmp(meta->detail_type, "agenda_view") || !strcmp(meta->tile_type, "agenda_list")))) {
        screen_calendar_show();
        return 1;
    }
    if (!strcmp(b, "local:sonos") || !strcmp(b, "sonos") ||
        (meta && (!strcmp(meta->detail_type, "media_player") || !strcmp(meta->tile_type, "media_player") || !strcmp(meta->detail_type, "custom_screen")))) {
        screen_sonos_show();
        return 1;
    }
    if (!strcmp(b, "local:energy")) {
        ui_push(screen_stats_create());
        return 1;
    }
    if (!strcmp(b, "local:vent") || !strcmp(b, "ventilation") || (meta && !strcmp(meta->id, "ventilation"))) {
        ui_push(screen_vent_remote_create());
        return 1;
    }
    if (!strcmp(b, "domoticz") || (meta && !strcmp(meta->id, "domoticz"))) {
        extern lv_obj_t * screen_domoticz_create(void);
        ui_push(screen_domoticz_create());
        return 1;
    }
    if (!strcmp(b, "homeassistant") || (meta && !strcmp(meta->id, "homeassistant"))) {
        extern lv_obj_t * screen_ha_devices_create(void);
        ui_push(screen_ha_devices_create());
        return 1;
    }
    if (!strcmp(b, "local:family")) {
        open_family_map(NULL);
        return 1;
    }
    if (!strcmp(b, "local:waste")) {
        return 1;
    }
    if (!strcmp(b, "local:templogger") || !strcmp(b, "temperaturelogger")) {
        ui_push(screen_chart_24h_create(tile_slots_integration_by_id("temperaturelogger")));
        return 1;
    }
    if (meta) {
        if (!strcmp(meta->detail_type, "chart_24h")) {
            ui_push(screen_chart_24h_create(meta));
            return 1;
        }
        if (!strcmp(meta->id, "crypto")) {
            ui_push(screen_crypto_create());
            return 1;
        }
    }
    return 0;
}

static int check_slot_integration_open(int slot) {
    const char * b = tile_slots_binding(slot);
    return open_integration_by_id(b);
}

static void open_placeholder(lv_event_t * e) {
    (void)e;  /* TODO: per-tile detail screens */
    if (check_slot_integration_open(3)) return;
}

static void on_update_modal_open_cb(lv_event_t * e) {
    (void)e;
    update_modal_open(scr_root);
}
static void on_update_banner_click(lv_event_t * e) {
    (void)e;
    update_modal_open(scr_root);
}

static void open_vent(lv_event_t * e) {
    (void)e;
    if (check_slot_integration_open(2)) return;
    ui_push(screen_vent_remote_create());
}

/* +/- buttons send a direct PWM bump (~15/255 ≈ 6%-step). e->user_data is
 * the int delta cast to a pointer. */
static void on_vent_bump(lv_event_t * e) {
    intptr_t d = (intptr_t)lv_event_get_user_data(e);
    vent_bump_speed((int)d);
}

/* Forward-declared here because on_vent_mode (next) writes to it but the
 * full set of vent statics below also references it from the timer handler.
 * See the 90-s window logic in refresh_cb's src_tag branch. */
static uint32_t   vent_local_press_ms = 0;

/* The remaining corner buttons send virtual-remote commands directly.
 * user_data is a literal C string (low/high/auto). The async send keeps
 * the LVGL event loop snappy — the HTTP roundtrip used to block ~1 s. */
static void on_vent_mode(lv_event_t * e) {
    const char * cmd = (const char *)lv_event_get_user_data(e);
    if (cmd) {
        vent_local_press_ms = lv_tick_get();
        vent_send_vremote_async(cmd);
    }
}

/* Low/High are back as discrete corner buttons; the switch flip-flopped
 * on each refresh when sync'd to fan_info, and "Auto" / "Timer" don't map
 * onto a 2-position toggle anyway. The buttons reuse on_vent_mode with
 * cmd="low" / "high". */

/* --- Pinned device quick-tiles (home bottom-left) ---------------------------
 * Any device flagged pin_home in the device manager appears as a compact
 * strip here: covers get Open/Stop/Close + a position slider, lights an
 * On/Off toggle + brightness slider, switches an On/Off toggle, scripts/
 * scenes a Run button. State is refreshed from ha_devices[] in refresh_cb.
 * Replaces the old fixed curtain + blinds tiles (covers are now just pinned
 * cover devices, configurable in Settings -> Devices). */
#define HOME_PIN_MAX 3
typedef struct {
    int        dev_idx;     /* index into ha_devices[] */
    lv_obj_t * tile;
    lv_obj_t * lbl;         /* "<name>  <state>" */
    lv_obj_t * slider;      /* cover position / light brightness, or NULL */
    lv_obj_t * btn;         /* light/switch toggle or script Run, or NULL */
    lv_obj_t * btn_lbl;
} home_pin_t;
static home_pin_t home_pins[HOME_PIN_MAX];
static int        home_pin_count = 0;

static int pin_ev_idx(lv_event_t * e) { return (int)(intptr_t)lv_event_get_user_data(e); }
static void on_pin_open (lv_event_t * e) { int i = pin_ev_idx(e); if (i>=0 && i<ha_device_count) ha_device_cover_async(ha_devices[i].entity_id, "open");  }
static void on_pin_stop (lv_event_t * e) { int i = pin_ev_idx(e); if (i>=0 && i<ha_device_count) ha_device_cover_async(ha_devices[i].entity_id, "stop");  }
static void on_pin_close(lv_event_t * e) { int i = pin_ev_idx(e); if (i>=0 && i<ha_device_count) ha_device_cover_async(ha_devices[i].entity_id, "close"); }
static void on_pin_toggle(lv_event_t * e) { int i = pin_ev_idx(e); if (i>=0 && i<ha_device_count) ha_device_toggle_async(ha_devices[i].type, ha_devices[i].entity_id); }
static void on_pin_run(lv_event_t * e) { int i = pin_ev_idx(e); if (i>=0 && i<ha_device_count) ha_device_run_async(ha_devices[i].type, ha_devices[i].entity_id); }
static void on_pin_pos(lv_event_t * e) {
    int i = pin_ev_idx(e);
    if (i < 0 || i >= ha_device_count) return;
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (ha_devices[i].type == HADEV_LIGHT) ha_light_set_brightness_async(ha_devices[i].entity_id, v);
    else                                   ha_cover_set_position_async(ha_devices[i].entity_id, v);
}

/* Build one pinned tile at row `slot` (0-based) for ha_devices[dev_idx]. */
static void build_pin_tile(lv_obj_t * parent, int dev_idx, int slot) {
    if (home_pin_count >= HOME_PIN_MAX) return;
    ha_device_t * D = &ha_devices[dev_idx];
    home_pin_t * P = &home_pins[home_pin_count];
    memset(P, 0, sizeof *P);
    P->dev_idx = dev_idx;

    lv_obj_t * t = lv_obj_create(parent);
    P->tile = t;
    lv_obj_set_size(t, 520, 44);
    lv_obj_set_pos(t, 20, 386 + slot * 48);
    lv_obj_set_style_bg_color(t, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_radius(t, 12, 0);
    lv_obj_set_style_pad_all(t, 6, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);

    P->lbl = lv_label_create(t);
    lv_obj_set_style_text_color(P->lbl, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(P->lbl, SF(14), 0);
    lv_label_set_text(P->lbl, D->name);
    lv_obj_align(P->lbl, LV_ALIGN_TOP_LEFT, 8, 2);

    if (D->type == HADEV_COVER || D->type == HADEV_LIGHT) {
        P->slider = lv_slider_create(t);
        lv_obj_set_size(P->slider, 240, 6);
        lv_obj_align(P->slider, LV_ALIGN_BOTTOM_LEFT, 8, -4);
        lv_slider_set_range(P->slider, 0, 100);
        lv_slider_set_value(P->slider, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(P->slider, lv_color_hex(0x223344), LV_PART_MAIN);
        lv_obj_set_style_bg_color(P->slider,
            lv_color_hex(D->type == HADEV_LIGHT ? 0xffcc44 : 0x44aaff), LV_PART_INDICATOR);
        lv_obj_remove_style(P->slider, NULL, LV_PART_KNOB);
        lv_obj_add_event_cb(P->slider, on_pin_pos, LV_EVENT_RELEASED, (void *)(intptr_t)dev_idx);
    }

    if (D->type == HADEV_COVER) {
        struct { const char * txt; uint32_t col; lv_event_cb_t cb; int x; } cb[] = {
            { tr("Open", "Open"),    0x2e6e3a, on_pin_open, -156 },
            { tr("Stop", "Stop"),    0x6a5424, on_pin_stop,  -82 },
            { tr("Dicht", "Close"),  0x6e3a3a, on_pin_close,  -8 },
        };
        for (size_t i = 0; i < 3; i++) {
            lv_obj_t * b = lv_btn_create(t);
            lv_obj_set_size(b, 70, 32);
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, cb[i].x, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(cb[i].col), 0);
            lv_obj_set_style_radius(b, 8, 0);
            lv_obj_add_event_cb(b, cb[i].cb, LV_EVENT_CLICKED, (void *)(intptr_t)dev_idx);
            lv_obj_t * l = lv_label_create(b);
            lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_text_font(l, SF(14), 0);
            lv_label_set_text(l, cb[i].txt);
            lv_obj_center(l);
        }
    } else {
        int run = (D->type == HADEV_SCRIPT || D->type == HADEV_SCENE);
        P->btn = lv_btn_create(t);
        lv_obj_set_size(P->btn, run ? 90 : 70, 32);
        lv_obj_align(P->btn, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_style_bg_color(P->btn, lv_color_hex(run ? 0x2e5e8a : 0x3a4658), 0);
        lv_obj_set_style_radius(P->btn, 8, 0);
        lv_obj_add_event_cb(P->btn, run ? on_pin_run : on_pin_toggle, LV_EVENT_CLICKED,
                            (void *)(intptr_t)dev_idx);
        P->btn_lbl = lv_label_create(P->btn);
        lv_obj_set_style_text_color(P->btn_lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(P->btn_lbl, SF(14), 0);
        lv_label_set_text(P->btn_lbl, run ? (D->type == HADEV_SCENE ? tr("Activeer", "Activate") : tr("Start", "Run")) : tr("Uit", "Off"));
        lv_obj_center(P->btn_lbl);
    }
    home_pin_count++;
}

/* Timer button cycles 10 → 20 → 30 → off and updates its own label.
 * "off" maps to vremotecmd=auto (the most natural resting state). */
static lv_obj_t * vent_timer_lbl = NULL;
/* Vent preset buttons — refs kept so refresh_cb can highlight the one that
 * matches vent_state.fan_info (white outline = currently active preset). */
static lv_obj_t * vent_btn_low   = NULL;
static lv_obj_t * vent_btn_high  = NULL;
static lv_obj_t * vent_btn_auto  = NULL;
static lv_obj_t * vent_btn_timer = NULL;
static int        vent_timer_step = 0;
static void on_vent_timer(lv_event_t * e) {
    (void)e;
    vent_timer_step = (vent_timer_step + 1) & 3;
    const char * cmd; const char * label;
    switch (vent_timer_step) {
        case 1:  cmd = "timer1"; label = "10m"; break;
        case 2:  cmd = "timer2"; label = "20m"; break;
        case 3:  cmd = "timer3"; label = "30m"; break;
        default: cmd = "auto";   label = tr("Timer", "Timer"); vent_timer_step = 0; break;
    }
    if (vent_timer_lbl) lv_label_set_text(vent_timer_lbl, label);
    vent_local_press_ms = lv_tick_get();
    vent_send_vremote_async(cmd);
}

/* Apply rotation animation directly to the fan image. TRUE_COLOR_ALPHA
 * format rotates fine via lv_img_set_angle. */
static void vent_fan_anim_cb(void * obj, int32_t v) {
    lv_img_set_angle((lv_obj_t *)obj, v);
}
static void vent_apply_fan_anim(int rpm) {
    if (!vent_fan_img) return;
    /* Drive the spin off rpm — the only signal that's actually physical on
       this unit. Itho's ExhFanSpeed (%) is broken (Error:16, stuck at 0 in
       High) AND its "Low" button can produce higher rpm than "High", so
       any percentage-derived rate gives the wrong visual.

       Park the icon below 50 rpm (fan off / coasting down). */
    if (rpm < 50) {
        if (vent_anim_period_ms == 0) return;
        vent_anim_period_ms = 0;
        lv_anim_del(vent_fan_img, NULL);
        return;
    }
    /* Linear rpm → ms/turn mapped onto this CVE's REAL rpm band so the spin
       visibly tracks Low→High. After the 2026-06-15 remote fix the band is
       ~1150 rpm (low/idle) → ~2700 rpm (high) → ~2900 (boost). The old
       3500-rpm curve only reached ~800 ms at high (not obviously fast); this
       steeper curve (~1.3 ms/rpm) gives:
         ~1150 rpm → ~2.4 s/turn — slow
         ~1800 rpm → ~1.6 s/turn — medium
         ~2700 rpm → ~0.4 s/turn — fast blur */
    int period = 2600 - (rpm - 1000) * 13 / 10;
    if (period < 280)  period = 280;
    if (period > 2600) period = 2600;
    /* Hysteresis: rpm jitters ±1 every 8 s poll. Without this every poll
       would lv_anim_del + restart the spin, snapping the icon back to 0°
       (visible glitch — that's what "spinner acting weird" actually was). */
    if (abs(period - vent_anim_period_ms) < 100) return;
    vent_anim_period_ms = period;
    lv_anim_del(vent_fan_img, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, vent_fan_img);
    lv_anim_set_exec_cb(&a, vent_fan_anim_cb);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_time(&a, period);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

/* Shrink a label's font (never grow) so `txt` fits within max_w px WITHOUT
 * truncation — proper scaling for narrow custom tiles. Tries the enabled
 * Montserrat ladder downward starting at the label's design size `base_px`;
 * picks the largest that fits, falling back to the smallest. */
static void fit_font(lv_obj_t * lbl, const char * txt, lv_coord_t max_w, int base_px) {
    static const struct { int px; const lv_font_t * f; } L[] = {
        { 28, &lv_font_montserrat_28 }, { 22, &lv_font_montserrat_22 },
        { 20, &lv_font_montserrat_20 }, { 18, &lv_font_montserrat_18 },
        { 14, &lv_font_montserrat_14 }, { 12, &lv_font_montserrat_12 },
    };
    if (!lbl || !txt || max_w < 1) return;
    for (unsigned i = 0; i < sizeof L / sizeof L[0]; i++) {
        if (L[i].px > base_px) continue;            /* never larger than the design size */
        lv_point_t sz;
        lv_txt_get_size(&sz, txt, L[i].f, 0, 0, LV_COORD_MAX, 0);
        if (sz.x <= max_w) { lv_obj_set_style_text_font(lbl, L[i].f, 0); return; }
    }
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
}
static void open_settings_action(void * ctx) {
    (void)ctx;
    ui_push(screen_settings_create());
}
static void open_settings(lv_event_t * e) {
    (void)e;
    pin_gate(open_settings_action, NULL);
}
static void open_stats(lv_event_t * e) {
    (void)e;
    if (check_slot_integration_open(0)) return;
    ui_push(screen_stats_create());
}
static void open_forecast(lv_event_t * e) {
    (void)e;
    fprintf(stderr, "[ui] forecast tap\n");
    ui_push(screen_forecast_create());
}
static void open_inbox(lv_event_t * e) {
    (void)e;
    screen_inbox_show();
}

/* ---------- refresh timer ---------- */
/* Per-tile offline rendering. Computes whether the tile's integration is
 * "live" right now (toggle on + remote reachable), and then either:
 *   - hides the entire tile (settings.hide_offline_tiles=1), or
 *   - lets the per-tile refresh below paint an "offline" placeholder
 *     (default — keeps the layout stable, matches Energy's existing
 *     "P1 offline" treatment).
 * Called once per refresh tick from refresh_cb. */
static void apply_offline_tile_visibility(void) {
    int hide = settings.hide_offline_tiles;
    int energy_live = energy_connected();
    int water_live  = (settings.enable_p1_water || settings.client_mode) && hw_state.connected_water;
    int vent_live   = (settings.enable_vent     || settings.client_mode) && vent_state.connected;
    int family_live = settings.enable_ha || (settings.client_mode && (ha_state.connected || ha_state.loc_a[0]));
    struct { lv_obj_t * tile; int live; int ltype; } v[] = {
        { tile_energy,   energy_live, LT_ENERGY },
        { tile_water,    water_live,  LT_WATER  },
        { tile_vent,     vent_live,   LT_VENT   },
        { tile_family,   family_live, LT_FAMILY },
        /* Pinned device tiles show their own "(HA offline)" text in refresh_cb. */
    };
    for (size_t i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        if (!v[i].tile) continue;
        /* With a custom layout, the layout decides visibility: a tile the user
         * removed/hid (or that isn't in the layout, e.g. curtains) stays hidden
         * — don't let the offline-gate re-show it every tick. */
        if (settings.custom_layout_enabled) {
            const layout_tile_t * L =
                (v[i].ltype != LT_NONE) ? layout_find(v[i].ltype) : NULL;
            if (!L || !L->visible) { lv_obj_add_flag(v[i].tile, LV_OBJ_FLAG_HIDDEN); continue; }
            lv_obj_clear_flag(v[i].tile, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        if (hide && !v[i].live) lv_obj_add_flag(v[i].tile, LV_OBJ_FLAG_HIDDEN);
        else                    lv_obj_clear_flag(v[i].tile, LV_OBJ_FLAG_HIDDEN);
    }
}

static int render_tile_slot(int slot, lv_obj_t * lbl_main, lv_obj_t * lbl_sub);  /* fwd */

/* Agenda overlay for the 4 swappable right-column tiles (ENERGY/FAMILY/VENT/
 * WATER). Shows a 2-column view (dates/times on the left, titles on the right)
 * when that slot is bound to "local:agenda" — shown/hidden in refresh_cb. */
typedef struct {
    lv_obj_t * root;
    lv_obj_t * lbl_hdr;
    lv_obj_t * lbl_dates;
    lv_obj_t * lbl_times;
    lv_obj_t * lbl_titles;
} slot_agenda_view_t;

static slot_agenda_view_t * slot_agenda[4] = {0};

/* Sonos UI view structure — used both for slot overlay on right-column tiles
 * and for the standalone custom layout tile (LT_SONOS). */
typedef struct {
    lv_obj_t * root;
    lv_obj_t * art_box;
    lv_obj_t * img_art;
    int        last_art_ver;
    lv_obj_t * lbl_zone;
    lv_obj_t * lbl_vol;
    lv_obj_t * lbl_artist;
    lv_obj_t * lbl_title;
    lv_obj_t * lbl_status;
    lv_obj_t * btn_voldn;
    lv_obj_t * btn_prev;
    lv_obj_t * btn_play;
    lv_obj_t * lbl_play;
    lv_obj_t * btn_next;
    lv_obj_t * btn_volup;
} sonos_view_t;

static sonos_view_t * slot_sonos[4] = {0};
static lv_obj_t *      slot_waste[4] = {0};
static sonos_view_t * tile_sonos_view = NULL;

typedef struct {
    lv_obj_t * root;
    lv_obj_t * lbl_title;
    lv_obj_t * lbl_val;
    lv_obj_t * chart;
    lv_chart_series_t * ser_room;
    lv_chart_series_t * ser_sp;
    lv_chart_series_t * ser_out;
} slot_templogger_view_t;

static slot_templogger_view_t * slot_templogger[4] = {0};
static lv_obj_t * lbl_title_slot[TILE_SLOT_COUNT] = {0};

static void on_sonos_tile_tap(lv_event_t * e) {
    (void)e;
    if (!settings.sonos_enabled) {
        screen_settings_open_sonos_modal();
    } else {
        screen_sonos_show();
    }
}

static void on_sonos_voldn(lv_event_t * e) {
    lv_event_stop_bubbling(e);
    sonos_volume_down();
}

static void on_sonos_prev(lv_event_t * e) {
    lv_event_stop_bubbling(e);
    sonos_prev();
}

static void on_sonos_playpause(lv_event_t * e) {
    lv_event_stop_bubbling(e);
    sonos_play_pause();
}

static void on_sonos_next(lv_event_t * e) {
    lv_event_stop_bubbling(e);
    sonos_next();
}

static void on_sonos_volup(lv_event_t * e) {
    lv_event_stop_bubbling(e);
    sonos_volume_up();
}

static sonos_view_t * make_sonos_view(lv_obj_t * parent) {
    if (!parent) return NULL;
    sonos_view_t * v = calloc(1, sizeof(sonos_view_t));
    if (!v) return NULL;

    v->last_art_ver = -1;
    v->root = lv_obj_create(parent);
    lv_obj_set_size(v->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(v->root, 0, 0);
    lv_obj_set_style_bg_color(v->root, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_bg_opa(v->root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(v->root, 10, 0);
    lv_obj_set_style_pad_all(v->root, SX(6), 0);
    lv_obj_set_style_border_width(v->root, 0, 0);
    lv_obj_clear_flag(v->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(v->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v->root, on_sonos_tile_tap, LV_EVENT_CLICKED, NULL);

    lv_obj_t * abar = lv_obj_create(v->root);
    lv_obj_set_size(abar, LV_PCT(100), SY(3));
    lv_obj_align(abar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(abar, lv_color_hex(0x2077a0), 0);
    lv_obj_set_style_border_width(abar, 0, 0);
    lv_obj_set_style_radius(abar, 2, 0);
    lv_obj_clear_flag(abar, LV_OBJ_FLAG_SCROLLABLE);

    v->lbl_zone = lv_label_create(v->root);
    lv_obj_set_style_text_color(v->lbl_zone, lv_color_hex(0x44aaff), 0);
    lv_obj_set_style_text_font(v->lbl_zone, SF(18), 0);
    lv_obj_align(v->lbl_zone, LV_ALIGN_TOP_LEFT, SX(2), SY(4));
    lv_label_set_text(v->lbl_zone, "Sonos");

    v->lbl_vol = lv_label_create(v->root);
    lv_obj_set_style_text_color(v->lbl_vol, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(v->lbl_vol, SF(15), 0);
    lv_obj_align(v->lbl_vol, LV_ALIGN_TOP_RIGHT, SX(-2), SY(5));
    lv_label_set_text(v->lbl_vol, "");

    /* Album art thumbnail (42x42, rounded corners) */
    lv_coord_t y_art = (DISP_VER < 600) ? 28 : SY(32);
    lv_coord_t y_artist = (DISP_VER < 600) ? 28 : SY(32);
    lv_coord_t y_title = (DISP_VER < 600) ? 46 : SY(52);
    lv_coord_t y_status = (DISP_VER < 600) ? 64 : SY(72);

    v->art_box = lv_obj_create(v->root);
    lv_obj_set_size(v->art_box, SUNI(42), SUNI(42));
    lv_obj_set_pos(v->art_box, SX(2), y_art);
    lv_obj_set_style_radius(v->art_box, 6, 0);
    lv_obj_set_style_clip_corner(v->art_box, true, 0);
    lv_obj_set_style_bg_color(v->art_box, lv_color_hex(0x132135), 0);
    lv_obj_set_style_border_width(v->art_box, 0, 0);
    lv_obj_set_style_pad_all(v->art_box, 0, 0);
    lv_obj_clear_flag(v->art_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(v->art_box, LV_OBJ_FLAG_HIDDEN);

    v->img_art = lv_img_create(v->art_box);
    lv_img_set_pivot(v->img_art, 0, 0);
    lv_obj_set_pos(v->img_art, 0, 0);

    v->lbl_artist = lv_label_create(v->root);
    lv_obj_set_style_text_color(v->lbl_artist, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(v->lbl_artist, SF(16), 0);
    lv_label_set_long_mode(v->lbl_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v->lbl_artist, LV_PCT(96));
    lv_obj_align(v->lbl_artist, LV_ALIGN_TOP_LEFT, SX(2), y_artist);
    lv_label_set_text(v->lbl_artist, "");

    v->lbl_title = lv_label_create(v->root);
    lv_obj_set_style_text_color(v->lbl_title, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(v->lbl_title, SF(14), 0);
    lv_label_set_long_mode(v->lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v->lbl_title, LV_PCT(96));
    lv_obj_align(v->lbl_title, LV_ALIGN_TOP_LEFT, SX(2), y_title);
    lv_label_set_text(v->lbl_title, "");

    v->lbl_status = lv_label_create(v->root);
    lv_obj_set_style_text_color(v->lbl_status, lv_color_hex(0xffcc44), 0);
    lv_obj_set_style_text_font(v->lbl_status, SF(12), 0);
    lv_obj_align(v->lbl_status, LV_ALIGN_TOP_LEFT, SX(2), y_status);
    lv_label_set_text(v->lbl_status, "");

    lv_obj_t * bar = lv_obj_create(v->root);
    lv_obj_set_size(bar, LV_PCT(96), (DISP_VER < 600) ? 34 : SY(38));
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, SY(-2));
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, SX(4), 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int btn_w = (DISP_HOR < 1000) ? 52 : SX(58);
    int btn_h = (DISP_VER < 600) ? 32 : SY(34);

    #define MAKE_TBTN(btn_ptr, lbl_ptr, sym, w, fsz, cb) do { \
        btn_ptr = lv_btn_create(bar); \
        lv_obj_set_size(btn_ptr, w, btn_h); \
        lv_obj_set_style_bg_color(btn_ptr, lv_color_hex(settings.home_light_mode ? 0xccdaea : 0x223a56), 0); \
        lv_obj_set_style_radius(btn_ptr, 8, 0); \
        lv_obj_set_style_pad_all(btn_ptr, 0, 0); \
        lv_obj_set_ext_click_area(btn_ptr, 10); \
        lv_obj_add_event_cb(btn_ptr, cb, LV_EVENT_CLICKED, NULL); \
        lbl_ptr = lv_label_create(btn_ptr); \
        lv_obj_set_style_text_color(lbl_ptr, lv_color_hex(settings.home_light_mode ? 0x1e2838 : 0xffffff), 0); \
        lv_obj_set_style_text_font(lbl_ptr, SF(fsz), 0); \
        lv_label_set_text(lbl_ptr, sym); \
        lv_obj_center(lbl_ptr); \
    } while (0)

    lv_obj_t * dummy;
    MAKE_TBTN(v->btn_prev, dummy,       LV_SYMBOL_PREV, btn_w, 18, on_sonos_prev);
    MAKE_TBTN(v->btn_play, v->lbl_play, LV_SYMBOL_PLAY, btn_w + SX(8), 20, on_sonos_playpause);
    MAKE_TBTN(v->btn_next, dummy,       LV_SYMBOL_NEXT, btn_w, 18, on_sonos_next);
    #undef MAKE_TBTN

    return v;
}

static void update_sonos_view(sonos_view_t * v) {
    if (!v) return;
    sonos_state_t st;
    sonos_get_snapshot(&st);

    lv_coord_t y_artist = (DISP_VER < 600) ? 28 : SY(32);
    lv_coord_t y_title = (DISP_VER < 600) ? 46 : SY(52);
    lv_coord_t y_status = (DISP_VER < 600) ? 64 : SY(72);

    if (!settings.sonos_enabled) {
        if (v->art_box) lv_obj_add_flag(v->art_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(v->lbl_artist, SX(2), y_artist);
        lv_obj_set_width(v->lbl_artist, LV_PCT(96));
        lv_obj_set_pos(v->lbl_title, SX(2), y_title);
        lv_obj_set_width(v->lbl_title, LV_PCT(96));
        lv_obj_set_pos(v->lbl_status, SX(2), y_status);
        lv_obj_set_width(v->lbl_status, LV_PCT(96));
        lv_label_set_text(v->lbl_zone, "Sonos (uit)");
        lv_label_set_text(v->lbl_artist, tr("Niet ingeschakeld", "Disabled"));
        lv_label_set_text(v->lbl_title, tr("Tik voor instellingen", "Tap for settings"));
        lv_label_set_text(v->lbl_status, "");
        lv_label_set_text(v->lbl_vol, "");
        lv_label_set_text(v->lbl_play, LV_SYMBOL_PLAY);
        return;
    }

    if (!st.connected) {
        if (v->art_box) lv_obj_add_flag(v->art_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(v->lbl_artist, SX(2), y_artist);
        lv_obj_set_width(v->lbl_artist, LV_PCT(96));
        lv_obj_set_pos(v->lbl_title, SX(2), y_title);
        lv_obj_set_width(v->lbl_title, LV_PCT(96));
        lv_obj_set_pos(v->lbl_status, SX(2), y_status);
        lv_obj_set_width(v->lbl_status, LV_PCT(96));
        lv_label_set_text_fmt(v->lbl_zone, "%s (offline)", st.zone[0] ? st.zone : "Sonos");
        lv_label_set_text(v->lbl_artist, tr("Geen verbinding", "No connection"));
        lv_label_set_text(v->lbl_title, tr("Controleer host/poort", "Check host/port"));
        lv_label_set_text(v->lbl_status, "");
        lv_label_set_text(v->lbl_vol, "");
        lv_label_set_text(v->lbl_play, LV_SYMBOL_PLAY);
        return;
    }

    /* Artwork update */
    int art_ver = 0;
    const lv_img_dsc_t * art_dsc = sonos_get_art_tile(&art_ver);
    if (art_ver != v->last_art_ver) {
        v->last_art_ver = art_ver;
        if (art_dsc && v->img_art && v->art_box) {
            lv_img_set_zoom(v->img_art, LV_IMG_ZOOM_NONE);
            lv_img_set_src(v->img_art, art_dsc);
            lv_obj_invalidate(v->img_art);
            lv_obj_clear_flag(v->art_box, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_pos(v->lbl_artist, SX(50), y_artist);
            lv_obj_set_width(v->lbl_artist, SX(136));
            lv_obj_set_pos(v->lbl_title, SX(50), y_title);
            lv_obj_set_width(v->lbl_title, SX(136));
            lv_obj_set_pos(v->lbl_status, SX(50), y_status);
            lv_obj_set_width(v->lbl_status, SX(136));
        } else {
            if (v->art_box) lv_obj_add_flag(v->art_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(v->lbl_artist, SX(2), y_artist);
            lv_obj_set_width(v->lbl_artist, LV_PCT(96));
            lv_obj_set_pos(v->lbl_title, SX(2), y_title);
            lv_obj_set_width(v->lbl_title, LV_PCT(96));
            lv_obj_set_pos(v->lbl_status, SX(2), y_status);
            lv_obj_set_width(v->lbl_status, LV_PCT(96));
        }
    }

    if (st.is_group) {
        lv_label_set_text_fmt(v->lbl_zone, "Grp %s", st.zone);
    } else {
        lv_label_set_text(v->lbl_zone, st.zone[0] ? st.zone : "Sonos");
    }

    lv_label_set_text_fmt(v->lbl_vol, "%d%%", st.volume);

    if (st.artist[0]) {
        lv_label_set_text(v->lbl_artist, st.artist);
    } else {
        lv_label_set_text(v->lbl_artist, st.playing ? tr("Muziek", "Music") : tr("Geen muziek", "No music"));
    }
    lv_label_set_text(v->lbl_title, st.title[0] ? st.title : "");

    if (!st.playing) {
        lv_label_set_text(v->lbl_status, tr("(gepauzeerd)", "(paused)"));
        lv_obj_set_style_text_color(v->lbl_status, lv_color_hex(settings.home_light_mode ? 0xd97706 : 0xffcc44), 0);
    } else if (st.duration > 0 && st.elapsed >= 0) {
        int em = st.elapsed / 60, es = st.elapsed % 60;
        int dm = st.duration / 60, ds = st.duration % 60;
        char tbuf[32];
        snprintf(tbuf, sizeof tbuf, "%02d:%02d / %02d:%02d", em, es, dm, ds);
        lv_label_set_text(v->lbl_status, tbuf);
        lv_obj_set_style_text_color(v->lbl_status, lv_color_hex(COL_TEXT_DIM), 0);
    } else {
        lv_label_set_text(v->lbl_status, "");
    }

    lv_label_set_text(v->lbl_play, st.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void on_slot_agenda_tap(lv_event_t * e) {
    (void)e;
    screen_calendar_show();
}

static slot_agenda_view_t * make_slot_agenda(lv_obj_t * tile, uint32_t bg) {
    if (!tile) return NULL;
    slot_agenda_view_t * a = calloc(1, sizeof(*a));
    if (!a) return NULL;

    a->root = lv_obj_create(tile);
    lv_obj_set_size(a->root, LV_PCT(100), LV_PCT(100));
    lv_obj_align(a->root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(a->root, lv_color_hex(settings.home_light_mode ? COL_TILE_BG : bg), 0);
    lv_obj_set_style_bg_opa(a->root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(a->root, 10, 0);
    lv_obj_set_style_pad_all(a->root, SX(8), 0);
    lv_obj_set_style_border_width(a->root, 0, 0);
    lv_obj_clear_flag(a->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a->root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->root, on_slot_agenda_tap, LV_EVENT_CLICKED, NULL);

    a->lbl_hdr = lv_label_create(a->root);
    lv_obj_set_style_text_color(a->lbl_hdr, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(a->lbl_hdr, SF(16), 0);
    lv_label_set_text(a->lbl_hdr, tr("Agenda", "Calendar"));
    lv_obj_align(a->lbl_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Column 1: Date (e.g. "wo", "15-10") */
    a->lbl_dates = lv_label_create(a->root);
    lv_obj_set_style_text_color(a->lbl_dates, lv_color_hex(settings.home_light_mode ? 0x1d4ed8 : 0xaad4f5), 0);
    lv_obj_set_style_text_font(a->lbl_dates, SF(14), 0);
    lv_label_set_long_mode(a->lbl_dates, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(a->lbl_dates, SX(38));
    lv_obj_align(a->lbl_dates, LV_ALIGN_TOP_LEFT, 0, SY(24));

    /* Column 2: Start time (e.g. "13:00") */
    a->lbl_times = lv_label_create(a->root);
    lv_obj_set_style_text_color(a->lbl_times, lv_color_hex(settings.home_light_mode ? 0x2563eb : 0x88ccff), 0);
    lv_obj_set_style_text_font(a->lbl_times, SF(14), 0);
    lv_label_set_long_mode(a->lbl_times, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(a->lbl_times, SX(38));
    lv_obj_align(a->lbl_times, LV_ALIGN_TOP_LEFT, SX(42), SY(24));

    /* Column 3: Title (width set dynamically at refresh) */
    a->lbl_titles = lv_label_create(a->root);
    lv_obj_set_style_text_color(a->lbl_titles, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(a->lbl_titles, SF(14), 0);
    lv_label_set_long_mode(a->lbl_titles, LV_LABEL_LONG_CLIP);
    lv_obj_align(a->lbl_titles, LV_ALIGN_TOP_LEFT, SX(86), SY(24));

    lv_obj_move_foreground(a->root);
    return a;
}

/* Format calendar date: 2-letter weekday for first 7 days, dd-mm thereafter. */
static void format_agenda_date(const char * ymd, time_t now, char * out, size_t outsz) {
    if (!ymd || strlen(ymd) < 10) {
        snprintf(out, outsz, "     ");
        return;
    }
    int y = 0, m = 0, d = 0;
    if (sscanf(ymd, "%4d-%2d-%2d", &y, &m, &d) == 3) {
        struct tm ev_tm = {0};
        ev_tm.tm_year = y - 1900;
        ev_tm.tm_mon  = m - 1;
        ev_tm.tm_mday = d;
        ev_tm.tm_hour = 12;
        time_t ev_time = mktime(&ev_tm);

        struct tm now_tm = *localtime(&now);
        now_tm.tm_hour = 12;
        now_tm.tm_min = 0;
        now_tm.tm_sec = 0;
        time_t today_noon = mktime(&now_tm);

        double diff_days = (double)(ev_time - today_noon) / 86400.0;
        if (diff_days >= -0.5 && diff_days < 6.5) {
            /* Within the first week (next 7 days): 2-letter weekday */
            static const char * const d_nl[] = {"zo", "ma", "di", "wo", "do", "vr", "za"};
            static const char * const d_en[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
            int wday = ev_tm.tm_wday;
            if (wday < 0 || wday > 6) wday = 0;
            const char * dw = (settings.language == LANG_EN) ? d_en[wday] : d_nl[wday];
            snprintf(out, outsz, "%s", dw);
            return;
        } else {
            /* Later appointments: dd-mm */
            snprintf(out, outsz, "%02d-%02d", d, m);
            return;
        }
    }
    snprintf(out, outsz, "%.5s", ymd + 5);
}

/* Fill an agenda view (dates column + times column + titles column) row for row. */
static void fill_agenda_view(lv_obj_t * parent, lv_obj_t * lbl_dates, lv_obj_t * lbl_times, lv_obj_t * lbl_titles, lv_coord_t avail_h) {
    if (!lbl_dates || !lbl_times || !lbl_titles) return;

    lv_coord_t cw = 0;
    if (parent) {
        cw = lv_obj_get_content_width(parent);
        if (cw <= 0) cw = lv_obj_get_width(parent);
    }
    if (cw <= 0) cw = SX(198);

    lv_coord_t col1_x = 0;
    lv_coord_t col1_w = SX(38);
    lv_coord_t col2_x = SX(42);
    lv_coord_t col2_w = SX(38);
    lv_coord_t col3_x = SX(86);
    lv_coord_t col3_w = cw - col3_x - SX(2);
    if (col3_w < SX(60)) col3_w = SX(60);

    if (!settings.calendar_enabled) {
        lv_label_set_text(lbl_dates, "");
        lv_label_set_text(lbl_times, "");
        lv_obj_set_pos(lbl_titles, 0, lv_obj_get_y(lbl_dates));
        lv_obj_set_width(lbl_titles, cw);
        lv_label_set_text(lbl_titles, tr("Agenda uit", "Calendar off"));
        return;
    }

    if (calendar_state.count <= 0) {
        lv_label_set_text(lbl_dates, "");
        lv_label_set_text(lbl_times, "");
        lv_obj_set_pos(lbl_titles, 0, lv_obj_get_y(lbl_dates));
        lv_obj_set_width(lbl_titles, cw);
        lv_label_set_text(lbl_titles, tr("Geen afspraken", "No events"));
        return;
    }

    int line_h = SY(20);
    if (line_h < 14) line_h = 14;
    int max_lines = avail_h / line_h;
    if (max_lines < 2) max_lines = 2;
    if (max_lines > 20) max_lines = 20;

    int n = calendar_state.count;
    if (n > max_lines) n = max_lines;

    char buf_dates[512] = "";
    char buf_times[512] = "";
    char buf_titles[2048] = "";
    time_t now = time(NULL);

    for (int i = 0; i < n; i++) {
        calendar_event_t * ev = &calendar_state.ev[i];

        /* Column 1: Date */
        char dt[16];
        format_agenda_date(ev->date, now, dt, sizeof dt);
        char row_date[32];
        snprintf(row_date, sizeof row_date, "%s\n", dt);
        strncat(buf_dates, row_date, sizeof buf_dates - strlen(buf_dates) - 1);

        /* Column 2: Start time (spaces if all-day event) */
        char row_time[32];
        if (ev->time[0]) {
            snprintf(row_time, sizeof row_time, "%s\n", ev->time);
        } else {
            snprintf(row_time, sizeof row_time, "     \n");
        }
        strncat(buf_times, row_time, sizeof buf_times - strlen(buf_times) - 1);

        /* Column 3: Title (sanitized single line) */
        char clean_title[64];
        size_t c = 0;
        for (const char * p = ev->summary; *p && c < sizeof(clean_title) - 1; p++) {
            if (*p == '\r' || *p == '\n') {
                clean_title[c++] = ' ';
            } else {
                clean_title[c++] = *p;
            }
        }
        clean_title[c] = '\0';
        char row_title[80];
        snprintf(row_title, sizeof row_title, "%s\n", clean_title);
        strncat(buf_titles, row_title, sizeof buf_titles - strlen(buf_titles) - 1);
    }

    lv_obj_set_pos(lbl_dates, col1_x, lv_obj_get_y(lbl_dates));
    lv_obj_set_width(lbl_dates, col1_w);

    lv_obj_set_pos(lbl_times, col2_x, lv_obj_get_y(lbl_dates));
    lv_obj_set_width(lbl_times, col2_w);

    lv_obj_set_pos(lbl_titles, col3_x, lv_obj_get_y(lbl_dates));
    lv_obj_set_width(lbl_titles, col3_w);

    lv_label_set_text(lbl_dates, buf_dates);
    lv_label_set_text(lbl_times, buf_times);
    lv_label_set_text(lbl_titles, buf_titles);
}

/* Fill an agenda slot overlay with upcoming events. */
static void fill_agenda_overlay(slot_agenda_view_t * a) {
    if (!a || !a->root) return;
    lv_coord_t th = lv_obj_get_height(lv_obj_get_parent(a->root));
    if (th <= 0) th = lv_obj_get_height(a->root);
    if (th <= 0) th = SY(130);
    lv_coord_t avail_h = th - SY(34);
    fill_agenda_view(a->root, a->lbl_dates, a->lbl_times, a->lbl_titles, avail_h);
}

static void on_slot_waste_tap(lv_event_t * e) {
    (void)e;
}

static lv_obj_t * make_slot_waste(lv_obj_t * tile, uint32_t bg) {
    if (!tile) return NULL;
    lv_obj_t * l = lv_label_create(tile);
    lv_obj_set_style_bg_color(l, lv_color_hex(settings.home_light_mode ? COL_TILE_BG : bg), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 10, 0);
    lv_obj_set_style_pad_all(l, SX(6), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(l, SF(14), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(l, LV_PCT(100), LV_PCT(100));
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(l, on_slot_waste_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(l);                 /* cover the tile's own widgets */
    return l;
}

static void fill_waste_overlay(lv_obj_t * l) {
    if (!l) return;
    lv_coord_t th = lv_obj_get_height(lv_obj_get_parent(l));
    if (th <= 0) th = lv_obj_get_height(l);
    if (th <= 0) th = SY(120);

    /* Determine how many pickup lines fit based on available tile height.
     * Header "Afval" + padding takes ~28px; each pickup line takes ~17px. */
    int max_lines = (th - SY(28)) / SY(17);
    if (max_lines < 1) max_lines = 1;
    if (max_lines > 4) max_lines = 4;

    waste_pickup_t pickups[4] = {0};
    int n = waste_state.connected ? waste_next_n_windowed(365, pickups, 4) : 0;
    if (n == 0 && waste_state.connected) {
        waste_pickup_t p1 = {0}, p2 = {0};
        int n2 = waste_next_2_pickups(&p1, &p2);
        if (n2 >= 1) { pickups[0] = p1; n = 1; }
        if (n2 >= 2) { pickups[1] = p2; n = 2; }
    }

    char out[512] = "";
    if (n >= 1) {
        snprintf(out, sizeof out, "%s", tr("Afval", "Waste"));
        int lines = (n < max_lines) ? n : max_lines;
        for (int i = 0; i < lines; i++) {
            int mo = atoi(pickups[i].date + 5);
            int d  = atoi(pickups[i].date + 8);
            char line[96];
            snprintf(line, sizeof line, "\n%d-%d  %s", d, mo, pickups[i].labels);
            strncat(out, line, sizeof out - strlen(out) - 1);
        }
    } else {
        snprintf(out, sizeof out, "%s\n%s",
                 tr("Afval", "Waste"),
                 waste_state.connected ? tr("Geen ophaaldata", "No pickups") : "...");
    }
    lv_label_set_text(l, out);
}

static void on_slot_templogger_tap(lv_event_t * e) {
    (void)e;
    ui_push(screen_chart_24h_create(tile_slots_integration_by_id("temperaturelogger")));
}

static slot_templogger_view_t * make_slot_templogger(lv_obj_t * tile) {
    if (!tile) return NULL;
    slot_templogger_view_t * v = calloc(1, sizeof(*v));
    if (!v) return NULL;

    lv_obj_t * root = lv_obj_create(tile);
    v->root = root;
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(root, 10, 0);
    lv_obj_set_style_pad_all(root, SX(6), 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, on_slot_templogger_tap, LV_EVENT_CLICKED, NULL);

    /* Blue accent bar at top */
    lv_obj_t * bar = lv_obj_create(root);
    lv_obj_set_size(bar, LV_PCT(100), 3);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x3388ff), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Title label */
    lv_obj_t * lt = lv_label_create(root);
    v->lbl_title = lt;
    lv_obj_set_style_text_color(lt, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lt, SF(16), 0);
    lv_label_set_text(lt, tr("Temperatuur", "Temperature"));
    lv_obj_align(lt, LV_ALIGN_TOP_LEFT, SX(2), SY(8));

    /* Value label */
    lv_obj_t * lv = lv_label_create(root);
    v->lbl_val = lv;
    lv_obj_set_style_text_color(lv, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lv, SF(18), 0);
    lv_label_set_text(lv, "--.- °C");
    lv_obj_align(lv, LV_ALIGN_TOP_RIGHT, SX(-2), SY(6));

    /* 24-hour mini sparkline chart */
    lv_obj_t * ch = lv_chart_create(root);
    v->chart = ch;
    lv_obj_set_size(ch, LV_PCT(100), SY(78));
    lv_obj_align(ch, LV_ALIGN_BOTTOM_MID, 0, SY(-2));
    lv_obj_set_style_bg_color(ch, lv_color_hex(0x101722), 0);
    lv_obj_set_style_border_color(ch, lv_color_hex(0x1e2c3e), 0);
    lv_obj_set_style_border_width(ch, 1, 0);
    lv_obj_set_style_radius(ch, 6, 0);
    lv_obj_set_style_pad_all(ch, 2, 0);
    lv_obj_clear_flag(ch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ch, LV_OBJ_FLAG_CLICKABLE);

    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, TEMPLOGGER_POINTS);
    lv_obj_set_style_size(ch, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);

    /* 3 subtle horizontal raster lines, 0 vertical */
    lv_chart_set_div_line_count(ch, 3, 0);
    lv_obj_set_style_line_color(ch, lv_color_hex(0x1e2c3e), LV_PART_MAIN);
    lv_obj_set_style_line_opa(ch, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_line_width(ch, 1, LV_PART_MAIN);

    /* Series: Room (blue), Setpoint (red), Outside (yellow) */
    v->ser_room = lv_chart_add_series(ch, lv_color_hex(0x3388ff), LV_CHART_AXIS_PRIMARY_Y);
    v->ser_sp   = lv_chart_add_series(ch, lv_color_hex(0xff4444), LV_CHART_AXIS_PRIMARY_Y);
    v->ser_out  = lv_chart_add_series(ch, lv_color_hex(0xffbb00), LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    return v;
}

static void update_templogger_view(slot_templogger_view_t * v) {
    if (!v) return;
    templogger_snapshot_t snap;
    templogger_get_snapshot(&snap);

    if (v->lbl_val) {
        if (snap.latest_room > 0.0f)
            lv_label_set_text_fmt(v->lbl_val, "%.1f °C", snap.latest_room);
        else
            lv_label_set_text(v->lbl_val, "--.- °C");
    }

    if (!v->chart || !v->ser_room || !v->ser_sp || !v->ser_out) return;

    /* Calculate dynamic Y range with margin */
    float mn = 1e9f, mx = -1e9f;
    int has_pts = 0;
    for (int i = 0; i < TEMPLOGGER_POINTS; i++) {
        float r = snap.room[i];
        if (r > 0.0f && r < 50.0f) { if (r < mn) mn = r; if (r > mx) mx = r; has_pts = 1; }
        float sp = snap.setpoint[i];
        if (sp > 0.0f && sp < 40.0f) { if (sp < mn) mn = sp; if (sp > mx) mx = sp; has_pts = 1; }
        float out = snap.outside[i];
        if (out > -50.0f && out < 60.0f) { if (out < mn) mn = out; if (out > mx) mx = out; has_pts = 1; }
    }
    if (!has_pts) { mn = 15.0f; mx = 25.0f; }
    else {
        mn -= 1.0f;
        mx += 1.0f;
        if (mx - mn < 4.0f) mx = mn + 4.0f;
    }

    lv_chart_set_range(v->chart, LV_CHART_AXIS_PRIMARY_Y,
                       (lv_coord_t)(mn * 10.0f), (lv_coord_t)(mx * 10.0f));

    for (int i = 0; i < TEMPLOGGER_POINTS; i++) {
        float r = snap.room[i];
        v->ser_room->y_points[i] = (r > 0.0f && r < 50.0f) ? (lv_coord_t)(r * 10.0f) : LV_CHART_POINT_NONE;

        float sp = snap.setpoint[i];
        v->ser_sp->y_points[i] = (sp > 0.0f && sp < 40.0f) ? (lv_coord_t)(sp * 10.0f) : LV_CHART_POINT_NONE;

        float out = snap.outside[i];
        v->ser_out->y_points[i] = (out > -50.0f && out < 60.0f) ? (lv_coord_t)(out * 10.0f) : LV_CHART_POINT_NONE;
    }
    lv_chart_refresh(v->chart);
}

/* Either render the agenda overlay (when the slot is bound to "local:agenda")
 * or the Sonos overlay (when bound to "local:sonos") or the Waste overlay
 * (when bound to "local:waste") or the 24h temperature logger mini-graph
 * or fall back to the normal integration/built-in render. */
static int slot_render_or_agenda(int slot, int ai, lv_obj_t * lbl_main, lv_obj_t * lbl_sub) {
    const char * b = tile_slots_binding(slot);
    const integration_meta_t * m = tile_slots_meta_for(slot);

    /* Update title dynamically based on binding */
    if (slot >= 0 && slot < TILE_SLOT_COUNT && lbl_title_slot[slot]) {
        const char * title_to_show = NULL;
        if (b && (!strcmp(b, "local:templogger") || !strcmp(b, "temperaturelogger"))) {
            title_to_show = tr("Temperatuur", "Temperature");
        } else if (b && (!strcmp(b, "local:agenda") || !strcmp(b, "agenda") || (m && !strcmp(m->tile_type, "agenda_list")))) {
            title_to_show = tr("Agenda", "Calendar");
        } else if (b && (!strcmp(b, "local:sonos") || !strcmp(b, "sonos") || (m && !strcmp(m->tile_type, "media_player")))) {
            title_to_show = "Sonos";
        } else if (b && !strcmp(b, "local:waste")) {
            title_to_show = tr("Afval", "Waste");
        } else if (b && m && m->tile_title[0]) {
            title_to_show = m->tile_title;
        } else {
            /* Default titles */
            switch (slot) {
                case TILE_SLOT_ENERGY: title_to_show = tr("Energie", "Energy"); break;
                case TILE_SLOT_FAMILY: title_to_show = tr("Familie", "Family"); break;
                case TILE_SLOT_VENT:   title_to_show = tr("Ventilatie", "Vent"); break;
                case TILE_SLOT_WATER:  title_to_show = tr("Water", "Water"); break;
                default: break;
            }
        }
        if (title_to_show) {
            lv_label_set_text(lbl_title_slot[slot], title_to_show);
        }
    }

    if (b && (!strcmp(b, "local:agenda") || !strcmp(b, "agenda") || (m && !strcmp(m->tile_type, "agenda_list")))) {
        if (slot_sonos[ai]) lv_obj_add_flag(slot_sonos[ai]->root, LV_OBJ_FLAG_HIDDEN);
        if (slot_waste[ai]) lv_obj_add_flag(slot_waste[ai], LV_OBJ_FLAG_HIDDEN);
        if (slot_templogger[ai]) lv_obj_add_flag(slot_templogger[ai]->root, LV_OBJ_FLAG_HIDDEN);
        fill_agenda_overlay(slot_agenda[ai]);
        if (slot_agenda[ai]) { lv_obj_clear_flag(slot_agenda[ai]->root, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(slot_agenda[ai]->root); }
        return 1;
    }
    if (slot_agenda[ai]) lv_obj_add_flag(slot_agenda[ai]->root, LV_OBJ_FLAG_HIDDEN);

    if (b && (!strcmp(b, "local:sonos") || !strcmp(b, "sonos") || (m && !strcmp(m->tile_type, "media_player")))) {
        if (slot_waste[ai]) lv_obj_add_flag(slot_waste[ai], LV_OBJ_FLAG_HIDDEN);
        if (slot_templogger[ai]) lv_obj_add_flag(slot_templogger[ai]->root, LV_OBJ_FLAG_HIDDEN);
        if (slot_sonos[ai]) {
            update_sonos_view(slot_sonos[ai]);
            lv_obj_clear_flag(slot_sonos[ai]->root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(slot_sonos[ai]->root);
        }
        return 1;
    }
    if (slot_sonos[ai]) lv_obj_add_flag(slot_sonos[ai]->root, LV_OBJ_FLAG_HIDDEN);

    if (b && !strcmp(b, "local:waste")) {
        if (slot_templogger[ai]) lv_obj_add_flag(slot_templogger[ai]->root, LV_OBJ_FLAG_HIDDEN);
        fill_waste_overlay(slot_waste[ai]);
        if (slot_waste[ai]) {
            lv_obj_clear_flag(slot_waste[ai], LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(slot_waste[ai]);
        }
        return 1;
    }
    if (slot_waste[ai]) lv_obj_add_flag(slot_waste[ai], LV_OBJ_FLAG_HIDDEN);

    if (b && (!strcmp(b, "local:templogger") || !strcmp(b, "temperaturelogger"))) {
        if (slot_templogger[ai]) {
            update_templogger_view(slot_templogger[ai]);
            lv_obj_clear_flag(slot_templogger[ai]->root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(slot_templogger[ai]->root);
        }
        return 1;
    }
    if (slot_templogger[ai]) lv_obj_add_flag(slot_templogger[ai]->root, LV_OBJ_FLAG_HIDDEN);

    return render_tile_slot(slot, lbl_main, lbl_sub);
}

/* Render an integration's latest_value/subtitle into the given pair of
 * labels. Returns 1 if a binding existed (caller should skip the built-in
 * refresh path), 0 otherwise. */
static int render_tile_slot(int slot, lv_obj_t * lbl_main, lv_obj_t * lbl_sub) {
    const integration_meta_t * m = tile_slots_meta_for(slot);
    if (!m) return 0;
    if (lbl_main) {
        const char * v = (const char *)m->latest_value;
        if (v && v[0])
            lv_label_set_text_fmt(lbl_main, "%s%s%s", v,
                                  m->value_unit[0] ? " " : "",
                                  m->value_unit);
        else
            lv_label_set_text_fmt(lbl_main, "%s...", m->tile_title);
    }
    if (lbl_sub) {
        const char * s = (const char *)m->latest_subtitle;
        if (s && s[0])
            lv_label_set_text_fmt(lbl_sub, "%s%s%s", s,
                                  m->subtitle_unit[0] ? " " : "",
                                  m->subtitle_unit);
        else
            lv_label_set_text(lbl_sub, "");
    }
    return 1;
}

/* Render a specific integration's latest value/subtitle/title into a page-1
 * tile's labels (used by the auto-rotate slot, which shows arbitrary
 * integrations rather than a fixed binding). */
static void render_meta_into(const integration_meta_t * m,
                             lv_obj_t * title, lv_obj_t * main, lv_obj_t * sub) {
    if (!m) return;
    if (title) lv_label_set_text(title, m->tile_title[0] ? m->tile_title : m->name);
    if (main) {
        const char * v = (const char *)m->latest_value;
        if (v && v[0]) lv_label_set_text_fmt(main, "%s%s%s", v,
                              m->value_unit[0] ? " " : "", m->value_unit);
        else           lv_label_set_text_fmt(main, "%s...", m->tile_title);
    }
    if (sub) {
        const char * s = (const char *)m->latest_subtitle;
        if (s && s[0]) lv_label_set_text_fmt(sub, "%s%s%s", s,
                              m->subtitle_unit[0] ? " " : "", m->subtitle_unit);
        else           lv_label_set_text(sub, "");
    }
}

/* Render a built-in ("local:") integration's live value into the rotate slot's
 * labels — energy/water/vent/family/air. Mirrors the fixed tiles' data sources
 * so the auto-rotate tile can cycle the local integrations too. */
static void render_local_into(const char * id, lv_obj_t * title,
                              lv_obj_t * main, lv_obj_t * sub) {
    if (sub) lv_label_set_text(sub, "");
    if (!strcmp(id, "local:energy")) {
        if (title) lv_label_set_text(title, tr("Energie", "Energy"));
        if (main)  lv_label_set_text_fmt(main, "%.0f W", energy_power_w());
    } else if (!strcmp(id, "local:water")) {
        if (title) lv_label_set_text(title, tr("Water", "Water"));
        if ((settings.enable_p1_water || settings.client_mode) && hw_state.connected_water) {
            if (main) lv_label_set_text_fmt(main, "%.1f L/min", hw_state.water_lpm);
            if (sub)  lv_label_set_text_fmt(sub, "%.2f m3", hw_state.water_total_m3);
        } else {
            if (main) lv_label_set_text(main, "--");
            if (sub)  lv_label_set_text(sub, "");
        }
    } else if (!strcmp(id, "local:vent")) {
        if (title) lv_label_set_text(title, tr("Ventilatie", "Ventilation"));
        if (vent_state.connected) {
            if (main) lv_label_set_text_fmt(main, "%d%%", vent_state.exh_fan_pct);
        } else if (main) lv_label_set_text(main, tr("offline", "offline"));
    } else if (!strcmp(id, "local:family")) {
        if (title) lv_label_set_text(title, tr("Familie", "Family"));
        if (main)  lv_label_set_text(main, ha_state.loc_a[0] ? (const char *)ha_state.loc_a : "-");
        if (sub)   lv_label_set_text(sub,  (const char *)ha_state.loc_b);
        if (lbl_life360_c) lv_label_set_text(lbl_life360_c, ha_state.loc_c[0] ? (const char *)ha_state.loc_c : "");
    } else if (!strcmp(id, "local:air")) {
        if (title) lv_label_set_text(title, tr("Lucht", "Air"));
        if (main)  lv_label_set_text_fmt(main, "%d ppm", toon_state.eco2);
        if (sub)   lv_label_set_text(sub, air_quality_label(toon_state.eco2, toon_state.tvoc));
    } else if (!strcmp(id, "local:sonos")) {
        sonos_state_t st;
        sonos_get_snapshot(&st);
        if (title) lv_label_set_text_fmt(title, "Sonos - %s", st.zone[0] ? st.zone : "Keuken");
        if (st.connected) {
            if (main) lv_label_set_text(main, st.artist[0] ? st.artist : (st.playing ? tr("Speelt", "Playing") : tr("Gepauzeerd", "Paused")));
            if (sub)  lv_label_set_text(sub, st.title[0] ? st.title : "");
        } else {
            if (main) lv_label_set_text(main, tr("offline", "offline"));
            if (sub)  lv_label_set_text(sub, "");
        }
    } else if (!strcmp(id, "local:waste")) {
        if (title) lv_label_set_text(title, tr("Afval", "Waste"));
        waste_pickup_t p1 = {0}, p2 = {0};
        int n = waste_state.connected ? waste_next_2_pickups(&p1, &p2) : 0;
        if (n >= 1) {
            int mo = atoi(p1.date + 5), d = atoi(p1.date + 8);
            if (main) lv_label_set_text_fmt(main, "%d-%d %s", d, mo, p1.labels);
            if (sub) {
                if (n >= 2) {
                    int mo2 = atoi(p2.date + 5), d2 = atoi(p2.date + 8);
                    lv_label_set_text_fmt(sub, "%d-%d %s", d2, mo2, p2.labels);
                } else {
                    lv_label_set_text(sub, "");
                }
            }
        } else {
            if (main) lv_label_set_text(main, waste_state.connected ? tr("geen", "none") : "...");
            if (sub)  lv_label_set_text(sub, "");
        }
    } else if (!strcmp(id, "local:agenda")) {
        if (title) lv_label_set_text(title, tr("Agenda", "Calendar"));
        if (calendar_state.count > 0) {
            calendar_event_t * ev = &calendar_state.ev[0];
            if (main) lv_label_set_text(main, ev->summary);
            if (sub)  lv_label_set_text_fmt(sub, "%s %s", ev->date + 5, ev->time[0] ? ev->time : "");
        } else {
            if (main) lv_label_set_text(main, tr("Geen afspraken", "No events"));
            if (sub)  lv_label_set_text(sub, "");
        }
    }
}

/* Return the n-th (wrapping) integration id from the comma-separated
 * settings.tile_rotate_members list. Returns 0 if the list is empty. */
static int rotate_member_at(int n, char * out, size_t sz) {
    const char * s = settings.tile_rotate_members;
    if (!s[0]) return 0;
    int count = 1;
    for (const char * p = s; *p; p++) if (*p == ',') count++;
    int want = ((n % count) + count) % count;
    int idx = 0;
    const char * start = s;
    for (const char * p = s; ; p++) {
        if (*p == ',' || *p == 0) {
            if (idx == want) {
                size_t len = (size_t)(p - start);
                if (len >= sz) len = sz - 1;
                memcpy(out, start, len); out[len] = 0;
                /* trim spaces */
                while (out[0] == ' ') memmove(out, out + 1, strlen(out));
                return out[0] ? 1 : 0;
            }
            idx++; start = p + 1;
            if (*p == 0) break;
        }
    }
    return 0;
}

static void on_tile_light_btn1(lv_event_t * e) {
    (void)e;
    if (settings.domoticz_tile_idx1 >= 0) {
        domoticz_switch_async(settings.domoticz_tile_idx1, "Toggle");
    }
}
static void on_tile_light_btn2(lv_event_t * e) {
    (void)e;
    if (settings.domoticz_tile_idx2 >= 0) {
        domoticz_switch_async(settings.domoticz_tile_idx2, "Toggle");
    }
}

static const domoticz_dev_t * find_domoticz_dev(int idx) {
    if (idx < 0) return NULL;
    for (int i = 0; i < domoticz_state.count && i < DOMOTICZ_MAX_DEV; i++) {
        if (domoticz_state.dev[i].idx == idx)
            return &domoticz_state.dev[i];
    }
    return NULL;
}

static void update_tile_light_btn(lv_obj_t * btn, lv_obj_t * lbl_name, lv_obj_t * sw_btn, lv_obj_t * lbl_st, int idx) {
    if (!btn) return;
    if (idx < 0) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
    const domoticz_dev_t * d = find_domoticz_dev(idx);
    if (d) {
        if (lbl_name) lv_label_set_text(lbl_name, d->name);
        if (lbl_st) lv_label_set_text(lbl_st, d->on ? tr("AAN", "ON") : tr("UIT", "OFF"));
        lv_obj_set_style_bg_color(btn, lv_color_hex(settings.home_light_mode ? 0xedf2f7 : 0x1e2838), 0);
        if (sw_btn) lv_obj_set_style_bg_color(sw_btn, lv_color_hex(d->on ? 0x2e6e3a : (settings.home_light_mode ? 0xd0d8e2 : 0x283848)), 0);
        if (lbl_name) lv_obj_set_style_text_color(lbl_name, lv_color_hex(COL_TEXT_HI), 0);
        if (lbl_st) lv_obj_set_style_text_color(lbl_st, lv_color_hex(d->on ? 0xffffff : (settings.home_light_mode ? 0x485868 : 0x88aabb)), 0);
    } else {
        if (lbl_name) lv_label_set_text_fmt(lbl_name, "Dev #%d", idx);
        if (lbl_st) lv_label_set_text(lbl_st, "--");
        lv_obj_set_style_bg_color(btn, lv_color_hex(settings.home_light_mode ? 0xedf2f7 : 0x1e2838), 0);
        if (sw_btn) lv_obj_set_style_bg_color(sw_btn, lv_color_hex(settings.home_light_mode ? 0xd0d8e2 : 0x283848), 0);
        if (lbl_name) lv_obj_set_style_text_color(lbl_name, lv_color_hex(COL_TEXT_HI), 0);
        if (lbl_st) lv_obj_set_style_text_color(lbl_st, lv_color_hex(settings.home_light_mode ? 0x485868 : 0x88aabb), 0);
    }
}

static void update_tile_lights(void) {
    if (!tile_lights_lbl_default && !tile_lights_btn1) return;
    int idx1 = settings.domoticz_tile_idx1;
    int idx2 = settings.domoticz_tile_idx2;
    int has1 = (idx1 >= 0);
    int has2 = (idx2 >= 0);

    if (!has1 && !has2) {
        if (tile_lights_lbl_default) lv_obj_clear_flag(tile_lights_lbl_default, LV_OBJ_FLAG_HIDDEN);
        if (tile_lights_btn1) lv_obj_add_flag(tile_lights_btn1, LV_OBJ_FLAG_HIDDEN);
        if (tile_lights_btn2) lv_obj_add_flag(tile_lights_btn2, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (tile_lights_lbl_default) lv_obj_add_flag(tile_lights_lbl_default, LV_OBJ_FLAG_HIDDEN);

    if (has1 && has2) {
        lv_coord_t btn_h = (DISP_VER < 600) ? 32 : SY(38);
        lv_coord_t y1 = (DISP_VER < 600) ? 40 : SY(40);
        lv_coord_t y2 = (DISP_VER < 600) ? 76 : SY(84);
        if (tile_lights_btn1) {
            lv_obj_set_size(tile_lights_btn1, LV_PCT(92), btn_h);
            lv_obj_align(tile_lights_btn1, LV_ALIGN_TOP_MID, 0, y1);
            update_tile_light_btn(tile_lights_btn1, tile_lights_lbl1_name, tile_lights_sw1, tile_lights_lbl1_st, idx1);
        }
        if (tile_lights_btn2) {
            lv_obj_set_size(tile_lights_btn2, LV_PCT(92), btn_h);
            lv_obj_align(tile_lights_btn2, LV_ALIGN_TOP_MID, 0, y2);
            update_tile_light_btn(tile_lights_btn2, tile_lights_lbl2_name, tile_lights_sw2, tile_lights_lbl2_st, idx2);
        }
    } else if (has1) {
        if (tile_lights_btn1) {
            lv_obj_set_size(tile_lights_btn1, LV_PCT(92), (DISP_VER < 600) ? 38 : SY(46));
            lv_obj_align(tile_lights_btn1, LV_ALIGN_CENTER, 0, SY(12));
            update_tile_light_btn(tile_lights_btn1, tile_lights_lbl1_name, tile_lights_sw1, tile_lights_lbl1_st, idx1);
        }
        if (tile_lights_btn2) lv_obj_add_flag(tile_lights_btn2, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (tile_lights_btn2) {
            lv_obj_set_size(tile_lights_btn2, LV_PCT(92), (DISP_VER < 600) ? 38 : SY(46));
            lv_obj_align(tile_lights_btn2, LV_ALIGN_CENTER, 0, SY(12));
            update_tile_light_btn(tile_lights_btn2, tile_lights_lbl2_name, tile_lights_sw2, tile_lights_lbl2_st, idx2);
        }
        if (tile_lights_btn1) lv_obj_add_flag(tile_lights_btn1, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (!scr_root || lv_scr_act() != scr_root) return;

    update_tile_lights();

    /* Only manage page-0 tile visibility while page 0 is showing — otherwise
     * this would clear the HIDDEN flag on the page-0 tiles every tick and let
     * them (e.g. the Family/Life360 tile) bleed through behind the page-2
     * swipe slots. */
    if (home_tile_page == 0) apply_offline_tile_visibility();

    /* Custom-layout info tiles — fill from cached state (no I/O). */
    if (tile_news_sum_lbl) {
        lv_coord_t th = lv_obj_get_height(lv_obj_get_parent(tile_news_sum_lbl));
        if (th <= 0) th = SY(150);
        int avail_h = th - SY(45);
        int line_h = SY(20);
        if (line_h < 14) line_h = 14;
        int max_lines = avail_h / line_h;
        if (max_lines < 3) max_lines = 3;
        if (max_lines > 20) max_lines = 20;

        char buf[1500] = ""; int nn = news_count(); if (nn > max_lines) nn = max_lines;
        for (int i = 0; i < nn; i++) {
            char ti[120];
            if (news_item(i, ti, sizeof ti, NULL, 0) != 0) continue;
            strncat(buf, "\xE2\x80\xA2 ", sizeof buf - strlen(buf) - 1);   /* bullet */
            strncat(buf, ti, sizeof buf - strlen(buf) - 1);
            strncat(buf, "\n", sizeof buf - strlen(buf) - 1);
        }
        lv_label_set_text(tile_news_sum_lbl, buf[0] ? buf : tr("Geen nieuws", "No news"));
    }
    if (tile_cal_dates && tile_cal_times && tile_cal_titles) {
        lv_obj_t * p = lv_obj_get_parent(tile_cal_dates);
        lv_coord_t th = lv_obj_get_height(p);
        if (th <= 0) th = SY(150);
        lv_coord_t avail_h = th - SY(46);
        fill_agenda_view(p, tile_cal_dates, tile_cal_times, tile_cal_titles, avail_h);
    }
    if (tile_sonos_view && !lv_obj_has_flag(lv_obj_get_parent(tile_sonos_view->root), LV_OBJ_FLAG_HIDDEN)) {
        update_sonos_view(tile_sonos_view);
    }

    /* Marketplace tile overrides — if a slot is bound, render the
     * integration's data into the matching tile's labels and remember
     * to skip the built-in refresh path further down. The vent tile's
     * buttons/fan stay visible (no separate hide pass) — accepted UX
     * trade-off for keeping refresh logic local. */
    int slot_active[TILE_SLOT_COUNT] = {0};
    slot_active[TILE_SLOT_ENERGY] = slot_render_or_agenda(TILE_SLOT_ENERGY, 0,
                                                    lbl_energy_w, lbl_energy_gas);
    slot_active[TILE_SLOT_FAMILY] = slot_render_or_agenda(TILE_SLOT_FAMILY, 1,
                                                    lbl_life360_a, lbl_life360_b);
    slot_active[TILE_SLOT_VENT]   = slot_render_or_agenda(TILE_SLOT_VENT, 2,
                                                    lbl_boiler_state, lbl_boiler_pressure);
    slot_active[TILE_SLOT_WATER]  = slot_render_or_agenda(TILE_SLOT_WATER, 3,
                                                    lbl_inbox_main, lbl_inbox_sub);

    /* Page-1 (swipe) slots — render the bound integration, or a placeholder.
     * Slot 0 is reserved for the efanlamp tile when a host is configured. */
    for (int i = 0; i < g_p1_count; i++) {
        if (i == 0 && settings.efanlamp_host[0]) {
            /* efanlamp tile: show fan speed + light state */
            if (p1_title[i]) lv_label_set_text(p1_title[i], tr("Ventilator", "Fan Lamp"));
            if (p1_main[i]) {
                if (!efanlamp.connected)
                    lv_label_set_text(p1_main[i], "...");
                else if (efanlamp.fan_on)
                    lv_label_set_text_fmt(p1_main[i], "L%d", efanlamp.fan_speed);
                else
                    lv_label_set_text(p1_main[i], tr("Uit", "Off"));
            }
            if (p1_sub[i]) {
                if (efanlamp.connected && efanlamp.light_on)
                    lv_label_set_text_fmt(p1_sub[i], tr("lamp %d%%", "lamp %d%%"), efanlamp.light_brightness);
                else if (efanlamp.connected)
                    lv_label_set_text(p1_sub[i], tr("lamp uit", "lamp off"));
                else
                    lv_label_set_text(p1_sub[i], "");
            }
            continue;
        }
        const char * b = tile_slots_binding(TILE_SLOT_P1_0 + i);
        if (b && !strcmp(b, "local:sonos")) {
            sonos_state_t st;
            sonos_get_snapshot(&st);
            if (p1_title[i]) lv_label_set_text_fmt(p1_title[i], "Sonos - %s", st.zone[0] ? st.zone : "Keuken");
            if (st.connected) {
                if (p1_main[i]) lv_label_set_text(p1_main[i], st.artist[0] ? st.artist : (st.playing ? tr("Speelt", "Playing") : tr("Gepauzeerd", "Paused")));
                if (p1_sub[i])  lv_label_set_text_fmt(p1_sub[i], "%s%s", st.title[0] ? st.title : "", (!st.playing) ? tr(" (pauze)", " (paused)") : "");
            } else {
                if (p1_main[i]) lv_label_set_text(p1_main[i], tr("offline", "offline"));
                if (p1_sub[i])  lv_label_set_text(p1_sub[i], "");
            }
            continue;
        }
        if (b && !strcmp(b, "local:agenda")) {
            if (p1_title[i]) lv_label_set_text(p1_title[i], tr("Agenda", "Calendar"));
            if (calendar_state.count > 0) {
                calendar_event_t * ev = &calendar_state.ev[0];
                if (p1_main[i]) lv_label_set_text(p1_main[i], ev->summary);
                if (p1_sub[i])  lv_label_set_text_fmt(p1_sub[i], "%s %s", ev->date + 5, ev->time[0] ? ev->time : "");
            } else {
                if (p1_main[i]) lv_label_set_text(p1_main[i], tr("Geen afspraken", "No events"));
                if (p1_sub[i])  lv_label_set_text(p1_sub[i], "");
            }
            continue;
        }
        if (b && !strcmp(b, "local:waste")) {
            if (p1_title[i]) lv_label_set_text(p1_title[i], tr("Afval", "Waste"));
            waste_pickup_t p1 = {0}, p2 = {0};
            int n = waste_state.connected ? waste_next_2_pickups(&p1, &p2) : 0;
            if (n >= 1) {
                int mo = atoi(p1.date + 5), d = atoi(p1.date + 8);
                if (p1_main[i]) lv_label_set_text_fmt(p1_main[i], "%d-%d %s", d, mo, p1.labels);
                if (p1_sub[i]) {
                    if (n >= 2) {
                        int mo2 = atoi(p2.date + 5), d2 = atoi(p2.date + 8);
                        lv_label_set_text_fmt(p1_sub[i], "%d-%d %s", d2, mo2, p2.labels);
                    } else {
                        lv_label_set_text(p1_sub[i], "");
                    }
                }
            } else {
                if (p1_main[i]) lv_label_set_text(p1_main[i], waste_state.connected ? tr("geen", "none") : "...");
                if (p1_sub[i])  lv_label_set_text(p1_sub[i], "");
            }
            continue;
        }
        if (b && (!strcmp(b, "local:templogger") || !strcmp(b, "temperaturelogger"))) {
            if (p1_title[i]) lv_label_set_text(p1_title[i], tr("Temperatuur", "Temperature"));
            float r = 0, sp = 0, out = 0;
            templogger_get_latest(&r, &sp, &out);
            if (p1_main[i]) {
                if (r > 0.0f) lv_label_set_text_fmt(p1_main[i], "%.1f °C", r);
                else lv_label_set_text(p1_main[i], "--.- °C");
            }
            if (p1_sub[i]) {
                if (out > -50.0f && sp > 0.0f)
                    lv_label_set_text_fmt(p1_sub[i], tr("buiten %.1f° | doel %.1f°", "out %.1f° | set %.1f°"), out, sp);
                else if (out > -50.0f)
                    lv_label_set_text_fmt(p1_sub[i], tr("buiten %.1f°", "out %.1f°"), out);
                else
                    lv_label_set_text(p1_sub[i], "");
            }
            continue;
        }
        if (b && strncmp(b, "local:", 6) == 0) {
            render_local_into(b, p1_title[i], p1_main[i], p1_sub[i]);
            continue;
        }
        const integration_meta_t * m = tile_slots_meta_for(TILE_SLOT_P1_0 + i);
        if (m) {
            if (p1_title[i]) lv_label_set_text(p1_title[i],
                                 m->tile_title[0] ? m->tile_title : m->name);
            render_tile_slot(TILE_SLOT_P1_0 + i, p1_main[i], p1_sub[i]);
        } else {
            if (p1_title[i]) lv_label_set_text(p1_title[i], "");
            if (p1_main[i])  lv_label_set_text(p1_main[i], tr(LV_SYMBOL_PLUS "  Tik om te koppelen", LV_SYMBOL_PLUS "  Tap to assign"));
            if (p1_sub[i])   lv_label_set_text(p1_sub[i], "");
        }
    }

    /* News ticker text — rebuild only when the lead headline changes (every
     * ~15 min) so the scroll animation isn't reset each second. */
    if (news_ticker) {
        /* In custom layout the ticker can be hidden via the layout — don't let
         * the text refresh re-show it. */
        int layout_hides_ticker = 0;
        if (settings.custom_layout_enabled) {
            const layout_tile_t * L = layout_find(LT_NEWS_TICKER);
            layout_hides_ticker = (!L || !L->visible);
        }
        if (settings.news_enabled && news_count() > 0 && home_tile_page == 0 && !layout_hides_ticker) {
            if (news_box) lv_obj_clear_flag(news_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(news_ticker, LV_OBJ_FLAG_HIDDEN);
            char t0[NEWS_TITLE_MAX], l0[NEWS_LINK_MAX];
            news_item(0, t0, sizeof t0, l0, sizeof l0);
            int nn = news_count();
            const char * cur_txt = news_ticker ? lv_label_get_text(news_ticker) : "";
            if (strcmp(t0, news_last_first) != 0 || nn != news_last_nn || !cur_txt || cur_txt[0] == '\0') {
                snprintf(news_last_first, sizeof news_last_first, "%s", t0);
                news_last_nn = nn;
                char buf[1400]; buf[0] = 0;
                int nf = news_feed_count();
                int per_feed = nf > 0 ? (12 + nf - 1) / nf : 12;
                int feed_cnt[NEWS_MAX_FEEDS] = {0};
                int added = 0;
                for (int i = 0; i < nn && added < 12; i++) {
                    int f = news_item_feed(i);
                    if (f >= 0 && f < NEWS_MAX_FEEDS) {
                        if (feed_cnt[f] >= per_feed) continue;
                        feed_cnt[f]++;
                    }
                    char ti[NEWS_TITLE_MAX], li[NEWS_LINK_MAX];
                    if (news_item(i, ti, sizeof ti, li, sizeof li) != 0) continue;
                    if (added > 0) strncat(buf, "    -    ", sizeof buf - strlen(buf) - 1);
                    strncat(buf, ti, sizeof buf - strlen(buf) - 1);
                    added++;
                }
                lv_label_set_text(news_ticker, buf);
            }
        } else {
            if (news_box) lv_obj_add_flag(news_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(news_ticker, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (news_modal && news_seq() != news_modal_seq) {
        news_modal_seq = news_seq();
        news_list_rebuild();
        if (!news_had_items && news_count() > 0) {
            news_had_items = 1;
            news_show_body(0);
        }
    }

    /* Auto-rotate: page-1 slot 0 cycles through the chosen integrations every
     * tile_rotate_seconds. Driven off this 1 Hz refresh — no extra timer. */
    if (settings.tile_rotate_enabled && settings.tile_rotate_members[0]) {
        static int rot_ctr = 0, rot_idx = 0;
        int period = settings.tile_rotate_seconds < 3 ? 3 : settings.tile_rotate_seconds;
        if (++rot_ctr >= period) { rot_ctr = 0; rot_idx++; }
        char id[48];
        if (rotate_member_at(rot_idx, id, sizeof id)) {
            snprintf(g_current_rot_id, sizeof g_current_rot_id, "%s", id);
            if (strncmp(id, "local:", 6) == 0) {
                render_local_into(id, p1_title[0], p1_main[0], p1_sub[0]);
            } else {
                const integration_meta_t * m = tile_slots_integration_by_id(id);
                if (m) render_meta_into(m, p1_title[0], p1_main[0], p1_sub[0]);
            }
        }
    } else {
        g_current_rot_id[0] = '\0';
    }

    /* Update-available banner — toggled live so the user sees it within
     * a refresh tick of the background checker finding a new release. */
    if (update_banner && update_banner_lbl) {
        const char * sk = update_modal_get_skipped_version();
        int skipped = (sk[0] && strcmp(sk, g_update_state.latest_version) == 0);
        int relevant = g_update_state.available && !skipped && !update_modal_already_installed();
        if (!relevant) update_modal_set_minimized(false);

        /* Auto-warn on newly detected release by popping up the FT update modal once */
        static char s_warned_version[UPDATE_VERSION_MAX] = "";
        if (relevant && strcmp(s_warned_version, g_update_state.latest_version) != 0) {
            snprintf(s_warned_version, sizeof s_warned_version, "%s", g_update_state.latest_version);
            update_modal_open(scr_root);
        }

        /* Banner when relevant + not minimized; envelope when minimized. */
        if (relevant && !update_modal_is_minimized()) {
            lv_label_set_text_fmt(update_banner_lbl,
                                  tr("%s beschikbaar  -  tik voor details",
                                     "%s available  -  tap for details"),
                                  g_update_state.latest_version);
            lv_obj_clear_flag(update_banner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(update_banner, LV_OBJ_FLAG_HIDDEN);
        }
        if (update_env_btn) {
            if (relevant && update_modal_is_minimized()) lv_obj_clear_flag(update_env_btn, LV_OBJ_FLAG_HIDDEN);
            else                                         lv_obj_add_flag(update_env_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Inbox envelope — show it whenever happ_usermsg has messages; the red
     * badge carries the unread count. The inbox model is fed live by the
     * BoxTalk notifications subscription (inbox_parse_dataset). */
    if (envelope_btn && envelope_badge && envelope_badge_lbl) {
        if (inbox_count > 0) {
            lv_obj_clear_flag(envelope_btn, LV_OBJ_FLAG_HIDDEN);
            if (inbox_unread > 0) {
                if (inbox_unread > 9) lv_label_set_text(envelope_badge_lbl, "9+");
                else                  lv_label_set_text_fmt(envelope_badge_lbl, "%d", inbox_unread);
                lv_obj_clear_flag(envelope_badge, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(envelope_badge, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(envelope_btn,   LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(envelope_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Update/About modal tick */
    update_modal_tick();

    /* Expire any pending +/- temporary override once the schedule advances. */
    boxtalk_tick();

    /* Clock on clock tile */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (lbl_t_clock_h && lbl_t_clock_m) {
        char ch[8], cm[8];
        strftime(ch, sizeof(ch), "%H", &tm);
        strftime(cm, sizeof(cm), "%M", &tm);
        lv_label_set_text(lbl_t_clock_h, ch);
        lv_label_set_text(lbl_t_clock_m, cm);
    }
    if (lbl_t_date) {
        char dt[48];
        i18n_date_short(dt, sizeof(dt), &tm);   /* localised — strftime is C-locale (always English) */
        lv_label_set_text(lbl_t_date, dt);
    }
    /* Refresh the moon-phase icon at most once per refresh tick — phase
     * changes daily, not by-the-second, but keep this cheap: a single
     * src-swap when the phase slot moves. */
    if (moon_phase_img) {
        /* Only show if enabled in settings and it is nighttime */
        if (!settings.clock_moon_phase || is_daytime_now()) {
            lv_obj_add_flag(moon_phase_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            const lv_img_dsc_t * ph = moon_phase_icon(40);
            if (lv_img_get_src(moon_phase_img) != ph)
                lv_img_set_src(moon_phase_img, ph);
            lv_obj_clear_flag(moon_phase_img, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (toon_state.indoor_temp > 0) {
        lv_label_set_text_fmt(lbl_t_temp, "%.1f°C", display_indoor_temp(toon_state.indoor_temp));
        lv_obj_update_layout(lbl_t_temp);
        int temp_y = th_is_compact ? 2 : (DISP_VER < 600 ? 6 : 14);
        lv_obj_align(lbl_t_temp, LV_ALIGN_TOP_MID, 0, temp_y);
        int flame_gap = th_is_compact ? 4 : 8;
        if (tile_img_flame) lv_obj_align_to(tile_img_flame, lbl_t_temp, LV_ALIGN_OUT_RIGHT_MID, flame_gap, 0);
        if (tile_img_faucet) lv_obj_align_to(tile_img_faucet, lbl_t_temp, LV_ALIGN_OUT_RIGHT_MID, flame_gap, 0);
        if (tile_img_drop) lv_obj_align_to(tile_img_drop, lbl_t_temp, LV_ALIGN_OUT_RIGHT_MID, flame_gap + 16, 6);
    }
    /* Setpoint visible at all times; the "to" prefix only when the CH
     * burner is actively heating toward it (idle just shows the target
     * temperature, no arrow). */
    if (toon_state.setpoint > 0) {
        if (toon_state.burner_on)
            lv_label_set_text_fmt(lbl_t_setpoint, tr("naar %.1f°C", "to %.1f°C"), toon_state.setpoint);
        else
            lv_label_set_text_fmt(lbl_t_setpoint, "%.1f°C", toon_state.setpoint);
    } else {
        lv_label_set_text(lbl_t_setpoint, "");
    }

    /* Mirror the stock UI's setActiveProgramState() (Toon mobile.js, the HCB web
     * app — the authoritative reference). Two INDEPENDENT things:
     *   1. the active preset (activeState 0..3) is highlighted ALWAYS — even in
     *      Manual (stock adds 'ui-btn-sel' to activeState regardless of mode);
     *   2. the Manual/Program toggle reflects programState only: "program" for
     *      BASE/TEMPOVERRIDE/PROGOVERRIDE/LOCKEDBASE (1/2/3/8), "manual" for
     *      MANUAL/HOLIDAY (0/4/5). */
    int ps = toon_state.program_state;
    int on_program = (ps == 1 || ps == 2 || ps == 3 || ps == 8);

    if (tile_btn_mode_manual) {
        lv_obj_set_style_border_width(tile_btn_mode_manual, on_program ? 0 : 2, 0);
        lv_obj_set_style_bg_color(tile_btn_mode_manual, lv_color_hex(on_program ? 0x243242 : 0x6a5424), 0);
        lv_obj_t * ml = lv_obj_get_child(tile_btn_mode_manual, 0);
        if (ml) lv_obj_set_style_text_color(ml, lv_color_hex(on_program ? 0x88aabb : 0xffffff), 0);
    }
    if (tile_btn_mode_program) {
        lv_obj_set_style_border_width(tile_btn_mode_program, on_program ? 2 : 0, 0);
        lv_obj_set_style_bg_color(tile_btn_mode_program, lv_color_hex(on_program ? 0x2f6b6b : 0x243242), 0);
        if (lbl_t_program) {
            lv_label_set_text(lbl_t_program, tr("Programma", "Program"));
            lv_obj_set_style_text_color(lbl_t_program, lv_color_hex(on_program ? 0xffffff : 0x88aabb), 0);
        }
    }

    /* Preset row: selected preset in full colour with white border,
     * unselected presets in grey values. */
    {
        static const uint32_t preset_cols[4] = {0xcc7733, 0x3377cc, 0x553388, 0x557788};
        int hi = (toon_state.active_state >= 0 && toon_state.active_state <= 3)
                     ? toon_state.active_state : -1;
        for (int i = 0; i < 4; i++) {
            if (!tile_btn_preset[i]) continue;
            if (!on_program) {
                lv_obj_add_flag(tile_btn_preset[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_clear_flag(tile_btn_preset[i], LV_OBJ_FLAG_HIDDEN);
            int sel = (i == hi);
            lv_obj_set_style_border_width(tile_btn_preset[i], sel ? 2 : 0, 0);
            lv_obj_set_style_bg_color(tile_btn_preset[i],
                                      lv_color_hex(sel ? preset_cols[i] : 0x243242), 0);
            lv_obj_set_style_opa(tile_btn_preset[i], LV_OPA_COVER, 0);
            lv_obj_t * bl = lv_obj_get_child(tile_btn_preset[i], 0);
            if (bl) {
                lv_obj_set_style_text_color(bl, lv_color_hex(sel ? 0xffffff : 0x88aabb), 0);
            }
        }
    }

    if (lbl_t_burner) lv_label_set_text(lbl_t_burner, "");
    if (toon_state.burner_on) {
        if (tile_img_flame)  lv_obj_clear_flag(tile_img_flame,  LV_OBJ_FLAG_HIDDEN);
        if (tile_img_faucet) lv_obj_add_flag(tile_img_faucet, LV_OBJ_FLAG_HIDDEN);
        if (tile_img_drop)   lv_obj_add_flag(tile_img_drop,   LV_OBJ_FLAG_HIDDEN);
    } else if (toon_state.dhw_on) {
        if (tile_img_flame)  lv_obj_add_flag(tile_img_flame, LV_OBJ_FLAG_HIDDEN);
        if (tile_img_faucet) lv_obj_clear_flag(tile_img_faucet, LV_OBJ_FLAG_HIDDEN);
        if (tile_img_drop)   lv_obj_clear_flag(tile_img_drop,   LV_OBJ_FLAG_HIDDEN);
    } else {
        if (tile_img_flame)  lv_obj_add_flag(tile_img_flame,  LV_OBJ_FLAG_HIDDEN);
        if (tile_img_faucet) lv_obj_add_flag(tile_img_faucet, LV_OBJ_FLAG_HIDDEN);
        if (tile_img_drop)   lv_obj_add_flag(tile_img_drop,   LV_OBJ_FLAG_HIDDEN);
    }

    if (lbl_t_pressure) {
        if (toon_state.water_pressure > 0.1f)
            lv_label_set_text_fmt(lbl_t_pressure, "%.1f bar",
                                  toon_state.water_pressure);
        else
            lv_label_set_text(lbl_t_pressure, "-- bar");
    }
    /* Low-pressure banner. Thresholds: <0.6 = critical (red), <0.8 =
       warning (amber). >=0.8 or unknown (0) → hidden. */
    if (pressure_banner && pressure_banner_lbl) {
        float p = toon_state.water_pressure;
        if (p > 0.1f && p < 0.6f) {
            lv_obj_set_style_bg_color(pressure_banner,
                                      lv_color_hex(0xff3344), 0);
            lv_obj_set_style_bg_opa(pressure_banner, LV_OPA_COVER, 0);
            lv_label_set_text_fmt(pressure_banner_lbl,
                                  tr("CV-waterdruk KRITIEK: %.1f bar",
                                     "CH water pressure CRITICAL: %.1f bar"), p);
            lv_obj_clear_flag(pressure_banner, LV_OBJ_FLAG_HIDDEN);
        } else if (p > 0.1f && p < 0.8f) {
            lv_obj_set_style_bg_color(pressure_banner,
                                      lv_color_hex(0xffcc44), 0);
            lv_obj_set_style_bg_opa(pressure_banner, LV_OPA_COVER, 0);
            lv_label_set_text_fmt(pressure_banner_lbl,
                                  tr("CV-waterdruk laag: %.1f bar",
                                     "CH water pressure low: %.1f bar"), p);
            lv_obj_clear_flag(pressure_banner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pressure_banner, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (lbl_t_humidity && toon_state.humidity > 0)
        lv_label_set_text_fmt(lbl_t_humidity, tr("RV %.0f%%", "RH %.0f%%"),
                              toon_state.humidity);
    if (lbl_t_ppm && toon_state.eco2)
        lv_label_set_text_fmt(lbl_t_ppm, "%d ppm", toon_state.eco2);
    if (lbl_t_tvoc && toon_state.tvoc)
        lv_label_set_text_fmt(lbl_t_tvoc, tr("TVOC %d", "TVOC %d"), toon_state.tvoc);
    if (lbl_t_aq) {
        const char * aql = air_quality_label(toon_state.eco2, toon_state.tvoc);
        if (*aql) {
            lv_label_set_text_fmt(lbl_t_aq, tr("Lucht: %s", "Air: %s"), aql);
            lv_obj_set_style_text_color(lbl_t_aq,
                lv_color_hex(air_quality_color(toon_state.eco2, toon_state.tvoc)),
                0);
        } else {
            lv_label_set_text(lbl_t_aq, "");
        }
    }

    /* (Air quality tile removed — those readings now live on the
       Heater bottom strip.) */

    /* Waste tile (top + next-up rows). Each row gets the icon and accent
       colour for its type label. */
    if (lbl_waste_date && lbl_waste_type) {
        /* Windowed + per-type cutoff, same source as the dim screen: only show
           pickups within the lead window (default 3 days, up to 7), so when
           only Plastic falls inside the window only Plastic shows. */
        /* Home tile shows the actual NEXT pickup(s), not limited to a window —
           the lead-day window is a dim-screen-only concept (show_dim_waste). */
        waste_pickup_t pickups[3] = {0};
        int n = waste_state.connected ? waste_next_n_windowed(365, pickups, 3) : 0;
        if (n == 0 && waste_state.connected) {
            waste_pickup_t p1 = {0}, p2 = {0};
            int n2 = waste_next_2_pickups(&p1, &p2);
            if (n2 >= 1) { pickups[0] = p1; n = 1; }
            if (n2 >= 2) { pickups[1] = p2; n = 2; }
        }
        waste_pickup_t p1 = pickups[0];
        waste_pickup_t p2 = pickups[1];
        waste_pickup_t p3 = pickups[2];

        lv_coord_t th = tile_waste ? lv_obj_get_height(tile_waste) : 0;
        int show_3 = (n >= 3 && lbl_waste_date_3 && th >= 180);

        if (show_3) {
            /* 3 rows evenly spaced: usable area y=36 to th-8 */
            int usable = th - 36 - 8;
            int r0_y = 36;
            int r1_y = 36 + usable / 3;
            int r2_y = 36 + (2 * usable) / 3;

            /* Row 1 */
            if (waste_icon_1) {
                lv_obj_align(waste_icon_1, LV_ALIGN_TOP_LEFT, 6, r0_y + 2);
                lv_img_set_src(waste_icon_1, waste_icon_for_label(p1.labels));
                lv_obj_set_style_img_recolor(waste_icon_1, lv_color_hex(waste_accent_for_label(p1.labels)), 0);
                lv_obj_clear_flag(waste_icon_1, LV_OBJ_FLAG_HIDDEN);
            }
            if (lbl_waste_date) {
                int mo = atoi(p1.date + 5), d = atoi(p1.date + 8);
                lv_label_set_text_fmt(lbl_waste_date, "%d-%d", d, mo);
                lv_obj_set_style_text_color(lbl_waste_date, lv_color_hex(COL_TEXT_HI), 0);
                lv_obj_set_style_text_font(lbl_waste_date, SF(18), 0);
                lv_obj_align(lbl_waste_date, LV_ALIGN_TOP_LEFT, 54, r0_y);
            }
            if (lbl_waste_type) {
                lv_label_set_text(lbl_waste_type, p1.labels);
                lv_obj_set_style_text_color(lbl_waste_type, lv_color_hex(waste_accent_for_label(p1.labels)), 0);
                fit_font(lbl_waste_type, p1.labels, lv_obj_get_content_width(tile_waste) - 58, 13);
                lv_obj_align(lbl_waste_type, LV_ALIGN_TOP_LEFT, 54, r0_y + 22);
            }

            /* Row 2 */
            if (waste_icon_2) {
                lv_obj_align(waste_icon_2, LV_ALIGN_TOP_LEFT, 6, r1_y + 2);
                lv_img_set_src(waste_icon_2, waste_icon_for_label(p2.labels));
                lv_obj_set_style_img_recolor(waste_icon_2, lv_color_hex(waste_accent_for_label(p2.labels)), 0);
                lv_obj_clear_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
            }
            if (lbl_waste_date_2) {
                int mo = atoi(p2.date + 5), d = atoi(p2.date + 8);
                lv_label_set_text_fmt(lbl_waste_date_2, "%d-%d", d, mo);
                lv_obj_set_style_text_color(lbl_waste_date_2, lv_color_hex(COL_TEXT_HI), 0);
                lv_obj_set_style_text_font(lbl_waste_date_2, SF(18), 0);
                lv_obj_align(lbl_waste_date_2, LV_ALIGN_TOP_LEFT, 54, r1_y);
                lv_obj_clear_flag(lbl_waste_date_2, LV_OBJ_FLAG_HIDDEN);
            }
            if (lbl_waste_type_2) {
                lv_label_set_text(lbl_waste_type_2, p2.labels);
                lv_obj_set_style_text_color(lbl_waste_type_2, lv_color_hex(waste_accent_for_label(p2.labels)), 0);
                fit_font(lbl_waste_type_2, p2.labels, lv_obj_get_content_width(tile_waste) - 58, 13);
                lv_obj_align(lbl_waste_type_2, LV_ALIGN_TOP_LEFT, 54, r1_y + 22);
                lv_obj_clear_flag(lbl_waste_type_2, LV_OBJ_FLAG_HIDDEN);
            }

            /* Row 3 */
            if (waste_icon_3) {
                lv_obj_align(waste_icon_3, LV_ALIGN_TOP_LEFT, 6, r2_y + 2);
                lv_img_set_src(waste_icon_3, waste_icon_for_label(p3.labels));
                lv_obj_set_style_img_recolor(waste_icon_3, lv_color_hex(waste_accent_for_label(p3.labels)), 0);
                lv_obj_clear_flag(waste_icon_3, LV_OBJ_FLAG_HIDDEN);
            }
            if (lbl_waste_date_3) {
                int mo = atoi(p3.date + 5), d = atoi(p3.date + 8);
                lv_label_set_text_fmt(lbl_waste_date_3, "%d-%d", d, mo);
                lv_obj_set_style_text_color(lbl_waste_date_3, lv_color_hex(COL_TEXT_HI), 0);
                lv_obj_set_style_text_font(lbl_waste_date_3, SF(18), 0);
                lv_obj_align(lbl_waste_date_3, LV_ALIGN_TOP_LEFT, 54, r2_y);
                lv_obj_clear_flag(lbl_waste_date_3, LV_OBJ_FLAG_HIDDEN);
            }
            if (lbl_waste_type_3) {
                lv_label_set_text(lbl_waste_type_3, p3.labels);
                lv_obj_set_style_text_color(lbl_waste_type_3, lv_color_hex(waste_accent_for_label(p3.labels)), 0);
                fit_font(lbl_waste_type_3, p3.labels, lv_obj_get_content_width(tile_waste) - 58, 13);
                lv_obj_align(lbl_waste_type_3, LV_ALIGN_TOP_LEFT, 54, r2_y + 22);
                lv_obj_clear_flag(lbl_waste_type_3, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            /* 1 or 2 rows */
            if (n >= 1) {
                int mo = atoi(p1.date + 5), d = atoi(p1.date + 8);
                lv_label_set_text_fmt(lbl_waste_date, "%d-%d", d, mo);
                lv_label_set_text(lbl_waste_type, p1.labels);
                lv_obj_set_style_text_color(lbl_waste_date, lv_color_hex(COL_TEXT_HI), 0);
                lv_obj_set_style_text_font(lbl_waste_date, (n >= 2) ? SF(24) : SF(28), 0);
#ifdef TOON1
                lv_obj_align(lbl_waste_date, LV_ALIGN_TOP_LEFT, 4, 38);
                lv_obj_align(lbl_waste_type, LV_ALIGN_TOP_LEFT, 4, (n >= 2) ? 68 : 74);
#else
                lv_obj_align(lbl_waste_date, LV_ALIGN_TOP_LEFT, 56, 42);
                lv_obj_align(lbl_waste_type, LV_ALIGN_TOP_LEFT, 56, (n >= 2) ? 70 : 78);
#endif
                fit_font(lbl_waste_type, p1.labels, lv_obj_get_content_width(tile_waste) - 60, (n >= 2) ? 15 : 18);
                if (waste_icon_1) {
                    lv_obj_align(waste_icon_1, LV_ALIGN_TOP_LEFT, 6, 42);
                    lv_img_set_src(waste_icon_1, waste_icon_for_label(p1.labels));
                    lv_obj_set_style_img_recolor(waste_icon_1, lv_color_hex(waste_accent_for_label(p1.labels)), 0);
                    lv_obj_clear_flag(waste_icon_1, LV_OBJ_FLAG_HIDDEN);
                }
#ifdef TOON1
                if (lbl_waste_type)
                    lv_obj_set_style_text_color(lbl_waste_type, lv_color_hex(waste_accent_for_label(p1.labels)), 0);
#endif
            } else {
                lv_label_set_text(lbl_waste_date, waste_state.connected ? "--" : "...");
                lv_label_set_text(lbl_waste_type, waste_state.connected ? tr("geen", "none") : "");
            }

            if (n >= 2 && lbl_waste_date_2 && lbl_waste_type_2) {
                int mo = atoi(p2.date + 5), d = atoi(p2.date + 8);
                lv_label_set_text_fmt(lbl_waste_date_2, "%d-%d", d, mo);
                lv_label_set_text(lbl_waste_type_2, p2.labels);
                lv_obj_set_style_text_color(lbl_waste_date_2, lv_color_hex(COL_TEXT_DIM), 0);
                lv_obj_set_style_text_font(lbl_waste_date_2, SF(20), 0);
#ifdef TOON1
                lv_obj_align(lbl_waste_date_2, LV_ALIGN_BOTTOM_LEFT, 4, -28);
                lv_obj_align(lbl_waste_type_2, LV_ALIGN_BOTTOM_LEFT, 4, -8);
#else
                lv_obj_align(lbl_waste_date_2, LV_ALIGN_BOTTOM_LEFT, 56, -30);
                lv_obj_align(lbl_waste_type_2, LV_ALIGN_BOTTOM_LEFT, 56, -8);
#endif
                fit_font(lbl_waste_type_2, p2.labels, lv_obj_get_content_width(tile_waste) - 60, 14);
                if (waste_icon_2) {
                    lv_obj_align(waste_icon_2, LV_ALIGN_BOTTOM_LEFT, 6, -36);
                    lv_img_set_src(waste_icon_2, waste_icon_for_label(p2.labels));
                    lv_obj_set_style_img_recolor(waste_icon_2, lv_color_hex(waste_accent_for_label(p2.labels)), 0);
                    lv_obj_clear_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
                }
#ifdef TOON1
                if (lbl_waste_type_2)
                    lv_obj_set_style_text_color(lbl_waste_type_2, lv_color_hex(waste_accent_for_label(p2.labels)), 0);
#endif
                lv_obj_clear_flag(lbl_waste_date_2, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(lbl_waste_type_2, LV_OBJ_FLAG_HIDDEN);
            } else {
                if (waste_icon_2) lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
                if (lbl_waste_date_2) lv_label_set_text(lbl_waste_date_2, "");
                if (lbl_waste_type_2) lv_label_set_text(lbl_waste_type_2, "");
            }

            /* Row 3 hidden */
            if (waste_icon_3) lv_obj_add_flag(waste_icon_3, LV_OBJ_FLAG_HIDDEN);
            if (lbl_waste_date_3) lv_obj_add_flag(lbl_waste_date_3, LV_OBJ_FLAG_HIDDEN);
            if (lbl_waste_type_3) lv_obj_add_flag(lbl_waste_type_3, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Humidity tile is gone — humidity now lives on the Heater bottom strip.
       Keep the guarded write for any legacy reference. */
    if (toon_state.humidity > 0 && lbl_humid_val)
        lv_label_set_text_fmt(lbl_humid_val, "%.0f %%", toon_state.humidity);

    /* Energy tile — live power + cumulative gas. "Today" stays blank until
       we track a midnight-baseline; intent is to show kWh-since-midnight
       once that book-keeping lands. Skip when a marketplace integration
       has taken over this slot. */
    if (!slot_active[TILE_SLOT_ENERGY]) {
        if (lbl_energy_w) {
            if (energy_connected())
                lv_label_set_text_fmt(lbl_energy_w, "%.0f W", energy_power_w());
            else
                lv_label_set_text(lbl_energy_w, energy_offline_label());
        }
        if (lbl_energy_gas) {
            lv_label_set_text(lbl_energy_gas, "");
            lv_obj_add_flag(lbl_energy_gas, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* NILM transient device label — visible for 10 s after a step-change event */
    if (lbl_nilm_event) {
        if (meter_state.nilm_event_ts &&
            time(NULL) - (time_t)meter_state.nilm_event_ts < 10) {
            lv_label_set_text(lbl_nilm_event, meter_state.nilm_device);
            lv_obj_clear_flag(lbl_nilm_event, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_nilm_event, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Vent tile. Top-right line combines preset + remaining ("Auto", "High",
       "Timer 25m"); bottom line combines % + rpm + source ("100 % · 695 rpm
       · vremote"). All four physical signals (preset/pct/rpm/who) on two
       label widgets so the tile stays uncluttered. Skip the whole block
       when a marketplace integration has taken over the slot — the labels
       were already rewritten by render_tile_slot. */
    if (!slot_active[TILE_SLOT_VENT] && lbl_boiler_state) {
        if (!settings.enable_vent && (!settings.client_mode || !vent_state.connected)) {
            /* Integration turned off in Settings → keep the tile visible
             * (unless hide_offline_tiles is on, in which case the whole
             * tile is already hidden by apply_offline_tile_visibility) but
             * make it obvious why no data is flowing. */
            const char * itho_off = tr("Itho offline", "Itho offline");
            lv_label_set_text(lbl_boiler_state, itho_off);
            if (tile_vent)
                fit_font(lbl_boiler_state, itho_off,
                         lv_obj_get_content_width(tile_vent) - SX(64), 18);
            if (vent_btn_low)   lv_obj_set_style_border_width(vent_btn_low,   0, 0);
            if (vent_btn_high)  lv_obj_set_style_border_width(vent_btn_high,  0, 0);
            if (vent_btn_auto)  lv_obj_set_style_border_width(vent_btn_auto,  0, 0);
            if (vent_btn_timer) lv_obj_set_style_border_width(vent_btn_timer, 0, 0);
        } else if (vent_state.connected && vent_state.itho_online == 0) {
            /* Itho bridge reported itself OFFLINE via its MQTT LWT
             * (topic itho/lwt). The broker link is up but the add-on/unit
             * is gone — surface it here on the home/main tile. The dim
             * screen deliberately shows only the mode, not online/offline. */
            const char * itho_off = tr("Itho offline", "Itho offline");
            lv_label_set_text(lbl_boiler_state, itho_off);
            if (tile_vent)
                fit_font(lbl_boiler_state, itho_off,
                         lv_obj_get_content_width(tile_vent) - SX(64), 18);
            if (vent_btn_low)   lv_obj_set_style_border_width(vent_btn_low,   0, 0);
            if (vent_btn_high)  lv_obj_set_style_border_width(vent_btn_high,  0, 0);
            if (vent_btn_auto)  lv_obj_set_style_border_width(vent_btn_auto,  0, 0);
            if (vent_btn_timer) lv_obj_set_style_border_width(vent_btn_timer, 0, 0);
        } else if (vent_state.connected) {
            /* Same snapshot trick as last_source — fan_info is a non-
             * volatile char[] written by another thread; memcpy a local
             * copy each refresh to dodge stale-cache reads. */
            char fi_local[16];
            memcpy(fi_local, (const char *)vent_state.fan_info,
                   sizeof(fi_local));
            fi_local[sizeof(fi_local) - 1] = 0;
            /* Current Itho firmware no longer publishes FanInfo, so fall
             * back to last_cmd (itho/lastcmd MQTT) for the live mode. Both
             * are already swap-corrected to the user-intent label. */
            if (!fi_local[0]) {
                memcpy(fi_local, (const char *)vent_state.last_cmd,
                       sizeof(fi_local));
                fi_local[sizeof(fi_local) - 1] = 0;
            }
            /* Title is the clean mode word ("Low"/"High"/"Auto"/"Manual"/
             * "Timer 20m"); vent_mode_label() maps the raw "speed:N" the
             * slider produces to "Manual". The source ("via API"/"via TOON")
             * stays on the lower line only. fi_local is still used below for
             * the per-button highlight. */
            const char * preset_pretty = vent_mode_label();
            char vshown[32];
            if (vent_state.remaining_min > 0)
                snprintf(vshown, sizeof vshown, "%s %dm",
                         preset_pretty, vent_state.remaining_min);
            else
                snprintf(vshown, sizeof vshown, "%s", preset_pretty);
            lv_label_set_text(lbl_boiler_state, vshown);
            /* Shrink the font (no truncation) so the mode label never collides
             * with the "Vent" title in a narrow custom tile. */
            if (tile_vent)
                fit_font(lbl_boiler_state, vshown,
                         lv_obj_get_content_width(tile_vent) - SX(64), 18);

            /* Highlight whichever preset button matches the current fan_info
             * (Timer wins when a countdown is running). White border = active,
             * 0 = idle. ventilation.c already translates wire→user-intent. */
            int act_low   = strcmp(fi_local, "low")    == 0;
            int act_high  = strcmp(fi_local, "high")   == 0;
            int act_auto  = strcmp(fi_local, "auto")   == 0
                         || strcmp(fi_local, "medium") == 0;
            int act_timer = strncasecmp(fi_local, "timer", 5) == 0
                         || vent_state.remaining_min > 0;
            if (act_timer) { act_low = act_high = act_auto = 0; }
            if (vent_btn_low)
                lv_obj_set_style_border_width(vent_btn_low,   act_low   ? 2 : 0, 0);
            if (vent_btn_high)
                lv_obj_set_style_border_width(vent_btn_high,  act_high  ? 2 : 0, 0);
            if (vent_btn_auto)
                lv_obj_set_style_border_width(vent_btn_auto,  act_auto  ? 2 : 0, 0);
            if (vent_btn_timer)
                lv_obj_set_style_border_width(vent_btn_timer, act_timer ? 2 : 0, 0);
        } else {
            lv_label_set_text(lbl_boiler_state, tr("uit", "off"));
            if (vent_btn_low)   lv_obj_set_style_border_width(vent_btn_low,   0, 0);
            if (vent_btn_high)  lv_obj_set_style_border_width(vent_btn_high,  0, 0);
            if (vent_btn_auto)  lv_obj_set_style_border_width(vent_btn_auto,  0, 0);
            if (vent_btn_timer) lv_obj_set_style_border_width(vent_btn_timer, 0, 0);
        }
    }
    if (!slot_active[TILE_SLOT_VENT] && lbl_boiler_pressure) {
        if (vent_state.connected && vent_state.itho_online != 0) {
            /* Classify source into a 3-char tag so the line stays compact:
               API = HTML/vremote (toonui or HA), RF  = physical radio remote,
               BTN = Itho front-panel button, ???  = anything unrecognised. */
            /* Snapshot vent_state.last_source into a local before strstr —
             * the buffer is written by vent_thread without a memory
             * barrier, so the compiler/CPU can hand refresh_cb a stale
             * (all-zero) view of the chars. memcpy forces a fresh read. */
            char src_local[64];
            memcpy(src_local, (const char *)vent_state.last_source,
                   sizeof(src_local));
            src_local[sizeof(src_local) - 1] = 0;
            const char * src_tag = "?";
            const char * s = src_local;
            int api_src = (strstr(s, "vremote") || strstr(s, "HTML")) ? 1 : 0;
            if      (api_src)                                   src_tag = "API";
            else if (strstr(s, "RFT")     || strstr(s, "RF"))   src_tag = "RF";
            else if (strstr(s, "button")  || strstr(s, "Itho")) src_tag = "BTN";
            else if (s[0])                                      src_tag = s;
            /* If the source looks like a vremote/HTML call AND we issued one
             * locally within the last 90 s, attribute it to TOON. Itho can't
             * distinguish callers — this is our only signal. */
            if (api_src && vent_local_press_ms != 0 &&
                (lv_tick_get() - vent_local_press_ms) < 90000) {
                src_tag = "TOON";
            }
            /* Show the HONEST actual airflow: vent_state.speed_pct is derived
             * from the real fan rpm (ExhFanSpeed is broken/0, and the wire
             * setpoint lies — it reads 100% for the wire-"high" command which
             * on this unit is the LOW-airflow direction). */
            lv_label_set_text_fmt(lbl_boiler_pressure,
                                  tr("%d%%  %drpm\nvia %s", "%d%%  %drpm\nvia %s"),
                                  vent_state.speed_pct,
                                  vent_state.fan_rpm,
                                  src_tag);
        } else {
            lv_label_set_text(lbl_boiler_pressure,
                              vent_state.itho_online == 0 ? tr("offline", "offline") : "-- %");
        }
    }
    /* Spin tracks the real fan rpm. With the wire swap, user-Low maps to the
       low-airflow direction (~700 rpm → slow) and user-High to high (~2500 rpm
       → fast), so the icon spins intuitively without faking it. */
    if (vent_state.connected)
        vent_apply_fan_anim(vent_state.fan_rpm);

    /* Pinned device quick-tiles — live state from ha_devices[]. */
    for (int p = 0; p < home_pin_count; p++) {
        home_pin_t * P = &home_pins[p];
        if (P->dev_idx < 0 || P->dev_idx >= ha_device_count || !P->lbl) continue;
        ha_device_t * D = &ha_devices[P->dev_idx];
        if (!ha_state.connected) {
            lv_label_set_text_fmt(P->lbl, tr("%s  (HA offline)", "%s  (HA offline)"), D->name);
            continue;
        }
        if (D->type == HADEV_COVER) {
            const char * s = D->state[0] ? D->state : "?";
            char pretty[24];
            snprintf(pretty, sizeof pretty, "%c%s",
                     (s[0] >= 'a' && s[0] <= 'z') ? s[0] - 'a' + 'A' : s[0], s + 1);
            lv_label_set_text_fmt(P->lbl, "%s  %s %d%%", D->name, pretty,
                                  D->position >= 0 ? D->position : 0);
            if (P->slider && D->position >= 0)
                lv_slider_set_value(P->slider, D->position, LV_ANIM_ON);
        } else if (D->type == HADEV_LIGHT || D->type == HADEV_SWITCH) {
            const char * st = !D->available ? tr("offline", "offline")
                                            : (D->on ? tr("aan", "on") : tr("uit", "off"));
            if (D->type == HADEV_LIGHT && D->available && D->on && D->brightness > 0)
                lv_label_set_text_fmt(P->lbl, tr("%s  aan %d%%", "%s  on %d%%"), D->name, D->brightness * 100 / 255);
            else
                lv_label_set_text_fmt(P->lbl, "%s  %s", D->name, st);
            if (P->btn_lbl) {
                lv_label_set_text(P->btn_lbl, D->on ? tr("Aan", "On") : tr("Uit", "Off"));
                lv_obj_set_style_text_color(P->btn_lbl, lv_color_hex(settings.home_light_mode && !D->on ? 0x1e2838 : 0xffffff), 0);
            }
            if (P->btn) lv_obj_set_style_bg_color(P->btn,
                            lv_color_hex(D->on ? (settings.home_light_mode ? 0xd97706 : 0xffcc44)
                                               : (settings.home_light_mode ? 0xd0d8e2 : 0x3a4658)), 0);
            if (P->slider && D->type == HADEV_LIGHT) {
                if (D->on && D->brightness > 0)
                    lv_slider_set_value(P->slider, D->brightness * 100 / 255, LV_ANIM_OFF);
                else if (!D->on)
                    lv_slider_set_value(P->slider, 0, LV_ANIM_OFF);
            }
        }
        /* script/scene: static label + Run button, nothing to refresh */
    }


    /* Energy bottom tile: live power only (gas removed per CR10) */
    if (lbl_bot_energy) {
        if (energy_connected()) {
            lv_label_set_text_fmt(lbl_bot_energy, "%.0f W", energy_power_w());
        } else {
            lv_label_set_text(lbl_bot_energy, energy_offline_label());
        }
    }
    if (lbl_bot_weather && lv_label_get_text(lbl_bot_weather)
        && strcmp(lv_label_get_text(lbl_bot_weather), "...") == 0)
        lv_label_set_text(lbl_bot_weather, tr("(binnenkort)", "(soon)"));
    if (lbl_bot_waste) {
        if (waste_state.connected) {
            char date[16], labels[40];
            waste_next_pickup(date, sizeof(date), labels, sizeof(labels));
            if (date[0]) {
                /* "YYYY-MM-DD" → "DD-M" short. Keep label tight. */
                int y = atoi(date), mo = atoi(date+5), d = atoi(date+8);
                (void)y;
                lv_label_set_text_fmt(lbl_bot_waste, "%d-%d  %s", d, mo, labels);
            } else {
                lv_label_set_text(lbl_bot_waste, tr("geen", "none"));
            }
        } else {
            lv_label_set_text(lbl_bot_waste, "...");
        }
    }

    /* Water tile: only show values when water meter is connected (CR10) */
    if (!slot_active[TILE_SLOT_WATER]) {
        if ((!settings.enable_p1_water && !settings.client_mode) || !hw_state.connected_water) {
            if (lbl_inbox_main) lv_label_set_text(lbl_inbox_main, "--");
            if (lbl_inbox_sub)  lv_label_set_text(lbl_inbox_sub, "");
        } else {
            if (lbl_inbox_main)
                lv_label_set_text_fmt(lbl_inbox_main, "%.3f m3", hw_state.water_total_m3);
            if (lbl_inbox_sub) {
                if (hw_state.water_lpm > 0.05f)
                    lv_label_set_text_fmt(lbl_inbox_sub, "%.1f L/min  +%.1f L",
                                          hw_state.water_lpm,
                                          hw_state.water_session_l);
                else if (hw_state.water_session_l > 0)
                    lv_label_set_text_fmt(lbl_inbox_sub, tr("+%.1f L zojuist getapt", "+%.1f L just poured"),
                                          hw_state.water_session_l);
                else
                    lv_label_set_text(lbl_inbox_sub, "0.0 L/min");
            }
        }
    }
    /* Drop + L/m indicator on the big heater tile — visible whenever a tap
     * is open so the user sees the flow at a glance without checking the
     * (smaller) Water tile. Stays up briefly after the tap closes to show
     * the just-poured total. */
    if (tile_img_water && tile_lbl_water) {
        if (hw_state.connected_water && hw_state.water_lpm > 0.05f) {
            lv_label_set_text_fmt(tile_lbl_water, "%.1f L/m",
                                  hw_state.water_lpm);
            lv_obj_clear_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);
        } else if (hw_state.connected_water && hw_state.water_session_l > 0) {
            lv_label_set_text_fmt(tile_lbl_water, "+%.1f L",
                                  hw_state.water_session_l);
            lv_obj_clear_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* (water_spinner visibility decided ONCE at the bottom of refresh_cb
     * with the proper connected_water + >0.05 LPM gate. This earlier copy
     * was a splat-recovery duplicate with a too-loose 0.01 threshold and
     * no connected check, so meter wobble or stale data could leave the
     * spinner running indefinitely. Dropped.) */

    /* Outside tile — current temp + short description. */
    if (lbl_outside_main) {
        if (weather_state.connected)
            lv_label_set_text_fmt(lbl_outside_main, "%.1f°C", weather_display_temp());
        else
            lv_label_set_text(lbl_outside_main, "-- C");
    }

    /* Forecast band: honour settings.forecast_mode (auto / forced hourly /
       forced daily). Forced-hourly still falls back to daily if the hourly
       fetch has nothing yet. */
    int show_hourly = settings.forecast_mode != FORECAST_DAILY
                      && weather_state.hour_count > 0;
    if (show_hourly) {
        /* The "Medemblik - X.X C now" header above the strip already shows
         * the current hour's value, so start at index 1 (the next 3-hour
         * slot) to avoid duplicating it in the first column. */
        for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
            int si = i + 1;     /* skip slot 0 — that's "now" */
            if (si < weather_state.hour_count) {
                const weather_hour_t * h = &weather_state.hours[si];
                lv_label_set_text(fc_day_lbl[i], h->label);
                lv_label_set_text_fmt(fc_temp_lbl[i], "%.0f\xc2\xb0",
                                      weather_hour_display_temp(h));
                set_forecast_icon(fc_icon[i], fc_icon_sun[i], h->icon);
                if (h->wind_dir[0]) {
                    lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d",
                                          h->wind_dir, h->wind_bft);
                    int ang = wind_dir_angle(h->wind_dir);
                    if (fc_wind_arrow[i] && ang >= 0) {
                        lv_img_set_angle(fc_wind_arrow[i], ang);
                        lv_obj_clear_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                    } else if (fc_wind_arrow[i]) {
                        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                    }
                } else {
                    lv_label_set_text(fc_wind_lbl[i], "");
                    if (fc_wind_arrow[i])
                        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                /* Past the hourly horizon — fall back to the daily forecast
                 * so the last column(s) still carry signal. Index into days[]
                 * starting at 0 (tomorrow / first daily entry available). */
                int di = si - weather_state.hour_count;
                if (di >= weather_state.day_count) {
                    lv_label_set_text(fc_day_lbl[i], "");
                    lv_label_set_text(fc_temp_lbl[i], "");
                    lv_label_set_text(fc_wind_lbl[i], "");
                    if (fc_wind_arrow[i])
                        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                    continue;
                }
                const weather_day_t * d = &weather_state.days[di];
                lv_label_set_text(fc_day_lbl[i], d->day);
                lv_label_set_text_fmt(fc_temp_lbl[i],
                                      "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                      d->max_temp, d->min_temp);
                set_forecast_icon(fc_icon[i], fc_icon_sun[i], d->icon);
                if (d->wind_dir[0]) {
                    lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d",
                                          d->wind_dir, d->wind_bft);
                    int ang = wind_dir_angle(d->wind_dir);
                    if (fc_wind_arrow[i] && ang >= 0) {
                        lv_img_set_angle(fc_wind_arrow[i], ang);
                        lv_obj_clear_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                    } else if (fc_wind_arrow[i]) {
                        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                    }
                } else {
                    lv_label_set_text(fc_wind_lbl[i], "");
                    if (fc_wind_arrow[i])
                        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    } else {
        for (int i = 0; i < weather_state.day_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_day_t * d = &weather_state.days[i];
            lv_label_set_text(fc_day_lbl[i], d->day);
            lv_label_set_text_fmt(fc_temp_lbl[i],
                                  "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                  d->max_temp, d->min_temp);
            set_forecast_icon(fc_icon[i], fc_icon_sun[i], d->icon);
            if (d->wind_dir[0]) {
                lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d",
                                      d->wind_dir, d->wind_bft);
                int ang = wind_dir_angle(d->wind_dir);
                if (fc_wind_arrow[i] && ang >= 0) {
                    lv_img_set_angle(fc_wind_arrow[i], ang);
                    lv_obj_clear_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                lv_label_set_text(fc_wind_lbl[i], "");
                if (fc_wind_arrow[i])
                    lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    /* Show the spinner only while water is actually flowing. */
    if (water_spinner) {
        if (hw_state.connected_water && hw_state.water_lpm > 0.05f)
            lv_obj_clear_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);
    }

    /* Outside tile — current temp + short description on second line. */
    if (lbl_outside_main && weather_state.connected) {
        lv_label_set_text_fmt(lbl_outside_main, "%.1f°C",
                              weather_display_temp());
    }
    /* "Medemblik - 14.7 C now" header above the forecast strip. Life360
     * moved to its own tile so this stays purely weather. */
    if (lbl_forecast_city) {
        const char * city = settings.weather_location[0] ? settings.weather_location : tr("Weer", "Forecast");
        if (weather_state.connected)
            lv_label_set_text_fmt(lbl_forecast_city, tr("%s  -  %.1f°C nu", "%s  -  %.1f°C now"),
                                  city, weather_display_temp());
        else
            lv_label_set_text(lbl_forecast_city, city);
    }

    /* Family tile (Life360). Name prefix + scrolling address. When HA
     * integration is off there's no poller to populate ha_state, so
     * surface that explicitly instead of leaving a pair of stale "?"s
     * that could be mistaken for "in transit" or "out of range".
     * Skip when a marketplace integration owns this slot. */
    if (!slot_active[TILE_SLOT_FAMILY]) {
        int ha_up = settings.enable_ha || (settings.client_mode && (ha_state.connected || ha_state.loc_a[0]));
        if (lbl_life360_a) {
            if (!ha_up)
                lv_label_set_text(lbl_life360_a, tr("HA offline", "HA offline"));
            else
                lv_label_set_text_fmt(lbl_life360_a, "%s: %s",
                    settings.life360_a_name[0] ? settings.life360_a_name : "A",
                    ha_state.loc_a[0] ? ha_state.loc_a : "?");
        }
        if (lbl_life360_b) {
            if (!ha_up)
                lv_label_set_text(lbl_life360_b, "");
            else
                lv_label_set_text_fmt(lbl_life360_b, "%s: %s",
                    settings.life360_b_name[0] ? settings.life360_b_name : "B",
                    ha_state.loc_b[0]   ? ha_state.loc_b   : "?");
        }
        if (lbl_life360_c) {
            if (!ha_up || !settings.life360_c_entity[0])
                lv_label_set_text(lbl_life360_c, "");
            else
                lv_label_set_text_fmt(lbl_life360_c, "%s: %s",
                    settings.life360_c_name[0] ? settings.life360_c_name : "C",
                    ha_state.loc_c[0]   ? ha_state.loc_c   : "?");
        }
    }

    /* Forecast band — splat-recovery left two more copies of this
       writer further down; gate them on the same hourly/daily decision so
       the data the first writer painted isn't immediately overwritten. */
    if (!show_hourly) {
        for (int i = 0; i < weather_state.day_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_day_t * d = &weather_state.days[i];
            if (fc_day_lbl[i])
                lv_label_set_text(fc_day_lbl[i], d->day);
            if (fc_temp_lbl[i])
                lv_label_set_text_fmt(fc_temp_lbl[i],
                                      "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                      d->max_temp, d->min_temp);
            if (fc_desc_lbl[i])
                lv_label_set_text(fc_desc_lbl[i], d->desc);
        }
    }

    /* Energy bottom tile: live power + cumulative gas. */
    /* Energy bottom tile: live power only (gas removed per CR10) */
    if (lbl_bot_energy) {
        if (energy_connected()) {
            lv_label_set_text_fmt(lbl_bot_energy, "%.0f W", energy_power_w());
        } else {
            lv_label_set_text(lbl_bot_energy, energy_offline_label());
        }
    }
    if (lbl_bot_weather && lv_label_get_text(lbl_bot_weather)
        && strcmp(lv_label_get_text(lbl_bot_weather), "...") == 0)
        lv_label_set_text(lbl_bot_weather, tr("(binnenkort)", "(soon)"));
    if (lbl_bot_waste) {
        if (waste_state.connected) {
            char date[16], labels[40];
            waste_next_pickup(date, sizeof(date), labels, sizeof(labels));
            if (date[0]) {
                /* "YYYY-MM-DD" → "DD-M" short. Keep label tight. */
                int y = atoi(date), mo = atoi(date+5), d = atoi(date+8);
                (void)y;
                lv_label_set_text_fmt(lbl_bot_waste, "%d-%d  %s", d, mo, labels);
            } else {
                lv_label_set_text(lbl_bot_waste, tr("geen", "none"));
            }
        } else {
            lv_label_set_text(lbl_bot_waste, "...");
        }
    }

    /* Water tile: only show values when water meter is connected (CR10) */
    if (!slot_active[TILE_SLOT_WATER]) {
        if ((!settings.enable_p1_water && !settings.client_mode) || !hw_state.connected_water) {
            if (lbl_inbox_main) lv_label_set_text(lbl_inbox_main, "--");
            if (lbl_inbox_sub)  lv_label_set_text(lbl_inbox_sub, "");
        } else {
            if (lbl_inbox_main)
                lv_label_set_text_fmt(lbl_inbox_main, "%.3f m3", hw_state.water_total_m3);
            if (lbl_inbox_sub) {
                if (hw_state.water_lpm > 0.05f)
                    lv_label_set_text_fmt(lbl_inbox_sub, "%.1f L/min  +%.1f L",
                                          hw_state.water_lpm,
                                          hw_state.water_session_l);
                else if (hw_state.water_session_l > 0)
                    lv_label_set_text_fmt(lbl_inbox_sub, tr("+%.1f L zojuist getapt", "+%.1f L just poured"),
                                          hw_state.water_session_l);
                else
                    lv_label_set_text(lbl_inbox_sub, "0.0 L/min");
            }
        }
    }
    /* Drop + L/m indicator on the big heater tile — visible whenever a tap
     * is open so the user sees the flow at a glance without checking the
     * (smaller) Water tile. Stays up briefly after the tap closes to show
     * the just-poured total. */
    if (tile_img_water && tile_lbl_water) {
        if (hw_state.connected_water && hw_state.water_lpm > 0.05f) {
            lv_label_set_text_fmt(tile_lbl_water, "%.1f L/m",
                                  hw_state.water_lpm);
            lv_obj_clear_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);
        } else if (hw_state.connected_water && hw_state.water_session_l > 0) {
            lv_label_set_text_fmt(tile_lbl_water, "+%.1f L",
                                  hw_state.water_session_l);
            lv_obj_clear_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* (water_spinner visibility decided ONCE at the bottom of refresh_cb
     * with the proper connected_water + >0.05 LPM gate. This earlier copy
     * was a splat-recovery duplicate with a too-loose 0.01 threshold and
     * no connected check, so meter wobble or stale data could leave the
     * spinner running indefinitely. Dropped.) */

    /* Outside tile — current temp + short description. */
    if (lbl_outside_main) {
        if (weather_state.connected)
            lv_label_set_text_fmt(lbl_outside_main, "%.1f°C", weather_display_temp());
        else
            lv_label_set_text(lbl_outside_main, "-- C");
    }

    /* Forecast band — third splat-recovery copy of the daily writer;
       same gating as the earlier two. */
    if (!show_hourly) {
        for (int i = 0; i < weather_state.day_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_day_t * d = &weather_state.days[i];
            lv_label_set_text(fc_day_lbl[i], d->day);
            lv_label_set_text_fmt(fc_temp_lbl[i],
                                  "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                  d->max_temp, d->min_temp);
            set_forecast_icon(fc_icon[i], fc_icon_sun[i], d->icon);
            if (d->wind_dir[0])
                lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d",
                                      d->wind_dir, d->wind_bft);
            else
                lv_label_set_text(fc_wind_lbl[i], "");
        }
    }

    /* Energy bottom tile: live power only (gas removed per CR10) */
    if (lbl_bot_energy) {
        if (energy_connected()) {
            lv_label_set_text_fmt(lbl_bot_energy, "%.0f W", energy_power_w());
        } else {
            lv_label_set_text(lbl_bot_energy, energy_offline_label());
        }
    }
    /* (Old splat-recovered "(soon)" override block removed — it was
       clobbering the real waste-pickup text written earlier in this
       refresh callback.) */

    /* Water tile: only show values when water meter is connected (CR10) */
    if (!slot_active[TILE_SLOT_WATER]) {
        if ((!settings.enable_p1_water && !settings.client_mode) || !hw_state.connected_water) {
            if (lbl_inbox_main) lv_label_set_text(lbl_inbox_main, "--");
            if (lbl_inbox_sub)  lv_label_set_text(lbl_inbox_sub, "");
        } else {
            if (lbl_inbox_main)
                lv_label_set_text_fmt(lbl_inbox_main, "%.3f m3", hw_state.water_total_m3);
            if (lbl_inbox_sub)
                lv_label_set_text_fmt(lbl_inbox_sub, "%.1f L/min", hw_state.water_lpm);
        }
    }

    /* Refresh lights handle logo dynamically if backend becomes active or changes */
    if (lights_handle) {
        const lv_img_dsc_t * want_logo = (settings.enable_domoticz || (settings.client_mode && domoticz_state.count > 0)) ? &icon_domoticz
                                       : (settings.enable_ha || (settings.client_mode && ha_light_count > 0)) ? &icon_ha
                                       : NULL;
        if (want_logo) {
            if (!lights_handle_img) {
                lights_handle_img = lv_img_create(lights_handle);
                lv_img_set_zoom(lights_handle_img, 224);   /* 48px -> ~42px */
                lv_obj_center(lights_handle_img);
            }
            if (lv_img_get_src(lights_handle_img) != want_logo) {
                lv_img_set_src(lights_handle_img, want_logo);
            }
            if (lv_obj_get_width(lights_handle) <= 60) {   /* collapsed state */
                lv_obj_clear_flag(lights_handle_img, LV_OBJ_FLAG_HIDDEN);
                if (lights_handle_lbl) lv_obj_add_flag(lights_handle_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (lights_handle_img) {
            lv_obj_add_flag(lights_handle_img, LV_OBJ_FLAG_HIDDEN);
            if (lights_handle_lbl && lv_obj_get_width(lights_handle) <= 60)
                lv_obj_clear_flag(lights_handle_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_invalidate(scr_root);
}

/* ---------- screen builder ---------- */
/* Push the Lights page. Used by both the swipe-right gesture and the
 * lightbulb button in the top-right. */
/* Route to whichever lights backend is enabled: Domoticz if configured,
 * otherwise the Home Assistant lights page. */
static void open_lights_backend(void) {
    if (settings.enable_domoticz || (settings.client_mode && domoticz_state.count > 0))
        ui_push(screen_domoticz_create());
    else
        ui_push(screen_lights_create());
}
/* Click handler for the optional LT_LIGHTS grid tile (custom layout). */
static void on_lights_tile(lv_event_t * e) { (void)e; open_lights_backend(); }

/* ---- swipeable tile pages ---- */
static void home_set_dot(int active) {
    for (int i = 0; i < 2; i++)
        if (home_dot[i])
            lv_obj_set_style_bg_opa(home_dot[i],
                                    i == active ? LV_OPA_COVER : LV_OPA_30, 0);
}
static void home_show_page(int n) {
    home_tile_page = n;
    lv_obj_t * p0[5] = { tile_waste, tile_energy, tile_vent, tile_family, tile_water };
    static const int p0_type[5] = { LT_WASTE, LT_ENERGY, LT_VENT, LT_FAMILY, LT_WATER };
    if (n == 0) {
        for (int i = 0; i < 5; i++) {
            if (!p0[i]) continue;
            /* In custom layout, the layout decides visibility — don't un-hide a
             * tile the user removed/hid (the offline pass only covers 4 of 5). */
            if (settings.custom_layout_enabled) {
                const layout_tile_t * L = layout_find(p0_type[i]);
                if (!L || !L->visible) { lv_obj_add_flag(p0[i], LV_OBJ_FLAG_HIDDEN); continue; }
            }
            lv_obj_clear_flag(p0[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (home_page1) lv_obj_add_flag(home_page1, LV_OBJ_FLAG_HIDDEN);
        /* ticker belongs to page 0 — show it again (if news is on, and the
         * custom layout hasn't hidden it). */
        if (news_ticker && settings.news_enabled) {
            const layout_tile_t * L = settings.custom_layout_enabled ? layout_find(LT_NEWS_TICKER) : NULL;
            if (!settings.custom_layout_enabled || (L && L->visible)) {
                if (news_box) lv_obj_clear_flag(news_box, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(news_ticker, LV_OBJ_FLAG_HIDDEN);
            } else {
                if (news_box) lv_obj_add_flag(news_box, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(news_ticker, LV_OBJ_FLAG_HIDDEN);
            }
        }
        apply_offline_tile_visibility();   /* re-hide offline tiles after un-hiding */
    } else {
        for (int i = 0; i < 5; i++) if (p0[i]) lv_obj_add_flag(p0[i], LV_OBJ_FLAG_HIDDEN);
        if (home_page1) {
            lv_obj_clear_flag(home_page1, LV_OBJ_FLAG_HIDDEN);
            /* bring to front so a full-screen custom page 2 covers the thermostat
             * + weather strip (which are created later, hence higher z). */
            lv_obj_move_foreground(home_page1);
        }
        /* hide the page-0 ticker so it doesn't draw over the page-2 slots */
        if (news_box) lv_obj_add_flag(news_box, LV_OBJ_FLAG_HIDDEN);
        if (news_ticker) lv_obj_add_flag(news_ticker, LV_OBJ_FLAG_HIDDEN);
    }
    home_set_dot(n);
}
static void on_page1_slot(lv_event_t * e) {
    lv_obj_t * tapped = lv_event_get_current_target(e);
    for (int i = 0; i < g_p1_count; i++) {
        if (p1_main[i] && tapped == lv_obj_get_parent(p1_main[i])) {
            /* If slot 0 is auto-rotating, open the currently displayed item */
            if (i == 0 && settings.tile_rotate_enabled && settings.tile_rotate_members[0] && g_current_rot_id[0]) {
                if (open_integration_by_id(g_current_rot_id)) return;
            }
            if (check_slot_integration_open(4 + i)) return;

            /* Slot 0: efanlamp tile when configured */
            if (i == 0 && settings.efanlamp_host[0]) {
                ui_push(screen_efanlamp_create());
                return;
            }

            /* Unassigned or unhandled slot: open single-slot assignment modal specifically for this slot! */
            screen_settings_open_tile_slot_single_modal(4 + i);
            return;
        }
    }
}

static void on_page1_slot_longpress(lv_event_t * e) {
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    screen_settings_open_tile_slot_single_modal(slot);
}
void screen_home_reset_to_main(void) {
    if (home_tile_page != 0) home_show_page(0);
}
/* Headless-sim helper: page 2 is gesture-only, so the sim can't swipe to it. */
void screen_home_force_page(int n) { home_show_page(n); }
static void on_home_gesture_to_lights(lv_event_t * e) {
    (void)e;
    open_lights_backend();
}

/* Collapsible lights handle: idles as a slim half-circle tab peeking from the
 * left edge, expands into a horizontal "Lights" button while pressed, and
 * opens the lights backend on release. */
static void lights_handle_set(bool open) {
    if (!lights_handle) return;
    lv_obj_set_size(lights_handle, open ? 160 : 56, open ? 56 : 80);
    lv_obj_align(lights_handle, LV_ALIGN_LEFT_MID, open ? 4 : -4, 0);
    lv_obj_set_style_radius(lights_handle, open ? 16 : LV_RADIUS_CIRCLE, 0);
    if (open) {
        /* Expanded: word label, hide the logo. */
        if (lights_handle_img) lv_obj_add_flag(lights_handle_img, LV_OBJ_FLAG_HIDDEN);
        if (lights_handle_lbl) {
            lv_label_set_text(lights_handle_lbl, tr("Apparaten", "Devices"));
            lv_obj_set_style_text_font(lights_handle_lbl, SF(22), 0);
            lv_obj_clear_flag(lights_handle_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        /* Collapsed: the backend logo if we have one, else the charge glyph. */
        if (lights_handle_img) {
            lv_obj_clear_flag(lights_handle_img, LV_OBJ_FLAG_HIDDEN);
            if (lights_handle_lbl) lv_obj_add_flag(lights_handle_lbl, LV_OBJ_FLAG_HIDDEN);
        } else if (lights_handle_lbl) {
            lv_label_set_text(lights_handle_lbl, LV_SYMBOL_CHARGE);
            lv_obj_set_style_text_font(lights_handle_lbl, SF(28), 0);
            lv_obj_clear_flag(lights_handle_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
static void on_lights_handle(lv_event_t * e) {
    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:                            lights_handle_set(true);  break;
        case LV_EVENT_RELEASED: case LV_EVENT_PRESS_LOST: lights_handle_set(false); break;
        case LV_EVENT_CLICKED:  lights_handle_set(false); open_lights_backend();    break;
        default: break;
    }
}

static void on_home_gesture(lv_event_t * e) {
    (void)e;
    lv_indev_t * indev = lv_indev_get_act();
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    lv_point_t p; lv_indev_get_point(indev, &p);
    /* When already on page 2, any right-swipe returns to page 0. A custom page 2
     * is full-screen+opaque so the touch lands left of x≈556 (over the covered
     * thermostat zone) — gating the back-swipe on x there left the user stuck on
     * page 2 (BUG 8). The lights shortcut only applies on page 0. */
    if (home_tile_page != 0) {
        if (dir == LV_DIR_RIGHT) home_show_page(0);
        return;
    }
    /* Page 0: right of x≈556 is the tile zone: swipe pages between tile sets.
     * Left of it (the thermostat) keeps the swipe-right-for-lights shortcut. */
    if (p.x >= 556) {
        if (dir == LV_DIR_LEFT) home_show_page(1);
    } else if (dir == LV_DIR_RIGHT) {
        open_lights_backend();
    }
}

static void news_modal_close(lv_event_t * e) {
    if (e) lv_event_stop_bubbling(e);
    if (news_modal) {
        lv_obj_del_async(news_modal);
        news_modal = NULL;
        news_pane = NULL;
        news_body_lbl = NULL;
        news_body_title = NULL;
        news_list = NULL;
    }
}
/* Show the i-th article's title + summary in the reading pane. */
static void news_show_body(int i) {
    if (news_pane) lv_obj_scroll_to_y(news_pane, 0, LV_ANIM_OFF);
    if (news_count() == 0) {
        if (news_body_title) lv_label_set_text(news_body_title, tr("Geen artikelen geladen", "No articles loaded"));
        if (news_body_lbl)   lv_label_set_text(news_body_lbl,
            tr("Er zijn nog geen nieuwsberichten opgehaald.\n\nControleer of de Toon verbinding heeft met internet en of de RSS-feed URL klopt in Instellingen -> Nieuwslezer.",
               "No news articles loaded yet.\n\nCheck if Toon is connected to the internet and verify the RSS feed URL in Settings -> Newsreader."));
        return;
    }
    char title[NEWS_TITLE_MAX] = "", body[NEWS_BODY_MAX] = "";
    news_item(i, title, sizeof title, NULL, 0);
    news_body(i, body, sizeof body);
    if (news_body_title) lv_label_set_text(news_body_title, title);
    if (news_body_lbl)   lv_label_set_text(news_body_lbl,
                             body[0] ? body : tr("(geen samenvatting beschikbaar)", "(no summary available)"));
}
static void on_news_item(lv_event_t * e) {
    news_show_body((int)(intptr_t)lv_event_get_user_data(e));
}
/* Live ticker scroll-speed change from Settings (px/sec). */
void screen_home_set_ticker_speed(int spd) {
    if (!news_ticker) return;
    /* Below ~33 px/s the offset moves <1px per 30ms refresh, so the integer
       scroll position steps once a second instead of gliding — looks broken
       and choppy. Floor it to a smooth value. */
    if (spd < 30) spd = 30;
    lv_obj_set_style_anim_speed(news_ticker, spd, 0);
    /* lv_label only reads anim_speed when it (re)builds the scroll animation
       inside lv_label_refr_text — setting the style alone leaves the running
       anim at its old speed. Re-apply the long mode to rebuild it now so the
       change is visible immediately rather than at the next headline rotation. */
    lv_label_set_long_mode(news_ticker, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

/* Tap a feed header → toggle that feed's section collapsed/expanded. */
static void on_news_feed_toggle(lv_event_t * e) {
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    if (f >= 0 && f < NEWS_MAX_FEEDS) news_collapsed[f] = !news_collapsed[f];
    news_list_rebuild();
}

/* Add one tappable headline row (article i) to the list. */
static void news_add_item_row(int i) {
    char ti[NEWS_TITLE_MAX];
    if (news_item(i, ti, sizeof ti, NULL, 0) != 0) return;
    lv_obj_t * b = lv_list_add_btn(news_list, NULL, ti);
    lv_obj_set_style_text_font(b, SF(16), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(settings.home_light_mode ? 0xffffff : 0x16243a), 0);
    lv_obj_add_event_cb(b, on_news_item, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t * bl = lv_obj_get_child(b, lv_obj_get_child_cnt(b) - 1);
    if (bl) {
        lv_label_set_long_mode(bl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(bl, lv_pct(100));
        lv_obj_set_style_text_color(bl, lv_color_hex(settings.home_light_mode ? 0x1e2838 : 0xffffff), 0);
    }
}

/* Rebuild the headline list grouped by feed, honouring per-feed collapse. */
static void news_list_rebuild(void) {
    if (!news_list) return;
    lv_obj_clean(news_list);
    int nf = news_feed_count();
    int n  = news_count();
    if (n == 0) {
        lv_obj_t * empty = lv_list_add_text(news_list, tr("Geen artikelen geladen", "No articles loaded"));
        lv_obj_set_style_text_font(empty, SF(16), 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(settings.home_light_mode ? 0x687c94 : 0x8899aa), 0);
        return;
    }
    for (int f = 0; f < nf; f++) {
        char fname[64]; news_feed_name(f, fname, sizeof fname);
        int cnt = 0;
        for (int i = 0; i < n; i++) if (news_item_feed(i) == f) cnt++;
        char hdr[128];
        snprintf(hdr, sizeof hdr, "%s  %s  (%d)",
                 news_collapsed[f] ? LV_SYMBOL_RIGHT : LV_SYMBOL_DOWN, fname, cnt);
        lv_obj_t * h = lv_list_add_btn(news_list, NULL, hdr);
        lv_obj_set_style_text_font(h, SF(16), 0);
        lv_obj_set_style_bg_color(h, lv_color_hex(settings.home_light_mode ? 0xd0d8e2 : 0x24364f), 0);
        lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(h, lv_color_hex(settings.home_light_mode ? 0x1e2838 : 0x9fd0ff), 0);
        lv_obj_add_event_cb(h, on_news_feed_toggle, LV_EVENT_CLICKED, (void *)(intptr_t)f);
        if (news_collapsed[f]) continue;
        for (int i = 0; i < n; i++)
            if (news_item_feed(i) == f) news_add_item_row(i);
    }
    /* No feed grouping (e.g. pre-upgrade data) → flat list. */
    if (nf == 0)
        for (int i = 0; i < n; i++) news_add_item_row(i);
}

static void on_news_tap(lv_event_t * e) {
    (void)e;
    if (news_modal) return;
    if (news_count() == 0) news_wake_fetch();
    news_modal_seq = news_seq();
    news_had_items = (news_count() > 0);
    news_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(news_modal);
    lv_obj_set_size(news_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(news_modal, 0, 0);
    lv_obj_set_style_bg_color(news_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(news_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(news_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(news_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(news_modal, news_modal_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t * card = lv_obj_create(news_modal);
    lv_obj_set_size(card, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(settings.home_light_mode ? 0xffffff : 0x16243a), 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps */

    int margin_x = SX(12);
    int top_y    = (DISP_VER < 600) ? 50 : 56;
    int bot_pad  = SY(12);
    int avail_w  = LV_HOR_RES - (margin_x * 3);
    int list_w   = (avail_w * 44) / 100;
    int pane_w   = avail_w - list_w;
    int content_h = LV_VER_RES - top_y - bot_pad;

    lv_obj_t * t = lv_label_create(card);
    lv_obj_set_style_text_font(t, SF(24), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(settings.home_light_mode ? 0x1e2838 : 0xffffff), 0);
    lv_label_set_text(t, tr("Nieuws", "News"));
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, SX(16), (top_y - 24) / 2);

    news_list = lv_list_create(card);
    lv_obj_set_size(news_list, list_w, content_h);
    lv_obj_set_pos(news_list, margin_x, top_y);
    lv_obj_set_style_bg_color(news_list, lv_color_hex(settings.home_light_mode ? 0xf0f4f8 : 0x0e1a2a), 0);
    /* Start with every feed expanded; headers collapse/expand on tap. */
    for (int f = 0; f < NEWS_MAX_FEEDS; f++) news_collapsed[f] = false;
    news_list_rebuild();

    /* Reading pane (right) — full-height vertically scrollable column holding
       both the selected article's title and full body text. */
    news_pane = lv_obj_create(card);
    lv_obj_set_size(news_pane, pane_w, content_h);
    lv_obj_set_pos(news_pane, margin_x + list_w + margin_x, top_y);
    lv_obj_set_style_bg_color(news_pane, lv_color_hex(settings.home_light_mode ? 0xf0f4f8 : 0x0e1a2a), 0);
    lv_obj_set_style_border_width(news_pane, 0, 0);
    lv_obj_set_style_radius(news_pane, 10, 0);
    lv_obj_set_style_pad_all(news_pane, SX(12), 0);
    lv_obj_set_style_pad_row(news_pane, SY(10), 0);
    lv_obj_set_flex_flow(news_pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(news_pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(news_pane, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(news_pane, LV_SCROLLBAR_MODE_AUTO);

    news_body_title = lv_label_create(news_pane);
    lv_obj_set_style_text_font(news_body_title, SF(20), 0);
    lv_obj_set_style_text_color(news_body_title, lv_color_hex(settings.home_light_mode ? 0x1e2838 : 0xffffff), 0);
    lv_label_set_long_mode(news_body_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(news_body_title, lv_pct(100));

    news_body_lbl = lv_label_create(news_pane);
    lv_obj_set_style_text_font(news_body_lbl, SF(16), 0);
    lv_obj_set_style_text_color(news_body_lbl, lv_color_hex(settings.home_light_mode ? 0x334455 : 0xcdd9e6), 0);
    lv_obj_set_style_text_line_space(news_body_lbl, SY(4), 0);
    lv_label_set_long_mode(news_body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(news_body_lbl, lv_pct(100));

    /* Top-right "X" close button — created last and moved to foreground so it is
       unconditionally on top of the reading pane and everything else */
    lv_obj_t * x = lv_btn_create(card);
    int x_w = (DISP_HOR < 1000) ? 80 : 88;
    lv_obj_set_size(x, x_w, top_y);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(settings.home_light_mode ? 0xd0d8e2 : 0x33445a), 0);
    lv_obj_set_style_radius(x, 0, 0);
    lv_obj_set_ext_click_area(x, 25);
    lv_obj_add_event_cb(x, news_modal_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(xl, SF(22), 0);
    lv_obj_set_style_text_color(xl, lv_color_hex(settings.home_light_mode ? 0x1e2838 : 0xffffff), 0);
    lv_obj_center(xl);
    lv_obj_move_foreground(x);

    news_show_body(0);
    lv_obj_move_foreground(news_modal);
}

/* Open the news reader from elsewhere (e.g. Settings → News) so it's reachable
   even when the home-screen ticker is turned off. */
void screen_home_open_news(void) { on_news_tap(NULL); }

/* ===================== Life360 location map ===========================
 * Tapping the Family tile opens a map of the tracked person's current GPS.
 * We fetch a 2x2 block of OpenStreetMap raster tiles around their coordinate
 * (off the UI thread — downloads must never block LVGL) and overlay a marker.
 * A/B buttons switch between the two tracked people. */
#define MAP_TILE_DISP SX(200)      /* on-screen px per 256px OSM tile (downscaled to fit; SX-scaled for Toon 1) */
typedef struct { float lat, lon; int zoom; } map_req_t;

static int g_map_zoom   = 13;      /* OSM zoom level; lower = wider area / less detail */
static int g_map_person = 0;       /* which tracked person is shown */
static lv_obj_t  * map_modal    = NULL;
static lv_obj_t  * map_box      = NULL;
static lv_obj_t  * map_imgs[4]  = {0};
static lv_obj_t  * map_marker   = NULL;
static lv_obj_t  * map_addr_lbl = NULL;
static lv_obj_t  * map_top_lbl  = NULL;   /* header: who + address */
static lv_obj_t  * map_btn_a    = NULL;
static lv_obj_t  * map_btn_b    = NULL;
static lv_obj_t  * map_btn_c    = NULL;
static lv_obj_t  * map_status   = NULL;
static lv_timer_t * map_timer   = NULL;
static volatile int g_map_ready = 0;            /* 0 loading, 1 ok, -1 fail */
static volatile int g_map_mx = 256, g_map_my = 256;  /* marker px in the 512 box */

static void * map_fetch_thread(void * arg) {
    map_req_t r = *(map_req_t *)arg; free(arg);
    int z = r.zoom, n = 1 << z;
    double latrad = r.lat * M_PI / 180.0;
    double fx = (r.lon + 180.0) / 360.0 * n;
    double fy = (1.0 - asinh(tan(latrad)) / M_PI) / 2.0 * n;
    int left = (int)floor(fx - 0.5), top = (int)floor(fy - 0.5);
    g_map_mx = (int)((fx - left) * 256.0);
    g_map_my = (int)((fy - top) * 256.0);
    int ok = 1;
    for (int row = 0; row < 2; row++)
        for (int col = 0; col < 2; col++) {
            int tx = left + col, ty = top + row;
            tx = ((tx % n) + n) % n;
            if (ty < 0) ty = 0;
            if (ty >= n) ty = n - 1;
            char cmd[400];
            snprintf(cmd, sizeof cmd,
                "curl -fsSL -m 12 -A 'freetoon-map/1.0' -o /tmp/map_%d.png "
                "'https://tile.openstreetmap.org/%d/%d/%d.png'",
                row * 2 + col, z, tx, ty);
            if (system(cmd) != 0) ok = 0;
        }
    g_map_ready = ok ? 1 : -1;
    return NULL;
}

static void map_tick(lv_timer_t * t) {
    (void)t;
    if (g_map_ready == 0) return;                 /* still downloading */
    if (map_timer) { lv_timer_del(map_timer); map_timer = NULL; }
    if (g_map_ready < 0) {
        if (map_status) lv_label_set_text(map_status, tr("Kaart laden mislukt (geen internet?)", "Map load failed (no internet?)"));
        return;
    }
    static const char * paths[4] = {
        "S:/tmp/map_0.png", "S:/tmp/map_1.png", "S:/tmp/map_2.png", "S:/tmp/map_3.png" };
    lv_img_cache_invalidate_src(NULL);            /* paths are reused — force reload */
    for (int i = 0; i < 4; i++) {
        if (!map_imgs[i]) continue;
        lv_img_set_src(map_imgs[i], paths[i]);
        lv_img_set_pivot(map_imgs[i], 0, 0);                  /* scale from top-left */
        lv_img_set_zoom(map_imgs[i], MAP_TILE_DISP);          /* 256 = 100%; downscale to fit */
        lv_obj_set_pos(map_imgs[i], (i % 2) * MAP_TILE_DISP, (i / 2) * MAP_TILE_DISP);
    }
    if (map_marker) {
        int mx = g_map_mx * MAP_TILE_DISP / 256;
        int my = g_map_my * MAP_TILE_DISP / 256;
        lv_obj_set_pos(map_marker, mx - 7, my - 7);
        lv_obj_clear_flag(map_marker, LV_OBJ_FLAG_HIDDEN);
    }
    if (map_status) lv_obj_add_flag(map_status, LV_OBJ_FLAG_HIDDEN);
}

static void start_map_fetch(int person) {
    if (person < 0 || person > 2) person = 0;
    g_map_person = person;
    float lat = person == 2 ? ha_state.lat_c : person == 1 ? ha_state.lat_b : ha_state.lat_a;
    float lon = person == 2 ? ha_state.lon_c : person == 1 ? ha_state.lon_b : ha_state.lon_a;
    const char * names[3] = { settings.life360_a_name, settings.life360_b_name, settings.life360_c_name };
    static const char * fallback[3] = { "A", "B", "C" };
    const char * who = names[person][0] ? names[person] : fallback[person];
    const char * loc = person == 2 ? (const char *)ha_state.loc_c
                     : person == 1 ? (const char *)ha_state.loc_b
                                   : (const char *)ha_state.loc_a;
    if (map_addr_lbl) lv_label_set_text_fmt(map_addr_lbl, "%s\n%s", who, loc[0] ? loc : "?");
    if (map_top_lbl)  lv_label_set_text_fmt(map_top_lbl, "%s  -  %s", who, loc[0] ? loc : "?");
    /* Highlight which person is selected. */
    if (map_btn_a) lv_obj_set_style_bg_color(map_btn_a,
                       lv_color_hex(person == 0 ? 0x2e6e3a : 0x2b3f5c), 0);
    if (map_btn_b) lv_obj_set_style_bg_color(map_btn_b,
                       lv_color_hex(person == 1 ? 0x2e6e3a : 0x2b3f5c), 0);
    if (map_btn_c) lv_obj_set_style_bg_color(map_btn_c,
                       lv_color_hex(person == 2 ? 0x2e6e3a : 0x2b3f5c), 0);
    if (map_marker)   lv_obj_add_flag(map_marker, LV_OBJ_FLAG_HIDDEN);
    if (map_status)   lv_obj_clear_flag(map_status, LV_OBJ_FLAG_HIDDEN);

    if (lat == 0.0f && lon == 0.0f) {
        if (map_status) lv_label_set_text(map_status, tr("Geen GPS-locatie bekend", "No GPS location known"));
        return;
    }
    if (map_status) lv_label_set_text(map_status, tr("Kaart laden...", "Loading map..."));
    g_map_ready = 0;
#ifdef WASM_BUILD
    /* WASM: no pthread + no system("curl"). Compute the 4 tile coords here
     * and hand them to wasm_map_kick (defined via EM_JS below), which fetches
     * the four OSM tiles in parallel and writes them into MEMFS at the same
     * /tmp/map_*.png paths the native thread uses. wasm_map_done() flips
     * g_map_ready so map_tick paints them. */
    {
        int z = g_map_zoom, n = 1 << z;
        double latrad = lat * M_PI / 180.0;
        double fx = (lon + 180.0) / 360.0 * n;
        double fy = (1.0 - asinh(tan(latrad)) / M_PI) / 2.0 * n;
        int left = (int)floor(fx - 0.5), topp = (int)floor(fy - 0.5);
        g_map_mx = (int)((fx - left) * 256.0);
        g_map_my = (int)((fy - topp) * 256.0);
        int tx[4], ty[4];
        for (int i = 0; i < 4; i++) {
            int rr = i / 2, cc = i % 2;
            int t  = ((left + cc) % n + n) % n;
            int v  = topp + rr;
            if (v < 0) v = 0; if (v >= n) v = n - 1;
            tx[i] = t; ty[i] = v;
        }
        extern void wasm_map_kick(int z,
            int tx0, int ty0, int tx1, int ty1,
            int tx2, int ty2, int tx3, int ty3);
        wasm_map_kick(z, tx[0],ty[0], tx[1],ty[1], tx[2],ty[2], tx[3],ty[3]);
    }
    if (!map_timer) map_timer = lv_timer_create(map_tick, 300, NULL);
    return;
#endif
    map_req_t * r = malloc(sizeof *r);
    if (!r) return;
    r->lat = lat; r->lon = lon; r->zoom = g_map_zoom;
    pthread_t th;
    if (pthread_create(&th, NULL, map_fetch_thread, r) == 0) pthread_detach(th);
    else { free(r); if (map_status) lv_label_set_text(map_status, tr("Kaart laden mislukt", "Map load failed")); return; }
    if (!map_timer) map_timer = lv_timer_create(map_tick, 300, NULL);
}

#ifdef WASM_BUILD
/* JS callback when the 4 OSM tile fetches finish (+1) or any fail (-1). */
EMSCRIPTEN_KEEPALIVE
void wasm_map_done(int rc) { g_map_ready = rc; }

/* JS-defined fetcher (EM_JS handles arbitrary commas in the body — EM_ASM
 * doesn't because the C preprocessor splits its args at top-level commas).
 * Goes through pwa_server's /api/tile proxy so the tile fetch is same-origin
 * (browsers block direct cross-origin fetches to tile.openstreetmap.org). */
EM_JS(void, wasm_map_kick, (int z,
                            int tx0, int ty0, int tx1, int ty1,
                            int tx2, int ty2, int tx3, int ty3), {
    var coords = [[tx0,ty0],[tx1,ty1],[tx2,ty2],[tx3,ty3]];
    Promise.all(coords.map(function(c, i) {
        var url  = '/api/tile?z=' + z + '&x=' + c[0] + '&y=' + c[1];
        var path = '/tmp/map_' + i + '.png';
        return fetch(url)
            .then(function(r) { return r.ok ? r.arrayBuffer() : Promise.reject(r.status); })
            .then(function(buf) {
                try { FS.unlink(path); } catch(e) {}
                FS.writeFile(path, new Uint8Array(buf));
            });
    })).then(function()  { Module._wasm_map_done(1); })
       .catch(function(e) { console.warn('[map] fetch fail:', e); Module._wasm_map_done(-1); });
});
#endif

static void map_close(lv_event_t * e) {
    (void)e;
    if (map_timer) { lv_timer_del(map_timer); map_timer = NULL; }
    if (map_modal) { lv_obj_del(map_modal); map_modal = NULL; }
    for (int i = 0; i < 4; i++) map_imgs[i] = NULL;
    map_box = map_marker = map_addr_lbl = map_status = NULL;
    map_top_lbl = map_btn_a = map_btn_b = map_btn_c = NULL;
}
static void on_map_a(lv_event_t * e) { (void)e; start_map_fetch(0); }
static void on_map_b(lv_event_t * e) { (void)e; start_map_fetch(1); }
static void on_map_c(lv_event_t * e) { (void)e; start_map_fetch(2); }
static void on_map_zoom_in(lv_event_t * e)  { (void)e; if (g_map_zoom < 18) { g_map_zoom++; start_map_fetch(g_map_person); } }
static void on_map_zoom_out(lv_event_t * e) { (void)e; if (g_map_zoom > 9)  { g_map_zoom--; start_map_fetch(g_map_person); } }

static void open_family_map(lv_event_t * e) {
    (void)e;
    if (check_slot_integration_open(1)) return;
    if (map_modal) return;

    map_modal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(map_modal);
    lv_obj_set_size(map_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(map_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(map_modal, LV_OPA_70, 0);
    lv_obj_add_flag(map_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(map_modal, map_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t * card = lv_obj_create(map_modal);
    lv_obj_set_size(card, SX(640), SY(500));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x16243a), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * h = lv_label_create(card);
    lv_obj_set_style_text_font(h, SF(28), 0);
    lv_obj_set_style_text_color(h, lv_color_hex(0xffffff), 0);
    lv_label_set_text(h, tr("Locatie", "Location"));
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, SX(16), SY(10));

    /* Header subtitle: who is selected + their address. */
    map_top_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(map_top_lbl, SF(18), 0);
    lv_obj_set_style_text_color(map_top_lbl, lv_color_hex(0x9fd0ff), 0);
    lv_label_set_long_mode(map_top_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(map_top_lbl, SX(420));
    lv_obj_align(map_top_lbl, LV_ALIGN_TOP_LEFT, SX(150), SY(18));
    lv_label_set_text(map_top_lbl, "");

    /* Map area — 2x2 OSM tiles downscaled to MAP_TILE_DISP each. */
    map_box = lv_obj_create(card);
    lv_obj_set_size(map_box, MAP_TILE_DISP * 2, MAP_TILE_DISP * 2);
    lv_obj_align(map_box, LV_ALIGN_TOP_LEFT, SX(16), SY(52));
    lv_obj_set_style_bg_color(map_box, lv_color_hex(0x0e1a2a), 0);
    lv_obj_set_style_border_width(map_box, 0, 0);
    lv_obj_set_style_pad_all(map_box, 0, 0);
    lv_obj_set_style_radius(map_box, 8, 0);
    lv_obj_clear_flag(map_box, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) {
        map_imgs[i] = lv_img_create(map_box);
        lv_obj_set_pos(map_imgs[i], (i % 2) * MAP_TILE_DISP, (i / 2) * MAP_TILE_DISP);
    }
    map_marker = lv_obj_create(map_box);
    lv_obj_set_size(map_marker, 14, 14);
    lv_obj_set_style_radius(map_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(map_marker, lv_color_hex(0xff3030), 0);
    lv_obj_set_style_border_width(map_marker, 2, 0);
    lv_obj_set_style_border_color(map_marker, lv_color_hex(0xffffff), 0);
    lv_obj_add_flag(map_marker, LV_OBJ_FLAG_HIDDEN);

    map_status = lv_label_create(map_box);
    lv_obj_set_style_text_color(map_status, lv_color_hex(0xcdd9e6), 0);
    lv_obj_set_style_text_font(map_status, SF(20), 0);
    lv_obj_center(map_status);
    lv_label_set_text(map_status, tr("Kaart laden...", "Loading map..."));

    /* Right column: address + zoom + A/B switch + close. */
    int px = 16 + MAP_TILE_DISP * 2 + 16;     /* just right of the map */
    map_addr_lbl = lv_label_create(card);
    lv_obj_set_style_text_color(map_addr_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(map_addr_lbl, SF(18), 0);
    lv_label_set_long_mode(map_addr_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(map_addr_lbl, SX(168));
    lv_obj_align(map_addr_lbl, LV_ALIGN_TOP_LEFT, px, SY(56));

    /* Zoom −/+ */
    lv_obj_t * zo = lv_btn_create(card);
    lv_obj_set_size(zo, SX(78), SY(56));
    lv_obj_align(zo, LV_ALIGN_TOP_LEFT, px, SY(170));
    lv_obj_add_event_cb(zo, on_map_zoom_out, LV_EVENT_CLICKED, NULL);
    lv_obj_t * zol = lv_label_create(zo);
    lv_obj_set_style_text_font(zol, SF(28), 0);
    lv_label_set_text(zol, LV_SYMBOL_MINUS); lv_obj_center(zol);
    lv_obj_t * zi = lv_btn_create(card);
    lv_obj_set_size(zi, SX(78), SY(56));
    lv_obj_align(zi, LV_ALIGN_TOP_LEFT, px + SX(90), SY(170));
    lv_obj_add_event_cb(zi, on_map_zoom_in, LV_EVENT_CLICKED, NULL);
    lv_obj_t * zil = lv_label_create(zi);
    lv_obj_set_style_text_font(zil, SF(28), 0);
    lv_label_set_text(zil, LV_SYMBOL_PLUS); lv_obj_center(zil);

    map_btn_a = lv_btn_create(card);
    lv_obj_set_size(map_btn_a, SX(168), SY(50));
    lv_obj_align(map_btn_a, LV_ALIGN_TOP_LEFT, px, SY(240));
    lv_obj_add_event_cb(map_btn_a, on_map_a, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bal = lv_label_create(map_btn_a);
    lv_label_set_text(bal, settings.life360_a_name[0] ? settings.life360_a_name : "A");
    lv_obj_center(bal);

    if (settings.life360_b_entity[0]) {
        map_btn_b = lv_btn_create(card);
        lv_obj_set_size(map_btn_b, SX(168), SY(50));
        lv_obj_align(map_btn_b, LV_ALIGN_TOP_LEFT, px, SY(300));
        lv_obj_add_event_cb(map_btn_b, on_map_b, LV_EVENT_CLICKED, NULL);
        lv_obj_t * bbl = lv_label_create(map_btn_b);
        lv_label_set_text(bbl, settings.life360_b_name[0] ? settings.life360_b_name : "B");
        lv_obj_center(bbl);
    }

    if (settings.life360_c_entity[0]) {
        map_btn_c = lv_btn_create(card);
        lv_obj_set_size(map_btn_c, SX(168), SY(50));
        lv_obj_align(map_btn_c, LV_ALIGN_TOP_LEFT, px, SY(360));
        lv_obj_add_event_cb(map_btn_c, on_map_c, LV_EVENT_CLICKED, NULL);
        lv_obj_t * ccl = lv_label_create(map_btn_c);
        lv_label_set_text(ccl, settings.life360_c_name[0] ? settings.life360_c_name : "C");
        lv_obj_center(ccl);
    }

    lv_obj_t * x = lv_btn_create(card);
    int x_w = (DISP_HOR < 1000) ? 68 : 76;
    int x_h = (DISP_VER < 600) ? 46 : 50;
    lv_obj_set_size(x, x_w, x_h);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, SX(-10), SY(6));
    lv_obj_set_style_bg_color(x, lv_color_hex(0x33445a), 0);
    lv_obj_set_style_radius(x, 12, 0);
    lv_obj_set_ext_click_area(x, 30);
    lv_obj_add_event_cb(x, map_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(xl, SF(22), 0);
    lv_obj_set_style_text_color(xl, lv_color_hex(0xffffff), 0);
    lv_obj_center(xl);

    start_map_fetch(0);
}

/* Public entry so other screens (e.g. the stock-theme home's Trackers tile)
 * can open the same Life360 location map modal. */
void life360_map_open(void) { open_family_map(NULL); }

lv_obj_t * screen_home_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_home_gesture, LV_EVENT_GESTURE, NULL);

    /* Top-of-screen package banner (hidden when queue is empty). Tap
     * dismisses the top entry. Receives from MQTT via packages.c. */
    packages_banner_attach(scr_root);

    /* Layout (1024x600):
         Big thermostat tile:  20,20  520x420   (col 0-1 spanned, row 0-1 spanned)
         Tile col 2 row 0:    560,20  220x200   Air quality
         Tile col 3 row 0:    790,20  220x200   Humidity
         Tile col 2 row 1:    560,230 220x200   Boiler / heating
         Tile col 3 row 1:    790,230 220x200   Inbox (placeholder)
         Bottom row (4 small tiles): 20,450 -> 1004,580
           Energy (P1), Buienradar, Waste, Settings
    */

    /* --- Clock / Date tile (LT_CLOCK) --- */
    lv_obj_t * tile_clock = lv_obj_create(scr_root);
    lv_obj_set_size(tile_clock, 330, 140);
    lv_obj_set_pos(tile_clock, 20, 20);
    if (settings.custom_layout_enabled) {
        const layout_tile_t * Lc = layout_find(LT_CLOCK);
        if (Lc && Lc->visible) {
            int x, y, w, h;
            layout_cell_px(Lc->col, Lc->row, Lc->w, Lc->h, &x, &y, &w, &h);
            x += 4; y += 4; w -= 8; h -= 8;     /* inter-tile margin */
            lv_obj_set_pos(tile_clock, x, y);
            lv_obj_set_size(tile_clock, w, h);
        } else if (Lc) {
            lv_obj_add_flag(tile_clock, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_set_style_bg_color(tile_clock, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_border_width(tile_clock, 0, 0);
    lv_obj_set_style_radius(tile_clock, 18, 0);
    lv_obj_set_style_pad_all(tile_clock, 8, 0);
    lv_obj_clear_flag(tile_clock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tile_clock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile_clock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tile_clock, (DISP_VER < 600 ? 2 : 4), 0);

    /* Clock row: Hours : Minutes centered horizontally */
    lv_obj_t * clk_row = lv_obj_create(tile_clock);
    lv_obj_set_size(clk_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(clk_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clk_row, 0, 0);
    lv_obj_set_style_pad_all(clk_row, 0, 0);
    lv_obj_set_style_pad_column(clk_row, (DISP_VER < 600 ? 1 : 2), 0);
    lv_obj_clear_flag(clk_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(clk_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clk_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const lv_font_t * font_clock = SF(64);

    lbl_t_clock_h = lv_label_create(clk_row);
    lv_obj_set_style_text_color(lbl_t_clock_h, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_t_clock_h, font_clock, 0);
    lv_label_set_text(lbl_t_clock_h, "--");

    lbl_t_clock_colon = lv_label_create(clk_row);
    lv_obj_set_style_text_color(lbl_t_clock_colon, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_t_clock_colon, font_clock, 0);
    lv_label_set_text(lbl_t_clock_colon, ":");
    clock_colon_pulse(lbl_t_clock_colon);

    lbl_t_clock_m = lv_label_create(clk_row);
    lv_obj_set_style_text_color(lbl_t_clock_m, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_t_clock_m, font_clock, 0);
    lv_label_set_text(lbl_t_clock_m, "--");

    /* Date label centered horizontally */
    lbl_t_date = lv_label_create(tile_clock);
    lv_obj_set_style_text_color(lbl_t_date, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_t_date, SF(22), 0);
    lv_obj_set_style_text_align(lbl_t_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_t_date, "");

    /* Moon phase icon (top-right corner, non-interfering) */
    moon_phase_img = lv_img_create(tile_clock);
    lv_obj_add_flag(moon_phase_img, LV_OBJ_FLAG_FLOATING);
    lv_img_set_src(moon_phase_img, moon_phase_icon(40));
    lv_obj_set_style_img_recolor(moon_phase_img, lv_color_hex(settings.home_light_mode ? 0x485868 : 0xffffff), 0);
    lv_obj_set_style_img_recolor_opa(moon_phase_img, 255, 0);
    lv_obj_align(moon_phase_img, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_add_flag(moon_phase_img, LV_OBJ_FLAG_HIDDEN);

    /* --- Compact Thermostat Tile (LT_THERMOSTAT) ---
     * Shows indoor temperature + flame icon next to it, setpoint, and +/- buttons.
     * Tapping the tile opens the full thermostat detail screen. */
    lv_obj_t * th = lv_obj_create(scr_root);
    lv_obj_set_size(th, 330, 210);
    lv_obj_set_pos(th, 20, 175);
    int th_w = 330, th_h = 210;
    if (settings.custom_layout_enabled) {
        const layout_tile_t * L = layout_find(LT_THERMOSTAT);
        if (L && L->visible) {
            int x, y, w, h;
            layout_cell_px(L->col, L->row, L->w, L->h, &x, &y, &w, &h);
            x += 4; y += 4; w -= 8; h -= 8;     /* inter-tile margin */
            th_w = w; th_h = h;
            lv_obj_set_pos(th, x, y);
            lv_obj_set_size(th, w, h);
        } else if (L) {
            lv_obj_add_flag(th, LV_OBJ_FLAG_HIDDEN);
        }
    }
    th_is_compact = (th_h <= 210 || th_w <= 270);

    lv_obj_set_style_bg_color(th, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_border_width(th, 0, 0);
    lv_obj_set_style_radius(th, 18, 0);
    lv_obj_set_style_pad_all(th, th_is_compact ? 6 : 10, 0);
    lv_obj_clear_flag(th, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(th, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(th, open_thermostat, LV_EVENT_CLICKED, NULL);

    lbl_t_temp = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_temp, lv_color_hex(COL_TEMP_YELLOW), 0);
    lv_obj_set_style_text_font(lbl_t_temp, th_is_compact ? SF(36) : (DISP_VER < 600 ? SF(36) : SF(48)), 0);
    lv_label_set_text(lbl_t_temp, "--.-°C");
    lv_obj_align(lbl_t_temp, LV_ALIGN_TOP_MID, 0, (th_is_compact ? 2 : (DISP_VER < 600 ? 6 : 14)));

    /* Flame / burner indicator sitting horizontally right next to the temp value */
    tile_img_flame = lv_img_create(th);
    lv_img_set_src(tile_img_flame, &icon_radiator);
    lv_img_set_zoom(tile_img_flame, (th_is_compact ? 180 : (DISP_VER < 600 ? 200 : 256)));
    lv_obj_set_style_img_recolor(tile_img_flame, lv_color_hex(COL_BURNER_RED), 0);
    lv_obj_set_style_img_recolor_opa(tile_img_flame, 255, 0);
    lv_obj_align_to(tile_img_flame, lbl_t_temp, LV_ALIGN_OUT_RIGHT_MID, th_is_compact ? 4 : 8, 0);
    lv_obj_add_flag(tile_img_flame, LV_OBJ_FLAG_HIDDEN);

    tile_img_faucet = lv_img_create(th);
    lv_img_set_src(tile_img_faucet, &icon_faucet);
    lv_img_set_zoom(tile_img_faucet, (th_is_compact ? 180 : (DISP_VER < 600 ? 200 : 256)));
    lv_obj_set_style_img_recolor(tile_img_faucet, lv_color_hex(0xff5544), 0);
    lv_obj_set_style_img_recolor_opa(tile_img_faucet, 255, 0);
    lv_obj_align_to(tile_img_faucet, lbl_t_temp, LV_ALIGN_OUT_RIGHT_MID, th_is_compact ? 4 : 8, 0);
    lv_obj_add_flag(tile_img_faucet, LV_OBJ_FLAG_HIDDEN);

    tile_img_drop = lv_img_create(th);
    lv_img_set_src(tile_img_drop, &icon_drop);
    lv_img_set_zoom(tile_img_drop, (th_is_compact ? 180 : (DISP_VER < 600 ? 200 : 256)));
    lv_obj_set_style_img_recolor(tile_img_drop, lv_color_hex(0xff5544), 0);
    lv_obj_set_style_img_recolor_opa(tile_img_drop, 255, 0);
    lv_obj_align_to(tile_img_drop, lbl_t_temp, LV_ALIGN_OUT_RIGHT_MID, th_is_compact ? 20 : 26, th_is_compact ? 4 : 8);
    lv_obj_add_flag(tile_img_drop, LV_OBJ_FLAG_HIDDEN);

    /* Setpoint row: [-] setpoint [+] */
    lv_obj_t * sp_row = lv_obj_create(th);
    lv_obj_set_size(sp_row, LV_PCT(100), (th_is_compact ? 44 : (DISP_VER < 600 ? 64 : 78)));
    lv_obj_align(sp_row, LV_ALIGN_BOTTOM_MID, 0, (th_is_compact ? -2 : (DISP_VER < 600 ? -2 : -6)));
    lv_obj_set_style_bg_opa(sp_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp_row, 0, 0);
    lv_obj_set_style_pad_all(sp_row, 0, 0);
    lv_obj_clear_flag(sp_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sp_row, LV_OBJ_FLAG_CLICKABLE);

    int btn_sz_w = th_is_compact ? 48 : (DISP_VER < 600 ? 64 : 78);
    int btn_sz_h = th_is_compact ? 40 : (DISP_VER < 600 ? 58 : 70);

    lv_obj_t * btn_dn = lv_btn_create(sp_row);
    lv_obj_set_size(btn_dn, btn_sz_w, btn_sz_h);
    lv_obj_align(btn_dn, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(btn_dn, lv_color_hex(COL_TILE_ACCENT), 0);
    lv_obj_set_style_radius(btn_dn, th_is_compact ? 8 : 12, 0);
    lv_obj_set_ext_click_area(btn_dn, 12);
    lv_obj_add_event_cb(btn_dn, on_tile_sp_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_dn_lbl = lv_label_create(btn_dn);
    lv_label_set_text(btn_dn_lbl, "-");
    lv_obj_set_style_text_font(btn_dn_lbl, th_is_compact ? SF(28) : (DISP_VER < 600 ? SF(36) : SF(48)), 0);
    lv_obj_set_style_text_color(btn_dn_lbl, lv_color_hex(settings.home_light_mode ? 0x1e2838 : 0xffffff), 0);
    lv_obj_center(btn_dn_lbl);

    lbl_t_setpoint = lv_label_create(sp_row);
    lv_obj_set_style_text_color(lbl_t_setpoint, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_t_setpoint, th_is_compact ? SF(18) : (DISP_VER < 600 ? SF(20) : SF(24)), 0);
    lv_label_set_text(lbl_t_setpoint, "--.-°C");
    lv_obj_align(lbl_t_setpoint, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * btn_up = lv_btn_create(sp_row);
    lv_obj_set_size(btn_up, btn_sz_w, btn_sz_h);
    lv_obj_align(btn_up, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(btn_up, lv_color_hex(COL_TILE_ACCENT), 0);
    lv_obj_set_style_radius(btn_up, th_is_compact ? 8 : 12, 0);
    lv_obj_set_ext_click_area(btn_up, 12);
    lv_obj_add_event_cb(btn_up, on_tile_sp_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_up_lbl = lv_label_create(btn_up);
    lv_label_set_text(btn_up_lbl, "+");
    lv_obj_set_style_text_font(btn_up_lbl, th_is_compact ? SF(28) : (DISP_VER < 600 ? SF(36) : SF(48)), 0);
    lv_obj_set_style_text_color(btn_up_lbl, lv_color_hex(settings.home_light_mode ? 0x1e2838 : 0xffffff), 0);
    lv_obj_center(btn_up_lbl);

    /* Camera button if video enabled */
    video_install_button(th, btn_up);

    /* Live water-flow indicator */
    tile_img_water = lv_img_create(th);
    lv_img_set_src(tile_img_water, &icon_drop);
    lv_img_set_zoom(tile_img_water, 200);
    lv_obj_set_style_img_recolor(tile_img_water, lv_color_hex(settings.home_light_mode ? 0x0284c7 : 0x66bbff), 0);
    lv_obj_set_style_img_recolor_opa(tile_img_water, 255, 0);
    lv_obj_align(tile_img_water, LV_ALIGN_TOP_LEFT, 4, 10);
    lv_obj_add_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);

    tile_lbl_water = lv_label_create(th);
    lv_obj_set_style_text_color(tile_lbl_water, lv_color_hex(settings.home_light_mode ? 0x0284c7 : 0x66bbff), 0);
    lv_obj_set_style_text_font(tile_lbl_water, SF(14), 0);
    lv_label_set_text(tile_lbl_water, "");
    lv_obj_align_to(tile_lbl_water, tile_img_water, LV_ALIGN_OUT_RIGHT_MID, 4, 0);
    lv_obj_add_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);

    /* Pressure warning banner */
    pressure_banner = lv_obj_create(th);
    lv_obj_set_size(pressure_banner, LV_PCT(100), 32);
    lv_obj_align(pressure_banner, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(pressure_banner, 8, 0);
    lv_obj_set_style_border_width(pressure_banner, 0, 0);
    lv_obj_clear_flag(pressure_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pressure_banner, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(pressure_banner, LV_OBJ_FLAG_HIDDEN);

    pressure_banner_lbl = lv_label_create(pressure_banner);
    lv_obj_set_style_text_color(pressure_banner_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(pressure_banner_lbl, SF(16), 0);
    lv_label_set_text(pressure_banner_lbl, "");
    lv_obj_center(pressure_banner_lbl);
    lv_obj_add_flag(pressure_banner_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Clean up unused legacy tile widgets */
    tile_btn_mode_manual  = NULL;
    tile_btn_mode_program = NULL;
    for (int i = 0; i < 4; i++) tile_btn_preset[i] = NULL;
    lbl_t_burner = NULL;
    lbl_t_humidity = NULL;
    lbl_t_ppm = NULL;
    lbl_t_tvoc = NULL;
    lbl_t_pressure = NULL;
    lbl_t_aq = NULL;

    /* --- Waste tile: two stacked pickup rows ---
       Each row gets its own type-specific icon (newspaper for Papier,
       milk carton for Plastic/PMD, leaf for GFT, trashcan fallback) so
       the user can read at a glance what's coming when. */
    tile_t waste_big;
    make_tile(scr_root, 560, 20, 220, 200, LT_WASTE, tr("Afval", "Waste"), 0x88dd66,
              open_placeholder, &waste_big);
    tile_waste = waste_big.tile;

#ifdef TOON1
    waste_icon_1 = NULL;
    lbl_waste_date = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_date, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_waste_date, SF(24), 0);
    lv_label_set_text(lbl_waste_date, "--");
    lv_obj_align(lbl_waste_date, LV_ALIGN_TOP_LEFT, 4, 38);

    lbl_waste_type = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_type, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_type, SF(16), 0);
    lv_label_set_text(lbl_waste_type, "");
    lv_obj_align(lbl_waste_type, LV_ALIGN_TOP_LEFT, 4, 68);

    waste_icon_2 = NULL;
    lbl_waste_date_2 = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_date_2, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_date_2, SF(20), 0);
    lv_label_set_text(lbl_waste_date_2, "");
    lv_obj_align(lbl_waste_date_2, LV_ALIGN_BOTTOM_LEFT, 4, -28);

    lbl_waste_type_2 = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_type_2, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_type_2, SF(14), 0);
    lv_label_set_text(lbl_waste_type_2, "");
    lv_obj_align(lbl_waste_type_2, LV_ALIGN_BOTTOM_LEFT, 4, -8);
#else
    waste_icon_1 = lv_img_create(waste_big.tile);
    lv_img_set_src(waste_icon_1, &icon_trash);
    lv_obj_set_style_img_recolor_opa(waste_icon_1, 255, 0);
    lv_obj_align(waste_icon_1, LV_ALIGN_TOP_LEFT, 6, 42);
    lv_obj_add_flag(waste_icon_1, LV_OBJ_FLAG_EVENT_BUBBLE);

    lbl_waste_date = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_date, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_waste_date, SF(28), 0);
    lv_label_set_text(lbl_waste_date, "--");
    lv_obj_align(lbl_waste_date, LV_ALIGN_TOP_LEFT, 56, 44);

    lbl_waste_type = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_type, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_type, SF(18), 0);
    lv_label_set_text(lbl_waste_type, "");
    lv_obj_align(lbl_waste_type, LV_ALIGN_TOP_LEFT, 56, 78);

    waste_icon_2 = lv_img_create(waste_big.tile);
    lv_img_set_src(waste_icon_2, &icon_trash);
    lv_obj_set_style_img_recolor_opa(waste_icon_2, 255, 0);
    lv_obj_align(waste_icon_2, LV_ALIGN_BOTTOM_LEFT, 6, -36);
    lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);

    lbl_waste_date_2 = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_date_2, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_date_2, SF(22), 0);
    lv_label_set_text(lbl_waste_date_2, "");
    lv_obj_align(lbl_waste_date_2, LV_ALIGN_BOTTOM_LEFT, 56, -30);

    lbl_waste_type_2 = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_type_2, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_type_2, SF(14), 0);
    lv_label_set_text(lbl_waste_type_2, "");
    lv_obj_align(lbl_waste_type_2, LV_ALIGN_BOTTOM_LEFT, 56, -8);

    waste_icon_3 = lv_img_create(waste_big.tile);
    lv_img_set_src(waste_icon_3, &icon_trash);
    lv_obj_set_style_img_recolor_opa(waste_icon_3, 255, 0);
    lv_obj_align(waste_icon_3, LV_ALIGN_BOTTOM_LEFT, 6, -36);
    lv_obj_add_flag(waste_icon_3, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(waste_icon_3, LV_OBJ_FLAG_HIDDEN);

    lbl_waste_date_3 = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_date_3, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_date_3, SF(22), 0);
    lv_label_set_text(lbl_waste_date_3, "");
    lv_obj_align(lbl_waste_date_3, LV_ALIGN_BOTTOM_LEFT, 56, -30);
    lv_obj_add_flag(lbl_waste_date_3, LV_OBJ_FLAG_HIDDEN);

    lbl_waste_type_3 = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_type_3, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_type_3, SF(14), 0);
    lv_label_set_text(lbl_waste_type_3, "");
    lv_obj_align(lbl_waste_type_3, LV_ALIGN_BOTTOM_LEFT, 56, -8);
    lv_obj_add_flag(lbl_waste_type_3, LV_OBJ_FLAG_HIDDEN);
#endif

    /* Live Energy tile (replaces the old Humidity tile — humidity is now
       on the Heater bottom strip). Big live power on top, gas total below,
       today's kWh in the corner. */
    tile_t energy_t;
    make_tile(scr_root, 790, 20, 214, 130, LT_ENERGY, tr("Energie", "Energy"), 0xaa77ff,
              open_stats, &energy_t);
    tile_energy = energy_t.tile;
    lbl_title_slot[TILE_SLOT_ENERGY] = energy_t.title;
    slot_agenda[0] = make_slot_agenda(tile_energy, 0x4477cc);
    slot_sonos[0]  = make_sonos_view(tile_energy);
    slot_waste[0]  = make_slot_waste(tile_energy, 0x1e3a24);
    slot_templogger[0] = make_slot_templogger(tile_energy);
    if (slot_sonos[0]) lv_obj_add_flag(slot_sonos[0]->root, LV_OBJ_FLAG_HIDDEN);
    /* Compressed for the new 130-px tile: 28-pt W value (was 48), gas
     * total on the same baseline range below. */
    lbl_energy_w = lv_label_create(energy_t.tile);
    lv_obj_set_style_text_color(lbl_energy_w, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_energy_w, SF(28), 0);
    lv_label_set_text(lbl_energy_w, "-- W");
    /* +6 sits the big number just below the tile centre — clears the
     * "Energy" header more cleanly than the prior -8. */
    lv_obj_align(lbl_energy_w, LV_ALIGN_CENTER, 0, 6);

    lbl_energy_gas = lv_label_create(energy_t.tile);
    lv_obj_set_style_text_color(lbl_energy_gas, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_energy_gas, SF(18), 0);
    lv_label_set_text(lbl_energy_gas, "");
    lv_obj_add_flag(lbl_energy_gas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(lbl_energy_gas, LV_ALIGN_BOTTOM_LEFT, 0, -4);

    lbl_energy_today = lv_label_create(energy_t.tile);
    lv_obj_set_style_text_color(lbl_energy_today, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_energy_today, SF(14), 0);
    lv_label_set_text(lbl_energy_today, "");
    lv_obj_align(lbl_energy_today, LV_ALIGN_TOP_RIGHT, -4, 4);

    /* NILM transient event — device name shown briefly above the watt number. */
    lbl_nilm_event = lv_label_create(energy_t.tile);
    lv_obj_set_style_text_color(lbl_nilm_event, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_nilm_event, SF(11), 0);
    lv_label_set_text(lbl_nilm_event, "");
    lv_obj_align(lbl_nilm_event, LV_ALIGN_CENTER, 0, -22);
    lv_obj_add_flag(lbl_nilm_event, LV_OBJ_FLAG_HIDDEN);

    /* Vent tile — spinning fan in centre, manual +/- on the sides, live
       % above and rpm below the fan. Tap on the fan itself opens the remote
       (the buttons get their own click handlers so they don't bubble). */
    tile_t vent;
    make_tile(scr_root, 560, 230, 220, 200, LT_VENT, tr("Ventilatie", "Vent"), 0x66bbdd,
              (lv_event_cb_t)open_vent, &vent);
    tile_vent = vent.tile;
    lbl_title_slot[TILE_SLOT_VENT] = vent.title;
    slot_agenda[2] = make_slot_agenda(tile_vent, 0x4477cc);
    slot_sonos[2]  = make_sonos_view(tile_vent);
    slot_waste[2]  = make_slot_waste(tile_vent, 0x1e3a24);
    slot_templogger[2] = make_slot_templogger(tile_vent);
    if (slot_sonos[2]) lv_obj_add_flag(slot_sonos[2]->root, LV_OBJ_FLAG_HIDDEN);

    /* Speed % shares the title row with the "Vent" label so the
       top-row buttons can sit cleanly below without overlap. */
    lbl_boiler_state = lv_label_create(vent.tile);
    lv_obj_set_style_text_color(lbl_boiler_state, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_boiler_state, SF(18), 0);
    lv_label_set_text(lbl_boiler_state, "-- %");
    lv_obj_align(lbl_boiler_state, LV_ALIGN_TOP_RIGHT, -4, 14);   /* font auto-fit at refresh */

    /* Spinning fan: 80x80 TRUE_COLOR_ALPHA icon (color baked in). Rotated
       directly with lv_img_set_angle — works reliably on this format.
       Scaled to ~50px (zoom 160) and parked dead-centre so it has clear
       space below it for the 2-line % / rpm / source label without any
       overlap on either side. */
    vent_fan_img = lv_img_create(vent.tile);
    lv_img_set_src(vent_fan_img, &icon_fan);
    lv_img_set_pivot(vent_fan_img, 40, 40);
    lv_img_set_zoom(vent_fan_img, 160);            /* 160/256 ≈ 62 % */
    lv_obj_align(vent_fan_img, LV_ALIGN_CENTER, 0, -6);
    lv_obj_add_flag(vent_fan_img, LV_OBJ_FLAG_EVENT_BUBBLE);  /* tap → tile */

    /* Four corner buttons — Low / High at top, Auto / Timer at bottom.
       Each issues an Itho-Wifi virtual-remote command (vremotecmd=…).
       Timer cycles 10/20/30 min and resets to "auto". */
    /* Buttons send user-intent commands ("low"/"high"). ventilation.c
     * applies the wire-side swap (VENT_SWAP_LOW_HIGH) and translates
     * FanInfo back, so UI code deals only in intuitive labels. */
    struct { lv_align_t a; int x, y; uint32_t col; const char * cmd;
             const char * txt; lv_event_cb_t cb; } btn[] = {
        { LV_ALIGN_TOP_LEFT,     4,  46, 0x224d70, "low",  "Low",   on_vent_mode  },
        { LV_ALIGN_TOP_RIGHT,   -4,  46, 0x804030, "high", "High",  on_vent_mode  },
        { LV_ALIGN_BOTTOM_LEFT,  4,  -4, 0x2e6e3a, "auto", "Auto",  on_vent_mode  },
        { LV_ALIGN_BOTTOM_RIGHT,-4,  -4, 0x6a5424, NULL,   "Timer", on_vent_timer },
    };
    for (size_t i = 0; i < sizeof(btn)/sizeof(btn[0]); i++) {
        lv_obj_t * b = lv_btn_create(vent.tile);
        lv_obj_set_size(b, 60, 28);
        lv_obj_align(b, btn[i].a, btn[i].x, btn[i].y);
        lv_obj_set_style_bg_color(b, lv_color_hex(btn[i].col), 0);
        lv_obj_set_style_radius(b, 8, 0);
        /* Pre-style border to 0 so the refresh-side toggle is a single
         * width change (color always white when shown). */
        lv_obj_set_style_border_color(b, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_add_event_cb(b, btn[i].cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(uintptr_t)btn[i].cmd);
        lv_obj_t * bl = lv_label_create(b);
        lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(bl, SF(14), 0);
        /* btn[i].txt stays the stable English key for the strcmp() ref
         * matching below; only the visible label is translated. */
        const char * disp = btn[i].txt;
        if      (strcmp(disp, "Low")   == 0) disp = tr("Laag",  "Low");
        else if (strcmp(disp, "High")  == 0) disp = tr("Hoog",  "High");
        else if (strcmp(disp, "Auto")  == 0) disp = tr("Auto",  "Auto");
        else if (strcmp(disp, "Timer") == 0) disp = tr("Timer", "Timer");
        lv_label_set_text(bl, disp);
        lv_obj_center(bl);
        if (btn[i].cb == on_vent_timer) vent_timer_lbl = bl;
        /* Refs keyed by the visible label, not the cmd — the two are inverted
         * for low/high on this Itho. */
        if      (strcmp(btn[i].txt, "Low")   == 0) vent_btn_low   = b;
        else if (strcmp(btn[i].txt, "High")  == 0) vent_btn_high  = b;
        else if (strcmp(btn[i].txt, "Auto")  == 0) vent_btn_auto  = b;
        else if (btn[i].cb == on_vent_timer)       vent_btn_timer = b;
    }

    /* % + rpm + source on two lines just above the Auto/Timer buttons.
       Two lines so the per-line width stays narrow and the text doesn't
       have to overlap the fan icon. Brighter colour than the old DIM grey
       so it's actually readable against the navy tile. */
    lbl_boiler_pressure = lv_label_create(vent.tile);
    lv_obj_set_style_text_color(lbl_boiler_pressure, lv_color_hex(settings.home_light_mode ? 0x2b4f6e : 0xbbd6e8), 0);
    lv_obj_set_style_text_font(lbl_boiler_pressure, SF(14), 0);
    lv_obj_set_style_text_align(lbl_boiler_pressure, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_boiler_pressure, "--\n--");
    lv_obj_align(lbl_boiler_pressure, LV_ALIGN_BOTTOM_MID, 0, -38);

    /* Family tile — Life360 locations for the two tracked people. Sits
     * between the shrunken Energy and Water tiles in the right column. */
    tile_t family_t;
    make_tile(scr_root, 790, 160, 214, 130, LT_FAMILY, tr("Familie", "Family"), 0xff8866,
              open_family_map, &family_t);
    tile_family = family_t.tile;
    lbl_title_slot[TILE_SLOT_FAMILY] = family_t.title;
    slot_agenda[1] = make_slot_agenda(tile_family, 0x4477cc);
    slot_sonos[1]  = make_sonos_view(tile_family);
    slot_waste[1]  = make_slot_waste(tile_family, 0x1e3a24);
    slot_templogger[1] = make_slot_templogger(tile_family);
    if (slot_sonos[1]) lv_obj_add_flag(slot_sonos[1]->root, LV_OBJ_FLAG_HIDDEN);
    /* Two scrolling labels — the formatted address ("City > Street > Num")
     * almost always exceeds the 194-px tile width, so we use
     * SCROLL_CIRCULAR to slide the full string through the label area
     * with a short gap between repeats. Looks tidier than chopping with
     * an ellipsis and the user can read the entire address without
     * tapping into a detail view. */
    /* Person 1 — blue (matches dim screen's first row colour). The two
     * scrolling labels are coloured rather than name-prefixed; the colour
     * carries the identity so the scroll has full width for the address. */
    lbl_life360_a = lv_label_create(family_t.tile);
    lv_obj_set_style_text_color(lbl_life360_a, lv_color_hex(settings.home_light_mode ? 0x2563eb : 0x88aaff), 0);
    lv_obj_set_style_text_font(lbl_life360_a, SF(16), 0);
    lv_obj_set_width(lbl_life360_a, 194);
    lv_label_set_long_mode(lbl_life360_a, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(lbl_life360_a, "?");
    lv_obj_align(lbl_life360_a, LV_ALIGN_TOP_LEFT, 0, 40);

    /* Person 2 — pink (mirrors dim screen). */
    lbl_life360_b = lv_label_create(family_t.tile);
    lv_obj_set_style_text_color(lbl_life360_b, lv_color_hex(settings.home_light_mode ? 0xd946ef : 0xff88cc), 0);
    lv_obj_set_style_text_font(lbl_life360_b, SF(16), 0);
    lv_obj_set_width(lbl_life360_b, 194);
    lv_label_set_long_mode(lbl_life360_b, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(lbl_life360_b, "?");
    lv_obj_align(lbl_life360_b, LV_ALIGN_TOP_LEFT, 0, 68);

    /* Person 3 — green. */
    lbl_life360_c = lv_label_create(family_t.tile);
    lv_obj_set_style_text_color(lbl_life360_c, lv_color_hex(settings.home_light_mode ? 0x16a34a : 0x66cc88), 0);
    lv_obj_set_style_text_font(lbl_life360_c, SF(16), 0);
    lv_obj_set_width(lbl_life360_c, 194);
    lv_label_set_long_mode(lbl_life360_c, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(lbl_life360_c, "");
    lv_obj_align(lbl_life360_c, LV_ALIGN_TOP_LEFT, 0, 96);

    tile_t water_t;
    make_tile(scr_root, 790, 300, 214, 130, LT_WATER, tr("Water", "Water"), 0x44aaff, open_placeholder, &water_t);

    /* Custom-layout-only tiles: a news-summary panel and a calendar/agenda tile.
     * Created only in custom layout mode (make_tile positions/hides them from
     * the grid); content is filled in refresh_cb. */
    if (settings.custom_layout_enabled) {
        tile_t nt, ct;
        make_tile(scr_root, 0, 0, 200, 150, LT_NEWS_SUMMARY, tr("Nieuws", "News"), 0x6666aa, on_news_tap, &nt);
        tile_news_sum_lbl = lv_label_create(nt.tile);
        lv_obj_set_style_text_color(tile_news_sum_lbl, lv_color_hex(COL_TEXT_HI), 0);
        lv_obj_set_style_text_font(tile_news_sum_lbl, SF(14), 0);
        lv_label_set_long_mode(tile_news_sum_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(tile_news_sum_lbl, LV_PCT(94));
        lv_obj_align(tile_news_sum_lbl, LV_ALIGN_TOP_LEFT, 0, 38);
        lv_label_set_text(tile_news_sum_lbl, "...");

        make_tile(scr_root, 0, 0, 200, 150, LT_CALENDAR, tr("Agenda", "Calendar"), 0x4477cc, on_slot_agenda_tap, &ct);
        tile_cal_dates = lv_label_create(ct.tile);
        lv_obj_set_style_text_color(tile_cal_dates, lv_color_hex(settings.home_light_mode ? 0x1d4ed8 : 0xaad4f5), 0);
        lv_obj_set_style_text_font(tile_cal_dates, SF(14), 0);
        lv_label_set_long_mode(tile_cal_dates, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(tile_cal_dates, SX(38));
        lv_obj_align(tile_cal_dates, LV_ALIGN_TOP_LEFT, 0, SY(38));
        lv_obj_add_flag(tile_cal_dates, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile_cal_dates, on_slot_agenda_tap, LV_EVENT_CLICKED, NULL);
        lv_label_set_text(tile_cal_dates, "");

        tile_cal_times = lv_label_create(ct.tile);
        lv_obj_set_style_text_color(tile_cal_times, lv_color_hex(settings.home_light_mode ? 0x2563eb : 0x88ccff), 0);
        lv_obj_set_style_text_font(tile_cal_times, SF(14), 0);
        lv_label_set_long_mode(tile_cal_times, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(tile_cal_times, SX(38));
        lv_obj_align(tile_cal_times, LV_ALIGN_TOP_LEFT, SX(42), SY(38));
        lv_obj_add_flag(tile_cal_times, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile_cal_times, on_slot_agenda_tap, LV_EVENT_CLICKED, NULL);
        lv_label_set_text(tile_cal_times, "");

        tile_cal_titles = lv_label_create(ct.tile);
        lv_obj_set_style_text_color(tile_cal_titles, lv_color_hex(COL_TEXT_HI), 0);
        lv_obj_set_style_text_font(tile_cal_titles, SF(14), 0);
        lv_label_set_long_mode(tile_cal_titles, LV_LABEL_LONG_CLIP);
        lv_obj_align(tile_cal_titles, LV_ALIGN_TOP_LEFT, SX(86), SY(38));
        lv_obj_add_flag(tile_cal_titles, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile_cal_titles, on_slot_agenda_tap, LV_EVENT_CLICKED, NULL);
        lv_label_set_text(tile_cal_titles, "...");

        /* Lights: a placeable button tile that opens the lights screen (the same
         * backend as the retractable side handle). Hidden until added in the editor. */
        tile_t lt;
        make_tile(scr_root, 0, 0, 200, 150, LT_LIGHTS, tr("Verlichting", "Lighting"), 0xddaa44, on_lights_tile, &lt);
        tile_lights_lbl_default = lv_label_create(lt.tile);
        lv_obj_set_style_text_color(tile_lights_lbl_default, lv_color_hex(COL_TEXT_HI), 0);
        lv_obj_set_style_text_font(tile_lights_lbl_default, SF(28), 0);
        lv_label_set_text(tile_lights_lbl_default, tr("Lampen", "Lights"));
        lv_obj_align(tile_lights_lbl_default, LV_ALIGN_CENTER, 0, 6);

        lv_coord_t btn_h = (DISP_VER < 600) ? 32 : SY(38);
        lv_coord_t y1 = (DISP_VER < 600) ? 40 : SY(40);
        lv_coord_t y2 = (DISP_VER < 600) ? 76 : SY(84);
        tile_lights_btn1 = lv_btn_create(lt.tile);
        lv_obj_set_size(tile_lights_btn1, LV_PCT(92), btn_h);
        lv_obj_align(tile_lights_btn1, LV_ALIGN_TOP_MID, 0, y1);
        lv_obj_set_style_radius(tile_lights_btn1, 8, 0);
        lv_obj_set_style_bg_color(tile_lights_btn1, lv_color_hex(settings.home_light_mode ? 0xedf2f7 : 0x1e2838), 0);
        lv_obj_set_style_border_width(tile_lights_btn1, 0, 0);
        lv_obj_set_style_pad_hor(tile_lights_btn1, 6, 0);
        lv_obj_set_style_pad_ver(tile_lights_btn1, 0, 0);
        lv_obj_add_event_cb(tile_lights_btn1, on_tile_light_btn1, LV_EVENT_CLICKED, NULL);

        tile_lights_lbl1_name = lv_label_create(tile_lights_btn1);
        lv_obj_set_style_text_font(tile_lights_lbl1_name, SF(16), 0);
        lv_obj_set_style_text_color(tile_lights_lbl1_name, lv_color_hex(COL_TEXT_HI), 0);
        lv_label_set_long_mode(tile_lights_lbl1_name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(tile_lights_lbl1_name, LV_PCT(60));
        lv_obj_align(tile_lights_lbl1_name, LV_ALIGN_LEFT_MID, SX(2), 0);
        lv_label_set_text(tile_lights_lbl1_name, "");

        tile_lights_sw1 = lv_obj_create(tile_lights_btn1);
        lv_obj_set_size(tile_lights_sw1, SX(56), SY(28));
        lv_obj_align(tile_lights_sw1, LV_ALIGN_RIGHT_MID, SX(-2), 0);
        lv_obj_set_style_radius(tile_lights_sw1, 6, 0);
        lv_obj_set_style_border_width(tile_lights_sw1, 0, 0);
        lv_obj_set_style_bg_color(tile_lights_sw1, lv_color_hex(settings.home_light_mode ? 0xd0d8e2 : 0x283848), 0);
        lv_obj_set_style_pad_all(tile_lights_sw1, 0, 0);
        lv_obj_clear_flag(tile_lights_sw1, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile_lights_sw1, LV_OBJ_FLAG_EVENT_BUBBLE);

        tile_lights_lbl1_st = lv_label_create(tile_lights_sw1);
        lv_obj_set_style_text_font(tile_lights_lbl1_st, SF(15), 0);
        lv_obj_set_style_text_color(tile_lights_lbl1_st, lv_color_hex(settings.home_light_mode ? 0x485868 : 0x88aabb), 0);
        lv_obj_center(tile_lights_lbl1_st);
        lv_label_set_text(tile_lights_lbl1_st, "");
        lv_obj_add_flag(tile_lights_btn1, LV_OBJ_FLAG_HIDDEN);

        tile_lights_btn2 = lv_btn_create(lt.tile);
        lv_obj_set_size(tile_lights_btn2, LV_PCT(92), btn_h);
        lv_obj_align(tile_lights_btn2, LV_ALIGN_TOP_MID, 0, y2);
        lv_obj_set_style_radius(tile_lights_btn2, 8, 0);
        lv_obj_set_style_bg_color(tile_lights_btn2, lv_color_hex(settings.home_light_mode ? 0xedf2f7 : 0x1e2838), 0);
        lv_obj_set_style_border_width(tile_lights_btn2, 0, 0);
        lv_obj_set_style_pad_hor(tile_lights_btn2, 6, 0);
        lv_obj_set_style_pad_ver(tile_lights_btn2, 0, 0);
        lv_obj_add_event_cb(tile_lights_btn2, on_tile_light_btn2, LV_EVENT_CLICKED, NULL);

        tile_lights_lbl2_name = lv_label_create(tile_lights_btn2);
        lv_obj_set_style_text_font(tile_lights_lbl2_name, SF(16), 0);
        lv_obj_set_style_text_color(tile_lights_lbl2_name, lv_color_hex(COL_TEXT_HI), 0);
        lv_label_set_long_mode(tile_lights_lbl2_name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(tile_lights_lbl2_name, LV_PCT(60));
        lv_obj_align(tile_lights_lbl2_name, LV_ALIGN_LEFT_MID, SX(2), 0);
        lv_label_set_text(tile_lights_lbl2_name, "");

        tile_lights_sw2 = lv_obj_create(tile_lights_btn2);
        lv_obj_set_size(tile_lights_sw2, SX(56), SY(28));
        lv_obj_align(tile_lights_sw2, LV_ALIGN_RIGHT_MID, SX(-2), 0);
        lv_obj_set_style_radius(tile_lights_sw2, 6, 0);
        lv_obj_set_style_border_width(tile_lights_sw2, 0, 0);
        lv_obj_set_style_bg_color(tile_lights_sw2, lv_color_hex(settings.home_light_mode ? 0xd0d8e2 : 0x283848), 0);
        lv_obj_set_style_pad_all(tile_lights_sw2, 0, 0);
        lv_obj_clear_flag(tile_lights_sw2, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile_lights_sw2, LV_OBJ_FLAG_EVENT_BUBBLE);

        tile_lights_lbl2_st = lv_label_create(tile_lights_sw2);
        lv_obj_set_style_text_font(tile_lights_lbl2_st, SF(15), 0);
        lv_obj_set_style_text_color(tile_lights_lbl2_st, lv_color_hex(settings.home_light_mode ? 0x485868 : 0x88aabb), 0);
        lv_obj_center(tile_lights_lbl2_st);
        lv_label_set_text(tile_lights_lbl2_st, "");
        lv_obj_add_flag(tile_lights_btn2, LV_OBJ_FLAG_HIDDEN);

        /* Sonos tile in custom layout */
        tile_t sonos_t;
        make_tile(scr_root, 0, 0, 200, 150, LT_SONOS, "Sonos", 0x2077a0, on_sonos_tile_tap, &sonos_t);
        tile_sonos_view = make_sonos_view(sonos_t.tile);
    }
    tile_water = water_t.tile;
    lbl_title_slot[TILE_SLOT_WATER] = water_t.title;
    slot_agenda[3] = make_slot_agenda(tile_water, 0x4477cc);
    slot_sonos[3]  = make_sonos_view(tile_water);
    slot_waste[3]  = make_slot_waste(tile_water, 0x1e3a24);
    slot_templogger[3] = make_slot_templogger(tile_water);
    if (slot_sonos[3]) lv_obj_add_flag(slot_sonos[3]->root, LV_OBJ_FLAG_HIDDEN);

    /* Long-press on any of the four right-column tiles → tile-slots picker.
     * Same modal as Settings → Tiles. Cheap to attach; the handler only
     * fires when LVGL's long-press timer expires (default 400 ms), so a
     * normal tap still routes through the existing CLICKED handler. */
    lv_obj_add_event_cb(tile_energy, on_tile_longpress, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(tile_family, on_tile_longpress, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(tile_vent,   on_tile_longpress, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(tile_water,  on_tile_longpress, LV_EVENT_LONG_PRESSED, NULL);
    lbl_inbox_main = lv_label_create(water_t.tile);
    lv_obj_set_style_text_color(lbl_inbox_main, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_inbox_main, SF(28), 0);
    lv_label_set_text(lbl_inbox_main, "-- m3");
    /* +6 matches the Energy tile so the two big numbers line up vertically. */
    lv_obj_align(lbl_inbox_main, LV_ALIGN_CENTER, 0, 6);
    lbl_inbox_sub = lv_label_create(water_t.tile);
    lv_obj_set_style_text_color(lbl_inbox_sub, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_inbox_sub, SF(18), 0);
    lv_label_set_text(lbl_inbox_sub, "-- L/min");
    lv_obj_align(lbl_inbox_sub, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* Spinner overlay: visible only while water is flowing. lv_spinner draws
       a continuously-rotating arc that reads as "something is moving". */
    water_spinner = lv_spinner_create(water_t.tile, 1200, 80);
    lv_obj_set_size(water_spinner, 48, 48);
    lv_obj_align(water_spinner, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_arc_color(water_spinner, lv_color_hex(0x223344), LV_PART_MAIN);
    lv_obj_set_style_arc_color(water_spinner, lv_color_hex(0x44aaff), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(water_spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(water_spinner, 6, LV_PART_INDICATOR);
    lv_obj_add_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);

    /* ---- Page-1 tile overlay (revealed by swiping the tile zone left) ----
     * Four configurable slots; tapping one opens the tile-slots picker so the
     * user can map any integration onto it. Sits on top of the tile zone and
     * is hidden until paged in. */
    home_page1 = lv_obj_create(scr_root);
    lv_obj_set_style_border_width(home_page1, 0, 0);
    lv_obj_set_style_pad_all(home_page1, 0, 0);
    lv_obj_clear_flag(home_page1, LV_OBJ_FLAG_SCROLLABLE);
    /* Start hidden so a full-screen+opaque custom page 2 never paints over the
     * thermostat on first show (BUG 8). home_show_page(0) below re-asserts this,
     * but a custom page-2 tile that overlaps the left zone is visible during the
     * rest of screen build until then — hide it up-front. */
    lv_obj_add_flag(home_page1, LV_OBJ_FLAG_HIDDEN);
    {
        /* Custom layout: render page 2 as the user's grid of assignable slots,
         * full-screen + opaque so it cleanly covers the thermostat/weather when
         * paged in. Otherwise: the original right-zone 2x2 of fixed slots. */
        const layout_tile_t * p1t[TILE_SLOT_P1_N];
        int np1 = 0;
        if (settings.custom_layout_enabled)
            for (int i = 0; i < g_layout.count && np1 < TILE_SLOT_P1_N; i++)
                if (g_layout.tiles[i].page == 1 && g_layout.tiles[i].visible)
                    p1t[np1++] = &g_layout.tiles[i];

        /* per-slot geometry + colour */
        int sx[TILE_SLOT_P1_N], sy[TILE_SLOT_P1_N], sw[TILE_SLOT_P1_N], sh[TILE_SLOT_P1_N];
        static const uint32_t CYC[6] = { 0x88dd66, 0xaa77ff, 0x66bbdd, 0xff8866, 0x44aaff, 0xddaa44 };

        if (np1 > 0) {                       /* custom page 2 */
            lv_obj_set_pos(home_page1, 0, 0);
            lv_obj_set_size(home_page1, LV_HOR_RES, LV_VER_RES);
            lv_obj_set_style_bg_color(home_page1, lv_color_hex(COL_BG), 0);
            lv_obj_set_style_bg_opa(home_page1, LV_OPA_COVER, 0);
            g_p1_count = np1;
            for (int i = 0; i < np1; i++) {
                int x, y, w, h;
                layout_cell_px(p1t[i]->col, p1t[i]->row, p1t[i]->w, p1t[i]->h, &x, &y, &w, &h);
                sx[i] = x + 4; sy[i] = y + 4; sw[i] = w - 8; sh[i] = h - 8;
            }
        } else {                             /* default right-zone 2x2 */
            lv_obj_set_pos(home_page1, SX(556), SY(16));
            lv_obj_set_size(home_page1, SX(456), SY(418));
            lv_obj_set_style_bg_opa(home_page1, LV_OPA_TRANSP, 0);
            g_p1_count = 4;
            static const int dx[4] = { 4, 234, 4, 234 }, dy[4] = { 4, 4, 214, 214 };
            for (int i = 0; i < 4; i++) { sx[i] = SX(dx[i]); sy[i] = SY(dy[i]); sw[i] = SX(214); sh[i] = SY(196); }
        }

        for (int i = 0; i < g_p1_count; i++) {
            tile_t s;
            make_tile(home_page1, sx[i], sy[i], sw[i], sh[i], LT_NONE,
                      "", CYC[i % 6], on_page1_slot, &s);
            p1_title[i] = s.title;   /* make_tile's title label, reused */
            /* Short, wide block → lay title + value horizontally on one row (like
             * the curtains strip); taller block → stacked with a value font that
             * scales to the height (and room for the subtitle below). */
            int horiz = sh[i] < 110;
            const lv_font_t * vf = horiz       ? SF(22)
                                 : sh[i] >= 230 ? SF(48)
                                 : sh[i] >= 130 ? SF(28)
                                                : SF(22);
            p1_main[i] = lv_label_create(s.tile);
            lv_obj_set_style_text_color(p1_main[i], lv_color_hex(COL_TEXT_HI), 0);
            lv_obj_set_style_text_font(p1_main[i], vf, 0);
            lv_label_set_text(p1_main[i], tr(LV_SYMBOL_PLUS "  Tik om te koppelen", LV_SYMBOL_PLUS "  Tap to assign"));
            if (horiz) {
                lv_obj_align(p1_title[i], LV_ALIGN_LEFT_MID, 0, 0);   /* title left-of-centre */
                lv_obj_align(p1_main[i],  LV_ALIGN_RIGHT_MID, -4, 0); /* value right, same line */
            } else {
                lv_obj_align(p1_main[i], LV_ALIGN_CENTER, 0, sh[i] >= 130 ? -8 : 0);
            }
            p1_sub[i] = lv_label_create(s.tile);
            lv_obj_set_style_text_color(p1_sub[i], lv_color_hex(COL_TEXT_DIM), 0);
            lv_obj_set_style_text_font(p1_sub[i], SF(18), 0);
            lv_label_set_text(p1_sub[i], "");
            if (horiz) lv_obj_add_flag(p1_sub[i], LV_OBJ_FLAG_HIDDEN);   /* no room on one row */
            else lv_obj_align(p1_sub[i], LV_ALIGN_BOTTOM_MID, 0, -4);

            lv_obj_add_event_cb(s.tile, on_page1_slot_longpress, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)(4 + i));
        }
    }

    /* Pagination dots, centred in the gap under the tile zone. */
    for (int i = 0; i < 2; i++) {
        home_dot[i] = lv_obj_create(scr_root);
        lv_obj_set_size(home_dot[i], 10, 10);
        lv_obj_set_pos(home_dot[i], SX(966 + i * 18), SY(461));   /* on the location row, right — off the ticker */
        lv_obj_set_style_radius(home_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(home_dot[i], lv_color_hex(settings.home_light_mode ? 0x1e2838 : 0xffffff), 0);
        lv_obj_set_style_border_width(home_dot[i], 0, 0);
        lv_obj_clear_flag(home_dot[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    home_show_page(0);

    /* --- Pinned device quick-tiles — replaces the old fixed curtain/blinds
           strips. Renders the devices flagged pin_home in the device manager
           (Settings -> Devices), stacked below the thermostat tile. Only in
           the default layout; the custom-layout grid uses its own tiles + the
           LT_LIGHTS tile, so hardcoded strips would collide with the grid.
           Moved to the foreground at the end of create() so they sit above
           the later-built forecast strip. --- */
    home_pin_count = 0;
    if (!settings.custom_layout_enabled) {
        int maxn = (DISP_VER >= 560) ? HOME_PIN_MAX : 2;
        for (int i = 0; i < ha_device_count && home_pin_count < maxn; i++)
            if (ha_devices[i].pin_home)
                build_pin_tile(scr_root, i, home_pin_count);
    }

    /* City header above the forecast — labels the strip and shows the
     * current outside temperature so the user immediately sees where the
     * data is coming from. Refreshed from settings.weather_location and
     * weather_state.current_temp by refresh_cb. */
    lbl_forecast_city = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_forecast_city, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_forecast_city, SF(18), 0);
    lv_label_set_text(lbl_forecast_city, settings.weather_location);
    /* Location sits BELOW the ticker, as the forecast's header line. Shifted
       right of x=22 to leave room for the night-moon glyph that precedes it. */
    lv_obj_align(lbl_forecast_city, LV_ALIGN_TOP_LEFT, 54, 457);

    /* News ticker — in custom layout, rendered inside its own tile box adhering
     * to the grid cell bounds (width & height); in default layout, full-width strip
     * at the top of the bottom band. Tap opens the headline list + QR. */
    int ntx = SX(12), nty = SY(432), ntw = SX(1000), nth = SY(36);
    int custom_nt = 0;
    int nt_show = settings.news_enabled;
    if (settings.custom_layout_enabled) {
        const layout_tile_t * Lt = layout_find(LT_NEWS_TICKER);
        if (!Lt || !Lt->visible) {
            nt_show = 0;
        } else {
            layout_cell_px(Lt->col, Lt->row, Lt->w, Lt->h, &ntx, &nty, &ntw, &nth);
            ntx += 4; nty += 4; ntw -= 8; nth -= 8;     /* inter-tile margin */
            custom_nt = 1;
        }
    }

    if (custom_nt) {
        news_box = lv_obj_create(scr_root);
        lv_obj_set_pos(news_box, ntx, nty);
        lv_obj_set_size(news_box, ntw, nth);
        lv_obj_set_style_bg_color(news_box, lv_color_hex(COL_TILE_BG), 0);
        lv_obj_set_style_radius(news_box, 14, 0);
        lv_obj_set_style_border_width(news_box, 0, 0);
        lv_obj_set_style_pad_all(news_box, 0, 0);
        lv_obj_clear_flag(news_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(news_box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(news_box, on_news_tap, LV_EVENT_CLICKED, NULL);

        /* Accent bar on top */
        lv_obj_t * bar = lv_obj_create(news_box);
        lv_obj_set_size(bar, ntw > 20 ? ntw - 20 : ntw, 4);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x666688), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0);

        /* Title */
        lv_obj_t * tlbl = lv_label_create(news_box);
        lv_obj_set_style_text_color(tlbl, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(tlbl, SF(15), 0);
        lv_label_set_text(tlbl, tr("Nieuws", "News"));

        /* News ticker text scrolling within tile width */
        news_ticker = lv_label_create(news_box);
        lv_label_set_long_mode(news_ticker, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_speed(news_ticker,
            settings.news_scroll_speed >= 30 ? settings.news_scroll_speed : 30, 0);
        lv_obj_set_style_text_color(news_ticker, lv_color_hex(COL_TEXT_HI), 0);
        lv_label_set_text(news_ticker, "");
        lv_obj_add_flag(news_ticker, LV_OBJ_FLAG_EVENT_BUBBLE);

        if (nth < 56) {
            lv_obj_align(tlbl, LV_ALIGN_LEFT_MID, 10, 0);
            lv_obj_align(news_ticker, LV_ALIGN_LEFT_MID, 75, 0);
            lv_obj_set_width(news_ticker, ntw > 85 ? ntw - 85 : ntw);
            lv_obj_set_style_text_font(news_ticker, SF(16), 0);
        } else {
            lv_obj_align(tlbl, LV_ALIGN_TOP_LEFT, 10, 8);
            lv_obj_align(news_ticker, LV_ALIGN_TOP_LEFT, 10, nth >= 80 ? 38 : 28);
            lv_obj_set_width(news_ticker, ntw > 20 ? ntw - 20 : ntw);
            lv_obj_set_style_text_font(news_ticker, nth >= 80 ? SF(20) : SF(16), 0);
        }

        if (!nt_show) lv_obj_add_flag(news_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        news_box = NULL;
        news_ticker = lv_label_create(scr_root);
        lv_label_set_long_mode(news_ticker, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_speed(news_ticker,
            settings.news_scroll_speed >= 30 ? settings.news_scroll_speed : 30, 0);
        lv_obj_set_width(news_ticker, SX(1000));
        lv_obj_align(news_ticker, LV_ALIGN_TOP_LEFT, SX(12), SY(432));
        lv_obj_set_style_text_color(news_ticker, lv_color_hex(settings.home_light_mode ? 0x1e2838 : 0xcfe0f0), 0);
        lv_obj_set_style_text_font(news_ticker, SF(18), 0);
        lv_label_set_text(news_ticker, "");
        lv_obj_add_flag(news_ticker, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(news_ticker, 12);
        lv_obj_add_event_cb(news_ticker, on_news_tap, LV_EVENT_CLICKED, NULL);
        if (!nt_show) lv_obj_add_flag(news_ticker, LV_OBJ_FLAG_HIDDEN);
    }

    /* --- Forecast band — fills the area below the upper-row tiles.
           5 day columns; each shows day label + min/max temp + a big
           weather icon. Shifted down 14 px to make room for the city
           header above; height shrunk to match so the bottom edge stays
           flush with the screen. --- */
    forecast_box = lv_obj_create(scr_root);
    lv_obj_set_style_bg_color(forecast_box, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_radius(forecast_box, 12, 0);
    lv_obj_set_style_border_width(forecast_box, 0, 0);
    lv_obj_set_style_pad_all(forecast_box, 6, 0);
    lv_obj_clear_flag(forecast_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(forecast_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(forecast_box, open_forecast, LV_EVENT_CLICKED, NULL);

    /* Geometry: the native full-width bottom strip, unless a custom layout
     * places the forecast tile elsewhere — then take its grid rect and SCALE
     * the day-columns/icons/text to fit (so a half-screen forecast doesn't clip
     * its icons off the edge). The city header is anchored to the native strip,
     * so it's hidden whenever the forecast is grid-placed. */
    int fbx = SX(10), fby = SY(480), fbw = SX(1004), fbh = SY(116);
    int fc_show = 1;
    if (settings.custom_layout_enabled) {
        const layout_tile_t * Lf = layout_find(LT_FORECAST);
        if (!Lf || !Lf->visible) fc_show = 0;
        else {
            layout_cell_px(Lf->col, Lf->row, Lf->w, Lf->h, &fbx, &fby, &fbw, &fbh);
            fbx += 4; fby += 4; fbw -= 8; fbh -= 8;     /* small inter-tile gap */
            if (lbl_forecast_city) lv_obj_add_flag(lbl_forecast_city, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_set_size(forecast_box, fbw, fbh);
    lv_obj_set_pos(forecast_box, fbx, fby);
    if (!fc_show) {
        lv_obj_add_flag(forecast_box, LV_OBJ_FLAG_HIDDEN);
        if (lbl_forecast_city) lv_obj_add_flag(lbl_forecast_city, LV_OBJ_FLAG_HIDDEN);
    }
    {
        /* Scale relative to the native column (200x104 inside the strip); never
         * upscale, and floor at 0.30 so it stays legible at tiny sizes. */
        int pad = 6;
        int inner_w = fbw - 2 * pad; if (inner_w < WEATHER_FORECAST_DAYS * 16) inner_w = WEATHER_FORECAST_DAYS * 16;
        int inner_h = fbh - 2 * pad; if (inner_h < 36) inner_h = 36;
        int col_w = inner_w / WEATHER_FORECAST_DAYS;
        float sh = inner_h / 104.0f;
        float hscale = sh < 1.0f ? sh : 1.0f;
        if (hscale < 0.5f) hscale = 0.5f;

        const lv_font_t * f_day  = font_for((int)(18 * hscale));
        const lv_font_t * f_temp = font_for((int)(20 * hscale));
        const lv_font_t * f_wind = font_for((int)(18 * hscale));
        int dy = (int)(2 * hscale);

        /* Weather columns: day/date on row 1, min/max temp on row 2,
         * weather icon centered, wind direction arrow + speed at bottom. */
        for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
            lv_obj_t * col = lv_obj_create(forecast_box);
            lv_obj_set_size(col, col_w - 4, inner_h);
            lv_obj_set_pos(col, i * col_w + 2, 0);
            lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(col, 0, 0);
            lv_obj_set_style_pad_all(col, 0, 0);
            lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);

            fc_day_lbl[i] = lv_label_create(col);
            lv_obj_set_style_text_color(fc_day_lbl[i], lv_color_hex(COL_TEXT_HI), 0);
            lv_obj_set_style_text_font(fc_day_lbl[i], f_day, 0);
            lv_label_set_text(fc_day_lbl[i], "--");
            lv_obj_align(fc_day_lbl[i], LV_ALIGN_TOP_MID, 0, 0);

            fc_temp_lbl[i] = lv_label_create(col);
            lv_obj_set_style_text_color(fc_temp_lbl[i], lv_color_hex(COL_TEMP_YELLOW), 0);
            lv_obj_set_style_text_font(fc_temp_lbl[i], f_temp, 0);
            lv_label_set_text(fc_temp_lbl[i], "");
            lv_obj_align(fc_temp_lbl[i], LV_ALIGN_TOP_MID, 0, (int)(20 * hscale));

            /* Big weather icon centered in the column */
            fc_icon[i] = lv_img_create(col);
            lv_obj_set_style_img_recolor(fc_icon[i], lv_color_hex(settings.home_light_mode ? 0x64748b : 0xc8d4e0), 0);
            lv_obj_set_style_img_recolor_opa(fc_icon[i], 255, 0);
            lv_img_set_src(fc_icon[i], weather_icon_for_lg("d"));   /* default cloud */
            lv_obj_align(fc_icon[i], LV_ALIGN_CENTER, 0, (int)(10 * hscale));
            lv_obj_add_flag(fc_icon[i], LV_OBJ_FLAG_EVENT_BUBBLE);

            /* Sun overlay for partly-cloudy codes */
            fc_icon_sun[i] = lv_img_create(col);
            lv_img_set_src(fc_icon_sun[i], weather_icon_for("a"));
            lv_img_set_zoom(fc_icon_sun[i], 180);   /* 70 % of cloud size */
            lv_obj_set_style_img_recolor(fc_icon_sun[i], lv_color_hex(settings.home_light_mode ? 0xd97706 : 0xffd24a), 0);
            lv_obj_set_style_img_recolor_opa(fc_icon_sun[i], 255, 0);
            lv_obj_align_to(fc_icon_sun[i], fc_icon[i], LV_ALIGN_TOP_LEFT, -6, -6);
            lv_obj_add_flag(fc_icon_sun[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fc_icon_sun[i], LV_OBJ_FLAG_EVENT_BUBBLE);

            fc_wind_lbl[i] = lv_label_create(col);
            lv_obj_set_style_text_color(fc_wind_lbl[i], lv_color_hex(COL_TEXT_DIM), 0);
            lv_obj_set_style_text_font(fc_wind_lbl[i], f_wind, 0);
            lv_label_set_text(fc_wind_lbl[i], "");
            lv_obj_align(fc_wind_lbl[i], LV_ALIGN_BOTTOM_MID, 16, -dy);

            /* Wind-direction arrow next to the wind label */
            fc_wind_arrow[i] = lv_img_create(col);
            lv_img_set_src(fc_wind_arrow[i], &icon_wind_arrow);
            lv_img_set_pivot(fc_wind_arrow[i], 16, 16);
            lv_obj_set_style_img_recolor(fc_wind_arrow[i], lv_color_hex(COL_TEXT_DIM), 0);
            lv_obj_set_style_img_recolor_opa(fc_wind_arrow[i], 255, 0);
            lv_obj_align(fc_wind_arrow[i], LV_ALIGN_BOTTOM_MID, -26, -dy);
            lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);

            fc_desc_lbl[i] = NULL;
        }
    }

    /* Bottom row removed entirely — the live W + cumulative gas now live
       on the top-right Energy tile. Null the refs so legacy refresh
       writers no-op cleanly. */
    lbl_bot_energy   = NULL;
    lbl_outside_main = NULL;
    lbl_outside_sub  = NULL;
    lbl_bot_waste    = NULL;
    lbl_bot_weather  = NULL;

    /* (Old narrow bottom-row Waste tile + its trash icon removed — the
       full-size Waste tile now lives in the right column.) */

    /* Settings + Inbox icons are placed at the absolute screen top-right
       (not inside the big tile) per the user's spec. They float in the
       gap above the Humidity tile's accent bar — overlapping just the
       accent line, never the humidity value. */
    /* Update-available banner — sits along the top edge, between the
     * left-side schedule controls and the right-side buttons. Hidden
     * by default; refresh_cb shows it when g_update_state.available
     * flips on. Tap opens a modal with release notes + install hint. */
    update_banner = lv_btn_create(scr_root);
    lv_obj_set_size(update_banner, 380, 36);
    lv_obj_align(update_banner, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(update_banner, lv_color_hex(0x2e6e3a), 0);
    lv_obj_set_style_radius(update_banner, 8, 0);
    lv_obj_set_ext_click_area(update_banner, 8);
    lv_obj_add_event_cb(update_banner, on_update_banner_click,
                        LV_EVENT_CLICKED, NULL);
    update_banner_lbl = lv_label_create(update_banner);
    lv_obj_set_style_text_color(update_banner_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(update_banner_lbl, SF(18), 0);
    lv_label_set_text(update_banner_lbl, tr("Update beschikbaar", "Update available"));
    lv_obj_center(update_banner_lbl);
    lv_obj_add_flag(update_banner, LV_OBJ_FLAG_HIDDEN);

    /* Minimized-update envelope — shown (green) when the user dismisses the
     * update banner; tap re-opens the update modal. Sits left of the inbox
     * envelope. Hidden until dismissed. */
    update_env_btn = lv_btn_create(scr_root);
    lv_obj_set_size(update_env_btn, 44, 44);
    lv_obj_align(update_env_btn, LV_ALIGN_TOP_RIGHT, -118, 4);
    lv_obj_set_style_bg_color(update_env_btn, lv_color_hex(0x2e6e3a), 0);
    lv_obj_set_style_radius(update_env_btn, 22, 0);
    lv_obj_set_ext_click_area(update_env_btn, 14);
    lv_obj_add_event_cb(update_env_btn, on_update_modal_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * uenv = lv_label_create(update_env_btn);
    lv_label_set_text(uenv, LV_SYMBOL_ENVELOPE);
    lv_obj_set_style_text_color(uenv, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(uenv, SF(18), 0);
    lv_obj_center(uenv);
    lv_obj_add_flag(update_env_btn, LV_OBJ_FLAG_HIDDEN);

    envelope_btn = lv_btn_create(scr_root);
    lv_obj_set_size(envelope_btn, 44, 44);
    lv_obj_align(envelope_btn, LV_ALIGN_TOP_RIGHT, -66, 4);
    lv_obj_set_style_bg_color(envelope_btn, lv_color_hex(settings.home_light_mode ? 0xd0d8e2 : 0x335577), 0);
    lv_obj_set_style_radius(envelope_btn, 22, 0);
    lv_obj_set_ext_click_area(envelope_btn, 14);
    lv_obj_add_event_cb(envelope_btn, open_inbox, LV_EVENT_CLICKED, NULL);
    lv_obj_t * env_lbl = lv_label_create(envelope_btn);
    lv_label_set_text(env_lbl, LV_SYMBOL_ENVELOPE);
    lv_obj_set_style_text_color(env_lbl, lv_color_hex(settings.home_light_mode ? 0x1e2838 : 0xffffff), 0);
    lv_obj_set_style_text_font(env_lbl, SF(18), 0);
    lv_obj_center(env_lbl);

    envelope_badge = lv_obj_create(scr_root);
    lv_obj_set_size(envelope_badge, 20, 20);
    lv_obj_align(envelope_badge, LV_ALIGN_TOP_RIGHT, -60, 2);
    lv_obj_set_style_bg_color(envelope_badge, lv_color_hex(0xff3344), 0);
    lv_obj_set_style_radius(envelope_badge, 10, 0);
    lv_obj_set_style_border_width(envelope_badge, 2, 0);
    lv_obj_set_style_border_color(envelope_badge, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_pad_all(envelope_badge, 0, 0);
    lv_obj_clear_flag(envelope_badge, LV_OBJ_FLAG_SCROLLABLE);
    envelope_badge_lbl = lv_label_create(envelope_badge);
    lv_obj_set_style_text_color(envelope_badge_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(envelope_badge_lbl, SF(14), 0);
    lv_label_set_text(envelope_badge_lbl, "0");
    lv_obj_center(envelope_badge_lbl);
    lv_obj_add_flag(envelope_btn,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envelope_badge, LV_OBJ_FLAG_HIDDEN);

    /* (Old top-right lights button removed — superseded by the left-edge
     * collapsible lights handle and the swipe-right gesture.) */

    /* Lights handle — a slim half-circle tab on the left edge that auto-hides.
     * Touch it and it expands into a horizontal "Lights" button; releasing
     * opens the lights backend (Domoticz if enabled, else Home Assistant).
     * Same target as the swipe-right gesture, but discoverable.
     * Hidden if a visible Verlichting tile (LT_LIGHTS) exists on page 0. */
    const layout_tile_t * Llt = settings.custom_layout_enabled ? layout_find(LT_LIGHTS) : NULL;
    int have_lights_tile = (Llt && Llt->visible);
    lights_handle = NULL;
    lights_handle_img = NULL;
    lights_handle_lbl = NULL;
    if (!have_lights_tile) {
        lights_handle = lv_btn_create(scr_root);
        lv_obj_set_size(lights_handle, 56, 80);
        lv_obj_align(lights_handle, LV_ALIGN_LEFT_MID, -4, 0);
        lv_obj_set_style_bg_color(lights_handle, lv_color_hex(settings.home_light_mode ? 0xccdaea : 0x2a4060), 0);
        lv_obj_set_style_bg_color(lights_handle, lv_color_hex(settings.home_light_mode ? 0xb0c4de : 0x3a5688), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(lights_handle, LV_OPA_70, 0);
        lv_obj_set_style_radius(lights_handle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(lights_handle, 0, 0);
        lv_obj_set_ext_click_area(lights_handle, 24);   /* generous touch target */
        lv_obj_add_event_cb(lights_handle, on_lights_handle, LV_EVENT_PRESSED,    NULL);
        lv_obj_add_event_cb(lights_handle, on_lights_handle, LV_EVENT_RELEASED,   NULL);
        lv_obj_add_event_cb(lights_handle, on_lights_handle, LV_EVENT_PRESS_LOST, NULL);
        lv_obj_add_event_cb(lights_handle, on_lights_handle, LV_EVENT_CLICKED,    NULL);
        lights_handle_lbl = lv_label_create(lights_handle);
        lv_obj_set_style_text_color(lights_handle_lbl, lv_color_hex(settings.home_light_mode ? 0xd97706 : 0xffe08a), 0);
        lv_obj_set_style_text_font(lights_handle_lbl, SF(28), 0);
        lv_label_set_text(lights_handle_lbl, LV_SYMBOL_CHARGE);
        lv_obj_center(lights_handle_lbl);

        /* Collapsed handle shows the active backend's logo: Domoticz if that's
         * configured (matches open_lights_backend's priority), else HA. The
         * charge glyph stays as the fallback when neither is enabled. */
        const lv_img_dsc_t * logo = (settings.enable_domoticz || (settings.client_mode && domoticz_state.count > 0)) ? &icon_domoticz
                                  : (settings.enable_ha || (settings.client_mode && ha_light_count > 0)) ? &icon_ha
                                  : NULL;
        if (logo) {
            lights_handle_img = lv_img_create(lights_handle);
            lv_img_set_src(lights_handle_img, logo);
            lv_img_set_zoom(lights_handle_img, 224);   /* 48px -> ~42px */
            lv_obj_center(lights_handle_img);
            lv_obj_add_flag(lights_handle_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Gear in the very top-right corner of the screen. */
    lv_obj_t * gear = lv_btn_create(scr_root);
    lv_obj_set_size(gear, 44, 44);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_bg_color(gear, lv_color_hex(settings.home_light_mode ? 0xd0d8e2 : 0x223344), 0);
    lv_obj_set_style_radius(gear, 22, 0);
    lv_obj_set_ext_click_area(gear, 14);
    lv_obj_add_event_cb(gear, open_settings, LV_EVENT_CLICKED, NULL);
    lv_obj_t * gear_lbl = lv_label_create(gear);
    lv_label_set_text(gear_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gear_lbl, lv_color_hex(settings.home_light_mode ? 0x1e2838 : 0xffffff), 0);
    lv_obj_set_style_text_font(gear_lbl, SF(18), 0);
    lv_obj_center(gear_lbl);

    /* Pinned device tiles were built before the forecast strip, so raise them
     * above it now that every tile exists. */
    for (int p = 0; p < home_pin_count; p++)
        if (home_pins[p].tile) lv_obj_move_foreground(home_pins[p].tile);

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 500, NULL);
    return scr_root;
}

void screen_home_destroy(void) {
    if (scr_root) {
        lv_obj_t * s = scr_root;
        scr_root = NULL;
        lbl_t_clock_h = NULL;
        lbl_t_clock_colon = NULL;
        lbl_t_clock_m = NULL;
        lbl_t_temp = NULL;
        lbl_t_setpoint = NULL;
        lbl_t_burner = NULL;
        lbl_t_pressure = NULL;
        lbl_t_humidity = NULL;
        lbl_t_ppm = NULL;
        lbl_t_tvoc = NULL;
        lbl_t_date = NULL;
        moon_phase_img = NULL;
        lbl_t_aq = NULL;
        lbl_waste_date = NULL;
        lbl_waste_type = NULL;
        lbl_waste_date_2 = NULL;
        lbl_waste_type_2 = NULL;
        lbl_waste_date_3 = NULL;
        lbl_waste_type_3 = NULL;
        waste_icon_1 = NULL;
        waste_icon_2 = NULL;
        waste_icon_3 = NULL;
        pressure_banner = NULL;
        pressure_banner_lbl = NULL;
        lbl_t_program = NULL;
        tile_btn_mode_manual  = NULL;
        tile_btn_mode_program = NULL;
        for (int i = 0; i < 4; i++) tile_btn_preset[i] = NULL;
        tile_img_flame = NULL;
        tile_img_faucet = NULL;
        tile_img_drop = NULL;
        tile_img_water = NULL;
        tile_lbl_water = NULL;
        tile_lights_lbl_default = NULL;
        tile_lights_btn1 = NULL;
        tile_lights_lbl1_name = NULL;
        tile_lights_sw1 = NULL;
        tile_lights_lbl1_st = NULL;
        tile_lights_btn2 = NULL;
        tile_lights_lbl2_name = NULL;
        tile_lights_sw2 = NULL;
        tile_lights_lbl2_st = NULL;
        tile_news_sum_lbl = NULL;
        tile_cal_dates = NULL;
        tile_cal_times = NULL;
        tile_cal_titles = NULL;
        lbl_bot_energy = NULL;
        lbl_bot_weather = NULL;
        lbl_bot_waste = NULL;
        lbl_inbox_main = NULL;
        lbl_inbox_sub = NULL;
        water_spinner = NULL;
        forecast_box = NULL;
        lbl_outside_main = NULL;
        lbl_outside_sub = NULL;
        for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
            fc_day_lbl[i] = NULL;
            fc_temp_lbl[i] = NULL;
            fc_wind_lbl[i] = NULL;
            fc_desc_lbl[i] = NULL;
            fc_wind_arrow[i] = NULL;
            fc_icon[i] = NULL;
            fc_icon_sun[i] = NULL;
        }
        envelope_btn = NULL;
        envelope_badge = NULL;
        envelope_badge_lbl = NULL;
        update_banner = NULL;
        update_banner_lbl = NULL;
        update_env_btn = NULL;
        lbl_forecast_city = NULL;
        lbl_life360_a = NULL;
        lbl_life360_b = NULL;
        lbl_life360_c = NULL;
        tile_waste = NULL;
        tile_energy = NULL;
        tile_vent = NULL;
        tile_family = NULL;
        tile_water = NULL;
        for (int i = 0; i < 4; i++) {
            slot_agenda[i] = NULL;
            slot_sonos[i] = NULL;
            slot_waste[i] = NULL;
            slot_templogger[i] = NULL;
            lbl_title_slot[i] = NULL;
        }
        home_page1 = NULL;
        home_dot[0] = NULL;
        home_dot[1] = NULL;
        news_box = NULL;
        news_ticker = NULL;
        news_last_first[0] = '\0';
        news_last_nn = 0;
        lights_handle = NULL;
        lights_handle_img = NULL;
        lights_handle_lbl = NULL;
        lv_obj_del(s);
    }
}
