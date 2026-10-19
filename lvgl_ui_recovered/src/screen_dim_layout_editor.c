/*
 * screen_dim_layout_editor.c — "Dim-indeling" editor.
 *
 * Full-screen modal that arranges the dim/ambient screen's peripheral blocks
 * (weather, waste, Life360, ventilation, thermostat readout, forecast strip,
 * clock, date, energy, gas) on the same LAYOUT_COLS×LAYOUT_ROWS grid the home
 * editor uses.
 *
 * Revamped:
 * - Full Light/Dark theme support (THEME_BG, THEME_CARD_BG, THEME_BORDER, etc.)
 * - Swap fallback & collision prevention when dragging (no overlap/stacking)
 * - Live drag feedback: green border when valid/swap, red border when no room
 * - Clean tile hiding (hidden block rect is removed, freeing the grid cells)
 * - "+ Blok" picker modal to restore any hidden block cleanly
 * - Safe resizing that respects adjacent block boundaries
 * - Enlarged touch targets for all toolbar buttons
 */
#include "lvgl/lvgl.h"
#include "screens.h"
#include "display.h"   /* SF()/SX()/SY() */
#include "layout.h"
#include "settings.h"
#include "theme.h"
#include "i18n.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifdef TOON1
#  define SCR_W 800
#  define SCR_H 480
#else
#  define SCR_W 1024
#  define SCR_H 600
#endif
#define BAR_H    64
#define CANVAS_H (SCR_H - BAR_H)
#define CELL_W   (SCR_W / LAYOUT_COLS)
#define CELL_H   (CANVAS_H / LAYOUT_ROWS)
#define DIM_MIN_W 2
#define DIM_MIN_H 2

static lv_obj_t * modal = NULL;
static lv_obj_t * chooser_modal = NULL;
static lv_obj_t * rects[DB_COUNT];
static dim_block_t edit_dim[DB_COUNT];
static int sel = -1;
static lv_obj_t * sel_lbl = NULL;

static int drag_moved = 0, press_x0 = 0, press_y0 = 0;
static int drag_last_col = -1, drag_last_row = -1;
static dim_block_t drag_preview[DB_COUNT];
static int drag_preview_ok = 0;

static const uint32_t DB_COL[DB_COUNT] = {
    [DB_THERMO]=0x335577, [DB_WEATHER]=0x4488aa, [DB_FORECAST]=0x66bbdd,
    [DB_WASTE]=0x88dd66,  [DB_FAMILY]=0xff8866,  [DB_VENT]=0x6666aa,
    [DB_ENERGY]=0xaa77ff, [DB_GAS]=0xffaa33,     [DB_CLOCK]=0x2b7bbb,
    [DB_DATE]=0x486b88,
};

static void place_rect(int i);
static void create_rect(int i);
static void build_canvas_rects(void);
static void update_sel_label(void);
static void close_chooser(void);

static void select_block(int i) {
    sel = i;
    for (int k = 0; k < DB_COUNT; k++) {
        if (rects[k]) {
            lv_obj_set_style_border_width(rects[k], k == i ? 4 : 0, 0);
            lv_obj_set_style_border_color(rects[k], lv_color_hex(0xffffff), 0);
        }
    }
    update_sel_label();
}

static void update_sel_label(void) {
    if (!sel_lbl) return;
    if (sel < 0 || sel >= DB_COUNT || !edit_dim[sel].visible) {
        lv_label_set_text(sel_lbl, tr("Tik een blok om te selecteren", "Tap a block to select"));
        return;
    }
    dim_block_t * b = &edit_dim[sel];
    lv_label_set_text_fmt(sel_lbl, "%s  %dx%d", dim_block_name(sel), b->w, b->h);
}

static int would_overlap(int idx, int c, int r, int w, int h) {
    for (int k = 0; k < DB_COUNT; k++) {
        if (k == idx || !edit_dim[k].visible) continue;
        const dim_block_t * b = &edit_dim[k];
        if (c < b->col + b->w && b->col < c + w &&
            r < b->row + b->h && b->row < r + h) return 1;
    }
    return 0;
}

static int find_free_cell(int idx, int w, int h, int * oc, int * orow) {
    for (int r = 0; r <= LAYOUT_ROWS - h; r++) {
        for (int c = 0; c <= LAYOUT_COLS - w; c++) {
            if (!would_overlap(idx, c, r, w, h)) {
                *oc = c; *orow = r;
                return 1;
            }
        }
    }
    return 0;
}

static void snap_cell(int i, int * oc, int * orow) {
    dim_block_t * b = &edit_dim[i];
    int col = (lv_obj_get_x(rects[i]) + CELL_W / 2) / CELL_W;
    int row = (lv_obj_get_y(rects[i]) + CELL_H / 2) / CELL_H;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (col + b->w > LAYOUT_COLS) col = LAYOUT_COLS - b->w;
    if (row + b->h > LAYOUT_ROWS) row = LAYOUT_ROWS - b->h;
    *oc = col; *orow = row;
}

static int trial_move(int i, int col, int row, dim_block_t * out) {
    memcpy(out, edit_dim, sizeof(dim_block_t) * DB_COUNT);
    out[i].col = col;
    out[i].row = row;

    int orig_col = edit_dim[i].col;
    int orig_row = edit_dim[i].row;
    int w = edit_dim[i].w;
    int h = edit_dim[i].h;

    /* Check clash with other visible blocks */
    int clash_idx = -1, clash_count = 0;
    for (int k = 0; k < DB_COUNT; k++) {
        if (k == i || !edit_dim[k].visible) continue;
        const dim_block_t * b = &edit_dim[k];
        if (col < b->col + b->w && b->col < col + w &&
            row < b->row + b->h && b->row < row + h) {
            clash_idx = k;
            clash_count++;
        }
    }

    if (clash_count == 0) return 1; /* Clean drop into free space */

    /* Swap fallback: if dropped onto exactly 1 other block, try swapping */
    if (clash_count == 1 && clash_idx >= 0) {
        int cw = edit_dim[clash_idx].w;
        int ch = edit_dim[clash_idx].h;
        if (orig_col + cw <= LAYOUT_COLS && orig_row + ch <= LAYOUT_ROWS) {
            out[clash_idx].col = orig_col;
            out[clash_idx].row = orig_row;
            /* Verify clash_idx does not clash with any other block */
            int ok = 1;
            for (int k = 0; k < DB_COUNT && ok; k++) {
                if (k == i || k == clash_idx || !out[k].visible) continue;
                const dim_block_t * a = &out[k];
                if (orig_col < a->col + a->w && a->col < orig_col + cw &&
                    orig_row < a->row + a->h && a->row < orig_row + ch) {
                    ok = 0;
                }
            }
            if (ok) return 1;
        }
    }

    return 0;
}

static void preview_others(const dim_block_t * src, int except) {
    for (int k = 0; k < DB_COUNT; k++) {
        if (k == except || !rects[k] || !src[k].visible) continue;
        lv_obj_set_pos(rects[k], src[k].col * CELL_W, src[k].row * CELL_H);
    }
}

static void rect_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * r = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(r);
    if (code == LV_EVENT_PRESSED) {
        select_block(i);
        drag_moved = 0;
        drag_last_col = drag_last_row = -1;
        press_x0 = lv_obj_get_x(r); press_y0 = lv_obj_get_y(r);
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t * in = lv_indev_get_act();
        lv_point_t v; lv_indev_get_vect(in, &v);
        lv_obj_set_pos(r, lv_obj_get_x(r) + v.x, lv_obj_get_y(r) + v.y);
        int dx = lv_obj_get_x(r) - press_x0, dy = lv_obj_get_y(r) - press_y0;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > 10 || dy > 10) drag_moved = 1;
        if (!drag_moved) return;

        int col, row; snap_cell(i, &col, &row);
        if (col != drag_last_col || row != drag_last_row) {
            drag_last_col = col; drag_last_row = row;
            drag_preview_ok = trial_move(i, col, row, drag_preview);
            lv_obj_set_style_border_width(r, 4, 0);
            lv_obj_set_style_border_color(r,
                lv_color_hex(drag_preview_ok ? 0x33dd66 : 0xdd4444), 0);
            preview_others(drag_preview_ok ? drag_preview : edit_dim, i);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (drag_moved) {
            int col, row; snap_cell(i, &col, &row);
            if (trial_move(i, col, row, drag_preview)) {
                memcpy(edit_dim, drag_preview, sizeof edit_dim);
            }
        }
        for (int k = 0; k < DB_COUNT; k++) place_rect(k);
        select_block(i);
        drag_moved = 0;
        drag_last_col = drag_last_row = -1;
    }
}

static void place_rect(int i) {
    dim_block_t * b = &edit_dim[i];
    lv_obj_t * r = rects[i];
    if (!r || !b->visible) return;
    lv_obj_set_pos(r, b->col * CELL_W, b->row * CELL_H);
    lv_obj_set_size(r, b->w * CELL_W - 4, b->h * CELL_H - 4);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
}

static void create_rect(int i) {
    if (!edit_dim[i].visible) {
        rects[i] = NULL;
        return;
    }
    lv_obj_t * r = lv_obj_create(modal);
    rects[i] = r;
    lv_obj_set_user_data(r, (void *)(intptr_t)i);
    lv_obj_set_style_bg_color(r, lv_color_hex(DB_COL[i]), 0);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_pad_all(r, 6, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, rect_event, LV_EVENT_ALL, NULL);

    lv_obj_t * l = lv_label_create(r);
    lv_label_set_text(l, dim_block_name(i));
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, SF(15), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 2, 2);
    place_rect(i);
}

static void build_canvas_rects(void) {
    for (int i = 0; i < DB_COUNT; i++) {
        if (rects[i]) {
            lv_obj_del(rects[i]);
            rects[i] = NULL;
        }
    }
    for (int i = 0; i < DB_COUNT; i++) create_rect(i);
}

static void on_resize(lv_event_t * e) {
    if (sel < 0 || sel >= DB_COUNT || !edit_dim[sel].visible) return;
    int d = (int)(intptr_t)lv_event_get_user_data(e);   /* 0:w- 1:w+ 2:h- 3:h+ */
    dim_block_t * b = &edit_dim[sel];
    int min_h = (sel == DB_DATE || sel == DB_ENERGY || sel == DB_GAS) ? 1 : DIM_MIN_H;
    int min_w = DIM_MIN_W;

    if (d == 0) { /* W- */
        if (b->w > min_w) b->w--;
    } else if (d == 1) { /* W+ */
        if (b->col + b->w < LAYOUT_COLS && !would_overlap(sel, b->col, b->row, b->w + 1, b->h)) {
            b->w++;
        } else if (b->col > 0 && !would_overlap(sel, b->col - 1, b->row, b->w + 1, b->h)) {
            b->col--;
            b->w++;
        } else {
            lv_label_set_text(sel_lbl, tr("Geen ruimte om te vergroten", "No room to enlarge"));
            return;
        }
    } else if (d == 2) { /* H- */
        if (b->h > min_h) b->h--;
    } else if (d == 3) { /* H+ */
        if (b->row + b->h < LAYOUT_ROWS && !would_overlap(sel, b->col, b->row, b->w, b->h + 1)) {
            b->h++;
        } else if (b->row > 0 && !would_overlap(sel, b->col, b->row - 1, b->w, b->h + 1)) {
            b->row--;
            b->h++;
        } else {
            lv_label_set_text(sel_lbl, tr("Geen ruimte om te vergroten", "No room to enlarge"));
            return;
        }
    }
    place_rect(sel);
    select_block(sel);
}

static void on_toggle_vis(lv_event_t * e) {
    (void)e;
    if (sel < 0 || sel >= DB_COUNT || !edit_dim[sel].visible) {
        lv_label_set_text(sel_lbl, tr("Tik eerst een blok", "Tap a block first"));
        return;
    }
    edit_dim[sel].visible = 0;
    if (rects[sel]) {
        lv_obj_del(rects[sel]);
        rects[sel] = NULL;
    }
    sel = -1;
    update_sel_label();
}

static void close_chooser(void) {
    if (chooser_modal) {
        lv_obj_del(chooser_modal);
        chooser_modal = NULL;
    }
}

static void on_chooser_close(lv_event_t * e) {
    (void)e;
    close_chooser();
}

static void on_restore_block(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= DB_COUNT) return;

    int w = edit_dim[idx].w;
    int h = edit_dim[idx].h;
    int min_h = (idx == DB_DATE || idx == DB_ENERGY || idx == DB_GAS) ? 1 : DIM_MIN_H;
    if (w < DIM_MIN_W) w = DIM_MIN_W;
    if (h < min_h) h = min_h;

    int c, r;
    if (!find_free_cell(idx, w, h, &c, &r)) {
        w = DIM_MIN_W;
        h = min_h;
        if (!find_free_cell(idx, w, h, &c, &r)) {
            c = 0; r = 0;
        }
    }

    edit_dim[idx].col = c;
    edit_dim[idx].row = r;
    edit_dim[idx].w = w;
    edit_dim[idx].h = h;
    edit_dim[idx].visible = 1;

    close_chooser();
    build_canvas_rects();
    select_block(idx);
}

static void open_add_modal(lv_event_t * e) {
    (void)e;
    close_chooser();

    chooser_modal = lv_obj_create(modal);
    lv_obj_set_size(chooser_modal, SCR_W, SCR_H);
    lv_obj_set_pos(chooser_modal, 0, 0);
    lv_obj_set_style_bg_color(chooser_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(chooser_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(chooser_modal, 0, 0);
    lv_obj_clear_flag(chooser_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * card = lv_obj_create(chooser_modal);
    lv_obj_set_size(card, SX(560), SY(420));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, SX(20), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(card);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(20), 0);
    lv_label_set_text(title, tr("Blok toevoegen", "Add block"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t * b_close = lv_btn_create(card);
    lv_obj_set_size(b_close, SX(64), SY(40));
    lv_obj_align(b_close, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(b_close, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(b_close, 8, 0);
    lv_obj_add_event_cb(b_close, on_chooser_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(b_close);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(cl, SF(16), 0);
    lv_obj_center(cl);

    /* Grid of hidden blocks */
    lv_obj_t * grid = lv_obj_create(card);
    lv_obj_set_size(grid, SX(520), SY(320));
    lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 4, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    int hidden_count = 0;
    for (int i = 0; i < DB_COUNT; i++) {
        if (!edit_dim[i].visible) {
            hidden_count++;
            lv_obj_t * btn = lv_btn_create(grid);
            lv_obj_set_size(btn, SX(160), SY(52));
            lv_obj_set_style_bg_color(btn, lv_color_hex(DB_COL[i]), 0);
            lv_obj_set_style_radius(btn, 10, 0);
            lv_obj_add_event_cb(btn, on_restore_block, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            lv_obj_t * bl = lv_label_create(btn);
            lv_label_set_text(bl, dim_block_name(i));
            lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_text_font(bl, SF(15), 0);
            lv_obj_center(bl);
        }
    }

    if (hidden_count == 0) {
        lv_obj_t * empty = lv_label_create(grid);
        lv_label_set_text(empty, tr("Alle blokken worden al getoond.", "All blocks are already shown."));
        lv_obj_set_style_text_color(empty, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(empty, SF(16), 0);
    }
}

static void on_reset(lv_event_t * e) {
    (void)e;
    dim_layout_reset_default();
    dim_layout_save();
    settings.dim_custom_enabled = 0;
    settings_save();
    fprintf(stderr, "[dim-layout] reset to standard dim — restarting UI\n");
    ui_request_restart();
}

static void on_cancel(lv_event_t * e) {
    (void)e;
    close_chooser();
    if (modal) { lv_obj_del(modal); modal = NULL; }
    for (int i = 0; i < DB_COUNT; i++) rects[i] = NULL;
    sel = -1;
}

static void on_save(lv_event_t * e) {
    (void)e;
    for (int i = 0; i < DB_COUNT; i++) g_dim_blocks[i] = edit_dim[i];
    dim_layout_save();
    settings.dim_custom_enabled = 1;
    settings_save();
    fprintf(stderr, "[dim-layout] saved + enabled — restarting UI to apply\n");
    ui_request_restart();
}

static lv_obj_t * tb_btn(lv_obj_t * bar, int x, int w, const char * txt,
                         lv_event_cb_t cb, void * ud, uint32_t col) {
    lv_obj_t * b = lv_btn_create(bar);
    lv_obj_set_size(b, w, SY(BAR_H - 14));
    lv_obj_set_pos(b, x, SY(7));
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t * l = lv_label_create(b);
    lv_label_set_text(l, txt);
#ifdef TOON1
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
#endif
    lv_obj_center(l);
    return b;
}

lv_obj_t * screen_dim_layout_editor_create(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_scr_load(scr);
    screen_dim_layout_editor_show();
    return scr;
}

void screen_dim_layout_editor_show(void) {
    if (modal) lv_obj_del(modal);
    modal = NULL; chooser_modal = NULL; sel = -1;
    drag_moved = 0; drag_last_col = -1; drag_last_row = -1;
    for (int i = 0; i < DB_COUNT; i++) {
        rects[i] = NULL;
        edit_dim[i] = g_dim_blocks[i];
    }

    modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, SCR_W, SCR_H);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);

    /* faint grid backdrop */
    for (int c = 1; c < LAYOUT_COLS; c++) {
        lv_obj_t * ln = lv_obj_create(modal);
        lv_obj_remove_style_all(ln);
        lv_obj_set_size(ln, 1, CANVAS_H);
        lv_obj_set_pos(ln, c * CELL_W, 0);
        lv_obj_set_style_bg_color(ln, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_50, 0);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    }
    for (int r = 1; r < LAYOUT_ROWS; r++) {
        lv_obj_t * ln = lv_obj_create(modal);
        lv_obj_remove_style_all(ln);
        lv_obj_set_size(ln, SCR_W, 1);
        lv_obj_set_pos(ln, 0, r * CELL_H);
        lv_obj_set_style_bg_color(ln, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_50, 0);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    }

    build_canvas_rects();

    /* bottom toolbar */
    lv_obj_t * bar = lv_obj_create(modal);
    lv_obj_set_size(bar, SCR_W, BAR_H);
    lv_obj_set_pos(bar, 0, SCR_H - BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(bar, settings.home_light_mode ? 1 : 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    int x = SX(8);
    tb_btn(bar, x, SX(64), tr("Sluit", "Close"), on_cancel, NULL, 0x444444); x += SX(70);
    tb_btn(bar, x, SX(42), "W-", on_resize, (void *)(intptr_t)0, 0x2a4060); x += SX(46);
    tb_btn(bar, x, SX(42), "W+", on_resize, (void *)(intptr_t)1, 0x2a4060); x += SX(46);
    tb_btn(bar, x, SX(42), "H-", on_resize, (void *)(intptr_t)2, 0x2a4060); x += SX(46);
    tb_btn(bar, x, SX(42), "H+", on_resize, (void *)(intptr_t)3, 0x2a4060); x += SX(48);
    tb_btn(bar, x, SX(82), tr("Verberg", "Hide"), on_toggle_vis, NULL, 0x553355); x += SX(88);
    tb_btn(bar, x, SX(74), tr("+ Blok", "+ Block"), open_add_modal, NULL, 0x2e5e6e); x += SX(80);
    tb_btn(bar, x, SX(92), tr("Standaard", "Default"), on_reset, NULL, 0x665522); x += SX(98);

    sel_lbl = lv_label_create(bar);
    lv_obj_set_style_text_color(sel_lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(sel_lbl, SF(14), 0);
    lv_obj_align(sel_lbl, LV_ALIGN_LEFT_MID, x + SX(6), 0);

    tb_btn(bar, SCR_W - SX(96), SX(88), tr("Opslaan", "Save"), on_save, NULL, 0x2e6e3a);
    update_sel_label();
}
