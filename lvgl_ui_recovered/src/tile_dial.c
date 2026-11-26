/* See tile_dial.h. */
#include "tile_dial.h"
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "ui_lazy.h"
#include "icons.h"
#include "boxtalk.h"
#include "pin_modal.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define SP_MIN 60            /* 6.0 °C, in tenths (the happ's range) */
#define SP_MAX 300           /* 30.0 °C */
#define SWEEP  270           /* degrees of the dial; the open part is at the bottom */

#define COL_HEAT   (settings.home_light_mode ? 0xe8590c : 0xff7a33)
#define COL_IDLE   (settings.home_light_mode ? 0x2563eb : 0x3b9bff)
#define COL_TRACK  (settings.home_light_mode ? 0xdde5ee : 0x24344c)
#define COL_HI     (settings.home_light_mode ? 0x1e2838 : 0xffffff)
#define COL_DIM    (settings.home_light_mode ? 0x687c94 : 0x88aabb)

static lv_obj_t * g_tile, * g_arc, * g_sp, * g_room, * g_state, * g_flame;
static lv_obj_t * g_minus, * g_plus;
static int    g_drag;          /* the knob is being dragged: show its value */
static time_t g_hold_until;    /* after a change: keep our value until the happ follows */
static int    g_hold_val;      /* tenths */
static uint32_t g_col = 0xffffffff;

static int snap(int v) {       /* 0.5 °C steps */
    v = (v + 2) / 5 * 5;
    return v < SP_MIN ? SP_MIN : v > SP_MAX ? SP_MAX : v;
}

static void show_sp(int tenths) {
    char b[16];
    snprintf(b, sizeof b, "%d.%d°", tenths / 10, tenths % 10);
    if (settings.language != LANG_EN) { char * d = strchr(b, '.'); if (d) *d = ','; }
    ui_label_set_text(g_sp, b);
}

static void apply_cb(void * ctx) {
    int v = (int)(intptr_t)ctx;
    boxtalk_setpoint_user(v / 10.0f);
}
static void up_cb(void * ctx)   { (void)ctx; boxtalk_setpoint_increase(); }
static void down_cb(void * ctx) { (void)ctx; boxtalk_setpoint_decrease(); }

static void hold(int v) {
    g_hold_val = v;
    g_hold_until = time(NULL) + 5;
}

static void on_arc(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_VALUE_CHANGED) {
        g_drag = 1;
        show_sp(snap(lv_arc_get_value(g_arc)));
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        if (!g_drag) return;
        g_drag = 0;
        int v = snap(lv_arc_get_value(g_arc));
        lv_arc_set_value(g_arc, v);
        show_sp(v);
        hold(v);
        pin_gate(apply_cb, (void *)(intptr_t)v);
    }
}

static void on_step(lv_event_t * e) {
    int up = (int)(intptr_t)lv_event_get_user_data(e);
    int cur = g_hold_until > time(NULL) ? g_hold_val : (int)(toon_state.setpoint * 10.0f + 0.5f);
    if (cur < SP_MIN) cur = 170;
    int v = snap(cur + (up ? 5 : -5));
    lv_arc_set_value(g_arc, v);
    show_sp(v);
    hold(v);
    pin_gate(up ? up_cb : down_cb, NULL);
}

static void on_tile_delete(lv_event_t * e) {
    if (lv_event_get_target(e) != g_tile) return;
    g_tile = g_arc = g_sp = g_room = g_state = g_flame = g_minus = g_plus = NULL;
    g_drag = 0;
    g_col = 0xffffffff;
}

static lv_obj_t * round_btn(lv_obj_t * parent, int sz, const char * sym, int up) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, sz, sz);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_ext_click_area(b, sz / 4);
    lv_obj_add_event_cb(b, on_step, LV_EVENT_CLICKED, (void *)(intptr_t)up);
    lv_obj_t * l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_HI), 0);
    lv_obj_set_style_text_font(l, sz >= 50 ? SF(24) : SF(18), 0);
    lv_obj_center(l);
    return b;
}

void td_tile_build(lv_obj_t * tile, lv_obj_t * title, lv_event_cb_t on_open) {
    g_tile = tile;
    lv_obj_add_event_cb(tile, on_tile_delete, LV_EVENT_DELETE, NULL);
    if (title) ui_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);   /* the dial says what it is */

    lv_obj_update_layout(tile);
    int cw = lv_obj_get_content_width(tile), ch = lv_obj_get_content_height(tile);
    int d = cw < ch ? cw : ch;
    d -= (DISP_VER < 600) ? 4 : 8;
    if (d < 120) d = 120;
    int cx = cw / 2, cy = ch / 2;
    int aw = d >= 300 ? 18 : d >= 200 ? 14 : 10;          /* ring width */

    /* tap in the middle: the thermostat screen (the ring and the buttons
     * take their own presses) */
    lv_obj_t * mid = lv_obj_create(tile);
    lv_obj_remove_style_all(mid);
    lv_obj_set_size(mid, d * 6 / 10, d * 6 / 10);
    lv_obj_set_pos(mid, cx - d * 3 / 10, cy - d * 3 / 10);
    lv_obj_set_style_radius(mid, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(mid, LV_OBJ_FLAG_CLICKABLE);
    if (on_open) lv_obj_add_event_cb(mid, on_open, LV_EVENT_CLICKED, NULL);

    g_arc = lv_arc_create(tile);
    lv_obj_set_size(g_arc, d, d);
    lv_obj_set_pos(g_arc, cx - d / 2, cy - d / 2);
    lv_arc_set_rotation(g_arc, 90 + (360 - SWEEP) / 2);   /* the gap at the bottom */
    lv_arc_set_bg_angles(g_arc, 0, SWEEP);
    lv_arc_set_range(g_arc, SP_MIN, SP_MAX);
    lv_arc_set_value(g_arc, 200);
    lv_obj_set_style_arc_width(g_arc, aw, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc, aw, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(g_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_arc, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_arc, lv_color_hex(COL_IDLE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_arc, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_arc, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_arc, aw / 3, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(g_arc, 6, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(g_arc, LV_OPA_40, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(g_arc, 0, 0);
    lv_obj_set_ext_click_area(g_arc, aw);
    lv_obj_add_event_cb(g_arc, on_arc, LV_EVENT_ALL, NULL);
    lv_obj_clear_flag(g_arc, LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_move_foreground(mid);   /* the arc object is a square over the middle: the tap area goes on top */

    /* size classes: L >= 300, M >= 220, S >= 160, XS below (no state line) */
    int cls = d >= SY(300) ? 3 : d >= SY(220) ? 2 : d >= SY(160) ? 1 : 0;
    const lv_font_t * big   = cls == 3 ? SF(64) : cls == 2 ? SF(48) : cls == 1 ? SF(36) : SF(28);
    const lv_font_t * small = cls >= 2 ? SF(18) : SF(14);
    int lh = lv_font_get_line_height(big);

    /* boiler state: flame + "Verwarmen 45%" / "Warm water" / "Ketel rust" */
    lv_obj_t * row = lv_obj_create(tile);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    g_flame = lv_img_create(row);
    lv_img_set_src(g_flame, &icon_flame);
    lv_obj_set_style_img_recolor(g_flame, lv_color_hex(COL_HEAT), 0);
    lv_obj_set_style_img_recolor_opa(g_flame, 255, 0);
    /* the flame as high as the state text (zoom counted in the layout) */
    {
        int ih = (int)icon_flame.header.h, th = lv_font_get_line_height(small);
        int z = ih > 0 ? th * 256 / ih : 256;
        lv_img_set_size_mode(g_flame, LV_IMG_SIZE_MODE_REAL);
        lv_img_set_zoom(g_flame, (uint16_t)(z < 64 ? 64 : z > 256 ? 256 : z));
    }
    ui_obj_add_flag(g_flame, LV_OBJ_FLAG_HIDDEN);
    g_state = lv_label_create(row);
    lv_obj_set_style_text_font(g_state, small, 0);
    lv_obj_set_style_text_color(g_state, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(g_state, "");
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, cy - lh / 2 - lv_font_get_line_height(small) - d / 30);
    if (cls == 0) ui_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);   /* too small: setpoint + -/+ only */

    g_sp = lv_label_create(tile);
    lv_obj_set_style_text_font(g_sp, big, 0);
    lv_obj_set_style_text_color(g_sp, lv_color_hex(COL_HI), 0);
    lv_label_set_text(g_sp, "--");
    lv_obj_align(g_sp, LV_ALIGN_TOP_MID, 0, cy - lh / 2);

    g_room = lv_label_create(tile);
    lv_obj_set_style_text_font(g_room, small, 0);
    lv_obj_set_style_text_color(g_room, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(g_room, "");
    lv_obj_align(g_room, LV_ALIGN_TOP_MID, 0, cy + lh / 2 + d / 40);
    if (cls == 0) ui_obj_add_flag(g_room, LV_OBJ_FLAG_HIDDEN);

    /* -/+ in the open bottom of the dial */
    int bs = cls == 3 ? SX(56) : cls == 2 ? SX(46) : cls == 1 ? SX(38) : SX(32);
    g_minus = round_btn(tile, bs, LV_SYMBOL_MINUS, 0);
    g_plus  = round_btn(tile, bs, LV_SYMBOL_PLUS, 1);
    /* in the gap between the dial's ends (at +-0.35 d, 0.35 d) */
    int bx = d * 17 / 100, by = d / 2 - bs / 2 - (DISP_VER < 600 ? 2 : 4);
    lv_obj_set_pos(g_minus, cx - bx - bs / 2, cy + by - bs / 2);
    lv_obj_set_pos(g_plus,  cx + bx - bs / 2, cy + by - bs / 2);

    td_tile_update();
}

void td_tile_update(void) {
    if (!g_tile || !g_arc) return;
    time_t now = time(NULL);
    int sp = (int)(toon_state.setpoint * 10.0f + 0.5f);
    int held = g_hold_until > now;
    if (held && sp == g_hold_val) { held = 0; g_hold_until = 0; }   /* the happ followed */
    if (!g_drag) {
        int v = held ? g_hold_val : sp;
        if (v >= SP_MIN && v <= SP_MAX) {
            lv_arc_set_value(g_arc, v);
            show_sp(v);
        } else if (!held) {
            ui_label_set_text(g_sp, "--");
        }
    }

    char b[48];
    if (toon_state.indoor_temp > 0) {
        snprintf(b, sizeof b, tr("In huis %.1f°", "Indoors %.1f°"), (double)toon_state.indoor_temp);
        if (settings.language != LANG_EN) { char * d = strchr(b, '.'); if (d) *d = ','; }
        ui_label_set_text(g_room, b);
    }

    uint32_t col;
    if (boxtalk_preheat_shown()) {
        snprintf(b, sizeof b, "%s", tr("Voorverwarmen", "Pre-heating"));
        col = COL_HEAT;
        ui_obj_clear_flag(g_flame, LV_OBJ_FLAG_HIDDEN);
    } else if (toon_state.burner_on) {
        int mod = (int)(toon_state.modulation_level + 0.5f);
        if (toon_state.boiler_type == 0 && mod > 0 && mod <= 100)
            snprintf(b, sizeof b, tr("Verwarmen %d%%", "Heating %d%%"), mod);
        else
            snprintf(b, sizeof b, "%s", tr("Verwarmen", "Heating"));
        col = COL_HEAT;
        ui_obj_clear_flag(g_flame, LV_OBJ_FLAG_HIDDEN);
    } else if (toon_state.dhw_on) {
        snprintf(b, sizeof b, "%s", tr("Warm water", "Hot water"));
        col = COL_IDLE;
        ui_obj_add_flag(g_flame, LV_OBJ_FLAG_HIDDEN);
    } else {
        snprintf(b, sizeof b, "%s", tr("Ketel rust", "Boiler idle"));
        col = COL_IDLE;
        ui_obj_add_flag(g_flame, LV_OBJ_FLAG_HIDDEN);
    }
    ui_label_set_text(g_state, b);
    if (col != g_col) {
        g_col = col;
        lv_obj_set_style_arc_color(g_arc, lv_color_hex(col), LV_PART_INDICATOR);
    }
}
