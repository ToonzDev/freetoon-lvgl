/*
 * screen_sonos.c — Full-screen Sonos Player & Favorites modal.
 * Provides room selector, playback state, transport controls, volume slider,
 * and a browsable list of Sonos favorites / radio stations for instant playback.
 */
#include "lvgl/lvgl.h"
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "sonos.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAX_FAVORITES 32

static lv_obj_t * s_modal = NULL;
static lv_obj_t * s_lbl_zone = NULL;
static lv_obj_t * s_dd_zone = NULL;
static lv_obj_t * s_lbl_status = NULL;
static lv_obj_t * s_lbl_title = NULL;
static lv_obj_t * s_lbl_artist = NULL;
static lv_obj_t * s_lbl_album = NULL;
static lv_obj_t * s_btn_play = NULL;
static lv_obj_t * s_lbl_play = NULL;
static lv_obj_t * s_slider_vol = NULL;
static lv_obj_t * s_lbl_vol = NULL;
static lv_obj_t * s_fav_list = NULL;
static lv_obj_t * s_lbl_fav_msg = NULL;
static lv_timer_t * s_refresh_timer = NULL;

static bool s_vol_dragging = false;

/* Background loading for favorites */
static char s_fav_names[MAX_FAVORITES][64];
static int  s_fav_count = 0;
static volatile int s_fav_loading = 0;
static volatile int s_fav_ready = 0;

static void * fav_loader_thread(void * arg) {
    (void)arg;
    int n = sonos_get_favorites(s_fav_names, MAX_FAVORITES);
    s_fav_count = n;
    s_fav_ready = 1;
    s_fav_loading = 0;
    return NULL;
}

static void trigger_load_favorites(void) {
    if (s_fav_loading) return;
    s_fav_loading = 1;
    s_fav_ready = 0;
    pthread_t tid;
    if (pthread_create(&tid, NULL, fav_loader_thread, NULL) == 0) {
        pthread_detach(tid);
    } else {
        s_fav_loading = 0;
    }
}

static void on_close_click(lv_event_t * e) {
    (void)e;
    if (s_refresh_timer) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    if (s_modal) {
        lv_obj_del(s_modal);
        s_modal = NULL;
    }
}

static void on_settings_click(lv_event_t * e) {
    (void)e;
    on_close_click(NULL);
    screen_settings_open_sonos_modal();
}

static void on_play_pause_click(lv_event_t * e) {
    (void)e;
    sonos_play_pause();
}

static void on_prev_click(lv_event_t * e) {
    (void)e;
    sonos_prev();
}

static void on_next_click(lv_event_t * e) {
    (void)e;
    sonos_next();
}

static void on_vol_down_click(lv_event_t * e) {
    (void)e;
    sonos_volume_down();
}

static void on_vol_up_click(lv_event_t * e) {
    (void)e;
    sonos_volume_up();
}

static void on_vol_slider_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);
    if (code == LV_EVENT_PRESSED) {
        s_vol_dragging = true;
    } else if (code == LV_EVENT_RELEASED) {
        s_vol_dragging = false;
        int val = lv_slider_get_value(slider);
        sonos_set_volume(val);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        int val = lv_slider_get_value(slider);
        if (s_lbl_vol) lv_label_set_text_fmt(s_lbl_vol, "%s: %d%%", tr("Volume", "Volume"), val);
        if (!s_vol_dragging) {
            sonos_set_volume(val);
        }
    }
}

static void on_zone_change(lv_event_t * e) {
    lv_obj_t * dd = lv_event_get_target(e);
    char buf[48];
    lv_dropdown_get_selected_str(dd, buf, sizeof buf);
    if (buf[0]) {
        sonos_set_zone(buf);
        trigger_load_favorites();
    }
}

static void on_fav_item_click(lv_event_t * e) {
    const char * name = (const char *)lv_event_get_user_data(e);
    if (name && name[0]) {
        sonos_play_favorite(name);
        if (s_lbl_status) {
            lv_label_set_text_fmt(s_lbl_status, "▶ %s...", name);
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x22cc66), 0);
        }
    }
}

static void on_fav_refresh_click(lv_event_t * e) {
    (void)e;
    if (s_lbl_fav_msg) {
        lv_label_set_text(s_lbl_fav_msg, tr("Laden...", "Loading..."));
        lv_obj_clear_flag(s_lbl_fav_msg, LV_OBJ_FLAG_HIDDEN);
    }
    trigger_load_favorites();
}

static void populate_favorites_list(void) {
    if (!s_fav_list) return;
    lv_obj_clean(s_fav_list);

    if (s_fav_count <= 0) {
        lv_obj_t * m = lv_label_create(s_fav_list);
        lv_label_set_text(m, tr("Geen favorieten gevonden in Sonos.", "No favorites found in Sonos."));
        lv_obj_set_style_text_color(m, lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(m, SF(18), 0);
        return;
    }

    for (int i = 0; i < s_fav_count; i++) {
        lv_obj_t * btn = lv_btn_create(s_fav_list);
        lv_obj_set_size(btn, SX(450), SY(52));
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a2e48), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_pad_all(btn, SX(10), 0);
        lv_obj_add_event_cb(btn, on_fav_item_click, LV_EVENT_CLICKED, (void *)s_fav_names[i]);

        lv_obj_t * ico = lv_label_create(btn);
        lv_label_set_text(ico, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(ico, lv_color_hex(0x44aaff), 0);
        lv_obj_set_style_text_font(ico, SF(18), 0);
        lv_obj_align(ico, LV_ALIGN_LEFT_MID, SX(4), 0);

        lv_obj_t * lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_fav_names[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl, SF(18), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, SX(390));
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, SX(34), 0);
    }
}

static void sonos_modal_tick_cb(lv_timer_t * t) {
    (void)t;
    if (!s_modal) return;

    /* Check if background favorites load completed */
    if (s_fav_ready) {
        s_fav_ready = 0;
        if (s_lbl_fav_msg) lv_obj_add_flag(s_lbl_fav_msg, LV_OBJ_FLAG_HIDDEN);
        populate_favorites_list();
    }

    sonos_state_t st;
    sonos_get_snapshot(&st);

    if (!settings.sonos_enabled || !settings.sonos_host[0]) {
        if (s_lbl_status) {
            lv_label_set_text(s_lbl_status, tr("⚠ Sonos niet ingeschakeld", "⚠ Sonos disabled"));
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xffaa22), 0);
        }
        if (s_lbl_title) lv_label_set_text(s_lbl_title, tr("Tik op instellingen om host in te stellen", "Tap settings to configure host"));
        if (s_lbl_artist) lv_label_set_text(s_lbl_artist, "");
        if (s_lbl_album) lv_label_set_text(s_lbl_album, "");
        return;
    }

    if (!st.connected) {
        if (s_lbl_status) {
            lv_label_set_text(s_lbl_status, tr("⚠ Geen verbinding met Sonos API", "⚠ No connection to Sonos API"));
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xff4444), 0);
        }
        if (s_lbl_title) lv_label_set_text(s_lbl_title, tr("Controleer host en poort", "Check host and port"));
        if (s_lbl_artist) lv_label_set_text(s_lbl_artist, "");
        if (s_lbl_album) lv_label_set_text(s_lbl_album, "");
        return;
    }

    /* Playback status */
    if (s_lbl_status) {
        if (st.playing) {
            lv_label_set_text(s_lbl_status, tr("● Speelt", "● Playing"));
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x22cc66), 0);
        } else {
            lv_label_set_text(s_lbl_status, tr("⏸ Gepauzeerd", "⏸ Paused"));
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xffaa22), 0);
        }
    }

    /* Track metadata */
    if (s_lbl_title) {
        if (st.title[0]) lv_label_set_text(s_lbl_title, st.title);
        else lv_label_set_text(s_lbl_title, st.playing ? tr("Geen titel", "No title") : tr("Geen muziek", "No music"));
    }
    if (s_lbl_artist) {
        lv_label_set_text(s_lbl_artist, st.artist[0] ? st.artist : "");
    }
    if (s_lbl_album) {
        lv_label_set_text(s_lbl_album, st.album[0] ? st.album : "");
    }

    /* Play/Pause button symbol */
    if (s_lbl_play) {
        lv_label_set_text(s_lbl_play, st.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    /* Volume slider */
    if (!s_vol_dragging && s_slider_vol) {
        lv_slider_set_value(s_slider_vol, st.volume, LV_ANIM_OFF);
        if (s_lbl_vol) lv_label_set_text_fmt(s_lbl_vol, "%s: %d%%", tr("Volume", "Volume"), st.volume);
    }
}

void screen_sonos_show(void) {
    if (s_modal) lv_obj_del(s_modal);

    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x0c1524), 0);
    lv_obj_set_style_bg_opa(s_modal, 248, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* --- Top Header --- */
    lv_obj_t * title = lv_label_create(s_modal);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, "Sonos");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(24), SY(16));

    /* Room selector dropdown */
    char rooms[SONOS_MAX_ZONES][48];
    int n_rooms = sonos_probe(rooms, SONOS_MAX_ZONES);
    char opts[1024] = "";
    int sel_idx = 0;
    if (n_rooms > 0) {
        for (int i = 0; i < n_rooms; i++) {
            if (opts[0]) strncat(opts, "\n", sizeof opts - strlen(opts) - 1);
            strncat(opts, rooms[i], sizeof opts - strlen(opts) - 1);
            if (settings.sonos_zone[0] && strcmp(settings.sonos_zone, rooms[i]) == 0)
                sel_idx = i;
        }
    } else if (settings.sonos_zone[0]) {
        snprintf(opts, sizeof opts, "%s", settings.sonos_zone);
    } else {
        snprintf(opts, sizeof opts, "%s", tr("Kamer kiezen...", "Select room..."));
    }

    s_dd_zone = lv_dropdown_create(s_modal);
    lv_obj_set_size(s_dd_zone, SX(260), SY(48));
    lv_obj_align(s_dd_zone, LV_ALIGN_TOP_LEFT, SX(150), SY(12));
    lv_obj_set_style_text_font(s_dd_zone, SF(18), 0);
    lv_dropdown_set_options(s_dd_zone, opts);
    lv_dropdown_set_selected(s_dd_zone, sel_idx);
    lv_obj_add_event_cb(s_dd_zone, on_zone_change, LV_EVENT_VALUE_CHANGED, NULL);

    /* Settings button */
    lv_obj_t * b_set = lv_btn_create(s_modal);
    lv_obj_set_size(b_set, SX(56), SY(52));
    lv_obj_align(b_set, LV_ALIGN_TOP_RIGHT, SX(-84), SY(10));
    lv_obj_set_style_bg_color(b_set, lv_color_hex(0x22354e), 0);
    lv_obj_set_style_radius(b_set, 10, 0);
    lv_obj_add_event_cb(b_set, on_settings_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(b_set);
    lv_label_set_text(sl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(sl, lv_color_hex(0xffffff), 0);
    lv_obj_center(sl);

    /* Close button */
    lv_obj_t * b_close = lv_btn_create(s_modal);
    lv_obj_set_size(b_close, SX(56), SY(52));
    lv_obj_align(b_close, LV_ALIGN_TOP_RIGHT, SX(-16), SY(10));
    lv_obj_set_style_bg_color(b_close, lv_color_hex(0x33445a), 0);
    lv_obj_set_style_radius(b_close, 10, 0);
    lv_obj_add_event_cb(b_close, on_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(b_close);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xffffff), 0);
    lv_obj_center(cl);

    /* --- Left Card: Now Playing --- */
    lv_obj_t * card_left = lv_obj_create(s_modal);
    lv_obj_set_size(card_left, SX(476), SY(510));
    lv_obj_set_pos(card_left, SX(20), SY(74));
    lv_obj_set_style_bg_color(card_left, lv_color_hex(0x132135), 0);
    lv_obj_set_style_radius(card_left, 16, 0);
    lv_obj_set_style_border_width(card_left, 0, 0);
    lv_obj_set_style_pad_all(card_left, SX(20), 0);
    lv_obj_clear_flag(card_left, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_status = lv_label_create(card_left);
    lv_obj_set_style_text_font(s_lbl_status, SF(18), 0);
    lv_label_set_text(s_lbl_status, tr("Verbinden...", "Connecting..."));
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_LEFT, 0, SY(4));

    s_lbl_title = lv_label_create(card_left);
    lv_obj_set_style_text_font(s_lbl_title, SF(24), 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_width(s_lbl_title, SX(436));
    lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_lbl_title, "...");
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_LEFT, 0, SY(38));

    s_lbl_artist = lv_label_create(card_left);
    lv_obj_set_style_text_font(s_lbl_artist, SF(20), 0);
    lv_obj_set_style_text_color(s_lbl_artist, lv_color_hex(0x66ccff), 0);
    lv_obj_set_width(s_lbl_artist, SX(436));
    lv_label_set_long_mode(s_lbl_artist, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_artist, "");
    lv_obj_align(s_lbl_artist, LV_ALIGN_TOP_LEFT, 0, SY(104));

    s_lbl_album = lv_label_create(card_left);
    lv_obj_set_style_text_font(s_lbl_album, SF(16), 0);
    lv_obj_set_style_text_color(s_lbl_album, lv_color_hex(0x88aabb), 0);
    lv_obj_set_width(s_lbl_album, SX(436));
    lv_label_set_long_mode(s_lbl_album, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_album, "");
    lv_obj_align(s_lbl_album, LV_ALIGN_TOP_LEFT, 0, SY(136));

    /* Transport Buttons Row */
    lv_obj_t * tbar = lv_obj_create(card_left);
    lv_obj_set_size(tbar, SX(436), SY(70));
    lv_obj_align(tbar, LV_ALIGN_TOP_MID, 0, SY(184));
    lv_obj_set_style_bg_opa(tbar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tbar, 0, 0);
    lv_obj_set_style_pad_all(tbar, 0, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * btn_prev = lv_btn_create(tbar);
    lv_obj_set_size(btn_prev, SX(80), SY(56));
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x1d3554), 0);
    lv_obj_set_style_radius(btn_prev, 12, 0);
    lv_obj_add_event_cb(btn_prev, on_prev_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * pl = lv_label_create(btn_prev);
    lv_label_set_text(pl, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(pl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(pl, SF(22), 0);
    lv_obj_center(pl);

    s_btn_play = lv_btn_create(tbar);
    lv_obj_set_size(s_btn_play, SX(110), SY(60));
    lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(0x2a5d96), 0);
    lv_obj_set_style_radius(s_btn_play, 14, 0);
    lv_obj_add_event_cb(s_btn_play, on_play_pause_click, LV_EVENT_CLICKED, NULL);
    s_lbl_play = lv_label_create(s_btn_play);
    lv_label_set_text(s_lbl_play, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_lbl_play, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_play, SF(26), 0);
    lv_obj_center(s_lbl_play);

    lv_obj_t * btn_next = lv_btn_create(tbar);
    lv_obj_set_size(btn_next, SX(80), SY(56));
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x1d3554), 0);
    lv_obj_set_style_radius(btn_next, 12, 0);
    lv_obj_add_event_cb(btn_next, on_next_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * nl = lv_label_create(btn_next);
    lv_label_set_text(nl, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(nl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(nl, SF(22), 0);
    lv_obj_center(nl);

    /* Volume Section */
    s_lbl_vol = lv_label_create(card_left);
    lv_obj_set_style_text_font(s_lbl_vol, SF(18), 0);
    lv_obj_set_style_text_color(s_lbl_vol, lv_color_hex(0xcdd9e6), 0);
    lv_label_set_text(s_lbl_vol, tr("Volume", "Volume"));
    lv_obj_align(s_lbl_vol, LV_ALIGN_TOP_LEFT, 0, SY(280));

    lv_obj_t * vbar = lv_obj_create(card_left);
    lv_obj_set_size(vbar, SX(436), SY(60));
    lv_obj_align(vbar, LV_ALIGN_TOP_MID, 0, SY(310));
    lv_obj_set_style_bg_opa(vbar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vbar, 0, 0);
    lv_obj_set_style_pad_all(vbar, 0, 0);
    lv_obj_clear_flag(vbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(vbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * btn_vdn = lv_btn_create(vbar);
    lv_obj_set_size(btn_vdn, SX(56), SY(48));
    lv_obj_set_style_bg_color(btn_vdn, lv_color_hex(0x1d3554), 0);
    lv_obj_set_style_radius(btn_vdn, 10, 0);
    lv_obj_add_event_cb(btn_vdn, on_vol_down_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * vdl = lv_label_create(btn_vdn);
    lv_label_set_text(vdl, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(vdl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(vdl, SF(20), 0);
    lv_obj_center(vdl);

    s_slider_vol = lv_slider_create(vbar);
    lv_obj_set_size(s_slider_vol, SX(280), SY(20));
    lv_slider_set_range(s_slider_vol, 0, 100);
    lv_obj_set_style_bg_color(s_slider_vol, lv_color_hex(0x22354e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_vol, lv_color_hex(0x44aaff), LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_slider_vol, on_vol_slider_event, LV_EVENT_ALL, NULL);

    lv_obj_t * btn_vup = lv_btn_create(vbar);
    lv_obj_set_size(btn_vup, SX(56), SY(48));
    lv_obj_set_style_bg_color(btn_vup, lv_color_hex(0x1d3554), 0);
    lv_obj_set_style_radius(btn_vup, 10, 0);
    lv_obj_add_event_cb(btn_vup, on_vol_up_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * vul = lv_label_create(btn_vup);
    lv_label_set_text(vul, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(vul, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(vul, SF(20), 0);
    lv_obj_center(vul);

    /* --- Right Card: Favorites & Radio --- */
    lv_obj_t * card_right = lv_obj_create(s_modal);
    lv_obj_set_size(card_right, SX(490), SY(510));
    lv_obj_set_pos(card_right, SX(514), SY(74));
    lv_obj_set_style_bg_color(card_right, lv_color_hex(0x132135), 0);
    lv_obj_set_style_radius(card_right, 16, 0);
    lv_obj_set_style_border_width(card_right, 0, 0);
    lv_obj_set_style_pad_all(card_right, SX(16), 0);
    lv_obj_clear_flag(card_right, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * r_head = lv_label_create(card_right);
    lv_obj_set_style_text_font(r_head, SF(22), 0);
    lv_obj_set_style_text_color(r_head, lv_color_hex(0xffffff), 0);
    lv_label_set_text(r_head, tr("Favorieten & Radio", "Favorites & Radio"));
    lv_obj_align(r_head, LV_ALIGN_TOP_LEFT, SX(6), SY(4));

    lv_obj_t * b_ref = lv_btn_create(card_right);
    lv_obj_set_size(b_ref, SX(48), SY(40));
    lv_obj_align(b_ref, LV_ALIGN_TOP_RIGHT, SX(-6), SY(0));
    lv_obj_set_style_bg_color(b_ref, lv_color_hex(0x1d3554), 0);
    lv_obj_set_style_radius(b_ref, 8, 0);
    lv_obj_add_event_cb(b_ref, on_fav_refresh_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * rl = lv_label_create(b_ref);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(rl, lv_color_hex(0xffffff), 0);
    lv_obj_center(rl);

    s_lbl_fav_msg = lv_label_create(card_right);
    lv_obj_set_style_text_font(s_lbl_fav_msg, SF(16), 0);
    lv_obj_set_style_text_color(s_lbl_fav_msg, lv_color_hex(0x66ccff), 0);
    lv_label_set_text(s_lbl_fav_msg, tr("Laden...", "Loading..."));
    lv_obj_align(s_lbl_fav_msg, LV_ALIGN_TOP_LEFT, SX(6), SY(36));

    s_fav_list = lv_obj_create(card_right);
    lv_obj_set_size(s_fav_list, SX(466), SY(426));
    lv_obj_set_pos(s_fav_list, 0, SY(56));
    lv_obj_set_style_bg_opa(s_fav_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_fav_list, 0, 0);
    lv_obj_set_style_pad_all(s_fav_list, 0, 0);
    lv_obj_set_style_pad_row(s_fav_list, 8, 0);
    lv_obj_add_flag(s_fav_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_fav_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_fav_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(s_fav_list, LV_FLEX_FLOW_COLUMN);

    /* Start background favorite load */
    trigger_load_favorites();

    /* Timer for periodic state update */
    s_refresh_timer = lv_timer_create(sonos_modal_tick_cb, 500, NULL);
    sonos_modal_tick_cb(s_refresh_timer);
}

