/*
 * update_modal.c — Modular About / Update modal and live install-progress dialog.
 * Extracted from screen_home.c to allow unified use across screens (Home, Settings, etc.).
 */

#include "update_modal.h"
#include "update_check.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef BUILD_VERSION
#define BUILD_VERSION "dev"
#endif

#define UPD_LOG_PATH "/var/volatile/tmp/selfinstall.log"
#define UPD_STEP_COUNT 6

static const char * const UPD_STEPS[UPD_STEP_COUNT] = {
    "Nieuwste versie opzoeken",
    "Binary downloaden",
    "Download controleren",
    "Helpers + assets",
    "Binary plaatsen",
    "UI herstarten",
};

static const char * const UPD_STEPS_EN[UPD_STEP_COUNT] = {
    "Find latest version",
    "Download binary",
    "Verify download",
    "Helpers + assets",
    "Place binary",
    "Restart UI",
};

/* Modal state */
static lv_obj_t *  s_update_modal            = NULL;
static lv_obj_t *  s_about_status_lbl        = NULL;
static lv_timer_t *s_modal_timer             = NULL;
static int         s_update_modal_has_install = 0;
static int         s_install_pinned          = 0;

/* Banner tracking state */
static char        s_skipped_version[UPDATE_VERSION_MAX] = "";
static bool        s_update_minimized        = false;

/* Install progress modal state */
static lv_obj_t *  s_upd_prog_modal          = NULL;
static lv_obj_t *  s_upd_prog_row[UPD_STEP_COUNT] = {0};
static lv_obj_t *  s_upd_prog_msg            = NULL;
static lv_timer_t *s_upd_prog_timer          = NULL;

/* -------------------------------------------------------------------------
 * Version comparisons and state queries
 * ------------------------------------------------------------------------- */

bool update_modal_already_installed(void) {
    const char * t = g_update_state.latest_version;
    if (!t[0]) return false;
    size_t tn = strcspn(t, "-");
    size_t bn = strcspn(BUILD_VERSION, "-");
    return (tn == bn && strncmp(t, BUILD_VERSION, tn) == 0);
}

bool update_modal_is_open(void) {
    return (s_update_modal != NULL);
}

bool update_modal_is_minimized(void) {
    return s_update_minimized;
}

void update_modal_set_minimized(bool min) {
    s_update_minimized = min;
}

const char * update_modal_get_skipped_version(void) {
    return s_skipped_version;
}

void update_modal_set_skipped_version(const char * ver) {
    if (ver) {
        snprintf(s_skipped_version, sizeof(s_skipped_version), "%s", ver);
    } else {
        s_skipped_version[0] = '\0';
    }
}

/* -------------------------------------------------------------------------
 * Install progress dialog
 * ------------------------------------------------------------------------- */

static void upd_prog_close(lv_event_t * e) {
    (void)e;
    if (s_upd_prog_timer) {
        lv_timer_del(s_upd_prog_timer);
        s_upd_prog_timer = NULL;
    }
    if (s_upd_prog_modal) {
        lv_obj_del_async(s_upd_prog_modal);
        s_upd_prog_modal = NULL;
    }
    for (int i = 0; i < UPD_STEP_COUNT; i++) s_upd_prog_row[i] = NULL;
    s_upd_prog_msg = NULL;
}

static void upd_row_set(int i, const char * mark, lv_color_t col) {
    if (!s_upd_prog_row[i]) return;
    lv_label_set_text_fmt(s_upd_prog_row[i], "%s  %s", mark, tr(UPD_STEPS[i], UPD_STEPS_EN[i]));
    lv_obj_set_style_text_color(s_upd_prog_row[i], col, 0);
}

static void upd_prog_tick(lv_timer_t * t) {
    (void)t;
    FILE * f = fopen(UPD_LOG_PATH, "r");
    if (!f) return;
    char line[256];
    int cur = 0, failed = -1;
    char failmsg[160] = "";
    while (fgets(line, sizeof line, f)) {
        int n;
        if (sscanf(line, "@@STEP %d/", &n) == 1) {
            if (n > cur) cur = n;
        } else if (strncmp(line, "@@FAIL ", 7) == 0) {
            failed = cur > 0 ? cur : 1;
            snprintf(failmsg, sizeof failmsg, "%.*s", (int)sizeof(failmsg) - 1, line + 7);
            size_t L = strlen(failmsg);
            while (L && (failmsg[L - 1] == '\n' || failmsg[L - 1] == '\r')) failmsg[--L] = 0;
        }
    }
    fclose(f);

    for (int i = 0; i < UPD_STEP_COUNT; i++) {
        if (failed > 0 && i == failed - 1)      upd_row_set(i, LV_SYMBOL_CLOSE, lv_color_hex(0xff5555));
        else if (i < cur - 1)                   upd_row_set(i, LV_SYMBOL_OK,    lv_color_hex(0x66dd88));
        else if (i == cur - 1 && cur > 0)       upd_row_set(i, LV_SYMBOL_RIGHT, lv_color_hex(0xffd24a));
        else                                    upd_row_set(i, "-",             lv_color_hex(0x6b7c93));
    }

    if (s_upd_prog_msg) {
        if (failed > 0) {
            lv_label_set_text_fmt(s_upd_prog_msg,
                                  tr("#ff7777 Mislukt bij stap %d:# %s",
                                     "#ff7777 Failed at step %d:# %s"),
                                  failed, failmsg[0] ? failmsg : tr("(zie log)", "(see log)"));
        } else if (cur >= UPD_STEP_COUNT) {
            lv_label_set_text(s_upd_prog_msg,
                              tr("Herstarten... het scherm komt zo terug.",
                                 "Restarting... the screen will return shortly."));
        } else {
            lv_label_set_text(s_upd_prog_msg,
                              tr("Bezig met installeren...", "Installing..."));
        }
    }
}

static void do_install_now(lv_event_t * e) {
    (void)e;
    s_install_pinned = 1;
    if (update_modal_already_installed()) {
        if (s_about_status_lbl) {
            lv_label_set_text_fmt(s_about_status_lbl,
                tr("Al op %s - niets te installeren.",
                   "Already on %s - nothing to install."), BUILD_VERSION);
        }
        return;
    }
    if (s_about_status_lbl) {
        lv_label_set_text(s_about_status_lbl,
                          tr("Installeren... het scherm herstart zo.",
                             "Installing... the screen will restart shortly."));
    }

    if (!s_upd_prog_modal) {
        s_upd_prog_modal = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(s_upd_prog_modal);
        lv_obj_set_size(s_upd_prog_modal, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_color(s_upd_prog_modal, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_upd_prog_modal, LV_OPA_70, 0);
        lv_obj_add_flag(s_upd_prog_modal, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t * card = lv_obj_create(s_upd_prog_modal);
        lv_obj_set_size(card, SX(620), SY(460));
        lv_obj_center(card);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x16243a), 0);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * h = lv_label_create(card);
        lv_obj_set_style_text_font(h, SF(28), 0);
        lv_obj_set_style_text_color(h, lv_color_hex(0xffffff), 0);
        lv_label_set_text(h, tr("Update installeren", "Installing update"));
        lv_obj_align(h, LV_ALIGN_TOP_LEFT, SX(24), SY(18));

        for (int i = 0; i < UPD_STEP_COUNT; i++) {
            s_upd_prog_row[i] = lv_label_create(card);
            lv_obj_set_style_text_font(s_upd_prog_row[i], SF(20), 0);
            lv_obj_align(s_upd_prog_row[i], LV_ALIGN_TOP_LEFT, SX(28), SY(70 + i * 38));
            upd_row_set(i, "-", lv_color_hex(0x6b7c93));
        }

        s_upd_prog_msg = lv_label_create(card);
        lv_obj_set_style_text_font(s_upd_prog_msg, SF(18), 0);
        lv_obj_set_style_text_color(s_upd_prog_msg, lv_color_hex(0xcdd9e6), 0);
        lv_label_set_recolor(s_upd_prog_msg, true);
        lv_label_set_long_mode(s_upd_prog_msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_upd_prog_msg, SX(560));
        lv_obj_align(s_upd_prog_msg, LV_ALIGN_TOP_LEFT, SX(28), SY(70 + UPD_STEP_COUNT * 38 + 6));
        lv_label_set_text(s_upd_prog_msg, tr("Bezig met installeren...", "Installing..."));

        lv_obj_t * x = lv_btn_create(card);
        lv_obj_set_size(x, SX(130), SY(46));
        lv_obj_align(x, LV_ALIGN_BOTTOM_RIGHT, SX(-16), SY(-12));
        lv_obj_add_event_cb(x, upd_prog_close, LV_EVENT_CLICKED, NULL);
        lv_obj_t * xl = lv_label_create(x);
        lv_label_set_text(xl, tr("Sluiten", "Close"));
        lv_obj_center(xl);
    }

    system("nohup sh -c ': > " UPD_LOG_PATH "; sleep 1; curl -fsSL -4 "
           "https://raw.githubusercontent.com/ToonzDev/freetoon-lvgl/main/install.sh "
           "| env FORCE_DOWNLOAD=1 sh -s -- --update' >> " UPD_LOG_PATH " 2>&1 &");

    if (!s_upd_prog_timer) {
        s_upd_prog_timer = lv_timer_create(upd_prog_tick, 400, NULL);
    }
}

/* -------------------------------------------------------------------------
 * Event handlers for Update / About modal
 * ------------------------------------------------------------------------- */

static void on_modal_close_event(lv_event_t * e) {
    (void)e;
    update_modal_close();
}

static void do_skip_version(lv_event_t * e) {
    (void)e;
    update_modal_set_skipped_version(g_update_state.latest_version);
    s_update_minimized = false;
    update_modal_close();
}

static void do_dismiss_to_envelope(lv_event_t * e) {
    (void)e;
    s_update_minimized = true;
    update_modal_close();
}

static void on_auto_update_toggle(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    settings.auto_update_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
}

static void * check_thread(void * arg) {
    (void)arg;
    update_check_now();
    return NULL;
}

static void do_check_updates(lv_event_t * e) {
    (void)e;
    if (s_about_status_lbl) {
        lv_label_set_text(s_about_status_lbl,
            tr("Updates zoeken...", "Checking for updates..."));
    }
    pthread_t t;
    if (pthread_create(&t, NULL, check_thread, NULL) == 0) {
        pthread_detach(t);
    }
}

static void on_channel_toggle(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    settings.update_channel = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
    if (s_about_status_lbl) {
        lv_label_set_text(s_about_status_lbl,
            settings.update_channel ? tr("Kanaal: beta/dev - zoeken...",
                                         "Channel: beta/dev - checking...")
                                    : tr("Kanaal: stabiel - zoeken...",
                                         "Channel: stable - checking..."));
    }
    pthread_t t;
    if (pthread_create(&t, NULL, check_thread, NULL) == 0) {
        pthread_detach(t);
    }
}

/* -------------------------------------------------------------------------
 * Periodic tick handler
 * ------------------------------------------------------------------------- */

void update_modal_tick(void) {
    if (s_about_status_lbl && !s_install_pinned) {
        if (g_update_state.available) {
            lv_label_set_text_fmt(s_about_status_lbl,
                                  tr("Update %s beschikbaar", "Update %s available"),
                                  g_update_state.latest_version);
        } else if (g_update_state.last_check_epoch) {
            lv_label_set_text(s_about_status_lbl, g_update_state.last_check_ok
                              ? tr("Je hebt de nieuwste versie.", "You're on the latest release.")
                              : tr("Updatecontrole mislukt - check internet.", "Update check failed - check internet."));
        } else {
            lv_label_set_text(s_about_status_lbl, tr("Tik \"Controleer updates\".", "Tap \"Check for updates\"."));
        }
    }

    if (s_update_modal && g_update_state.available && !s_update_modal_has_install && !s_install_pinned) {
        lv_obj_t * parent = lv_obj_get_parent(s_update_modal);
        update_modal_open(parent);
    }
}

static void modal_timer_cb(lv_timer_t * t) {
    (void)t;
    update_modal_tick();
}

/* -------------------------------------------------------------------------
 * Open / Close modal
 * ------------------------------------------------------------------------- */

void update_modal_close(void) {
    s_about_status_lbl = NULL;
    s_install_pinned = 0;
    if (s_modal_timer) {
        lv_timer_del(s_modal_timer);
        s_modal_timer = NULL;
    }
    if (s_update_modal) {
        lv_obj_del_async(s_update_modal);
        s_update_modal = NULL;
    }
}

void update_modal_open(lv_obj_t * parent) {
    if (s_update_modal) {
        lv_obj_del_async(s_update_modal);
        s_update_modal = NULL;
    }
    if (s_modal_timer) {
        lv_timer_del(s_modal_timer);
        s_modal_timer = NULL;
    }

    lv_obj_t * root = parent ? parent : lv_scr_act();
    s_update_modal = lv_obj_create(root);
    lv_obj_remove_style_all(s_update_modal);
    lv_obj_set_size(s_update_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_update_modal, 0, 0);
    lv_obj_set_style_bg_color(s_update_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_update_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(s_update_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_update_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_update_modal, on_modal_close_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t * panel = lv_obj_create(s_update_modal);
    lv_obj_set_size(panel, SX(760), SY(470));
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x16243a), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 22, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    /* Logo mark + title */
    lv_obj_t * mark = lv_obj_create(panel);
    lv_obj_set_size(mark, 46, 46);
    lv_obj_align(mark, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(mark, lv_color_hex(0x2e6e9e), 0);
    lv_obj_set_style_radius(mark, 12, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * mk = lv_label_create(mark);
    lv_obj_set_style_text_color(mk, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(mk, SF(28), 0);
    lv_label_set_text(mk, "ft");
    lv_obj_center(mk);

    lv_obj_t * t = lv_label_create(panel);
    lv_obj_set_style_text_font(t, SF(28), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), 0);
    lv_label_set_text(t, "freetoon");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 60, 2);

    lv_obj_t * ver = lv_label_create(panel);
    lv_obj_set_style_text_font(ver, SF(18), 0);
    lv_obj_set_style_text_color(ver, lv_color_hex(0x88aabb), 0);
    lv_label_set_text_fmt(ver, tr("%s  -  beta", "%s  -  beta"), BUILD_VERSION);
    lv_obj_align(ver, LV_ALIGN_TOP_LEFT, 60, 34);

    /* Credit + license line */
    lv_obj_t * cred = lv_label_create(panel);
    lv_obj_set_style_text_font(cred, SF(14), 0);
    lv_obj_set_style_text_color(cred, lv_color_hex(0x6f8aa0), 0);
    lv_label_set_text(cred, tr("alternatieve UI door Ierlandfan  -  MIT-licentie",
                               "alternative UI by Ierlandfan  -  MIT License"));
    lv_obj_align(cred, LV_ALIGN_TOP_RIGHT, 0, 6);

    /* Live status line */
    s_about_status_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(s_about_status_lbl, SF(22), 0);
    lv_obj_set_style_text_color(s_about_status_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_width(s_about_status_lbl, SX(716));
    lv_label_set_long_mode(s_about_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_about_status_lbl, LV_ALIGN_TOP_LEFT, 0, SY(70));

    /* Release notes box */
    lv_obj_t * notesbox = lv_obj_create(panel);
    lv_obj_set_size(notesbox, SX(716), SY(230));
    lv_obj_align(notesbox, LV_ALIGN_TOP_LEFT, 0, SY(110));
    lv_obj_set_style_bg_color(notesbox, lv_color_hex(0x0e1a2a), 0);
    lv_obj_set_style_border_width(notesbox, 0, 0);
    lv_obj_set_style_radius(notesbox, 10, 0);
    lv_obj_set_style_pad_all(notesbox, 12, 0);
    lv_obj_set_scroll_dir(notesbox, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(notesbox, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t * notes = lv_label_create(notesbox);
    lv_obj_set_style_text_font(notes, SF(18), 0);
    lv_obj_set_style_text_color(notes, lv_color_hex(0xc8d4e0), 0);
    lv_label_set_long_mode(notes, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(notes, SX(690));
    lv_label_set_text(notes, g_update_state.changelog[0]
        ? g_update_state.changelog
        : g_update_state.release_notes[0]
        ? g_update_state.release_notes
        : "freetoon - an independent LVGL UI for the Eneco Toon, by Ierlandfan.\n"
          "Released under the MIT License.\n\n"
          "Thanks to Quby / Eneco for the underlying Toon platform and the\n"
          "BoxTalk / Quby protocol structure this UI builds on. The stock Toon\n"
          "binaries and keteladapter firmware remain (c) Eneco / Quby and are\n"
          "not redistributed or modified by this project.\n\n"
          "Built with open-source components:\n"
          "  - LVGL - embedded UI library (MIT) (c) LVGL Kft\n"
          "  - QR-Code-generator (MIT) (c) Project Nayuki\n"
          "  - LodePNG (zlib) (c) Lode Vandevenne\n"
          "  - OTGW HTTP firmware (c) Robert van den Breemen\n"
          "  - Itho-WiFi REST add-on (c) Arjen Hiemstra\n"
          "  - HomeWizard P1, Buienradar, NOS feeds - public APIs\n\n"
          "An update's release notes replace this text when a newer version\n"
          "is found.");

    /* Auto-update toggle */
    lv_obj_t * au_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(au_lbl, SF(18), 0);
    lv_obj_set_style_text_color(au_lbl, lv_color_hex(0xc8d4e0), 0);
    lv_label_set_text_fmt(au_lbl, tr("Auto-update 's nachts (~%02d:00)",
                                     "Auto-update nightly (~%02d:00)"), settings.auto_update_hour);
    lv_obj_align(au_lbl, LV_ALIGN_TOP_LEFT, 0, SY(350));
    lv_obj_t * au_sw = lv_switch_create(panel);
    lv_obj_align(au_sw, LV_ALIGN_TOP_LEFT, SX(320), SY(344));
    if (settings.auto_update_enabled) lv_obj_add_state(au_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(au_sw, on_auto_update_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    /* Release channel toggle */
    lv_obj_t * ch_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(ch_lbl, SF(18), 0);
    lv_obj_set_style_text_color(ch_lbl, lv_color_hex(0xc8d4e0), 0);
    lv_label_set_text(ch_lbl, tr("Beta / dev", "Beta / dev"));
    lv_obj_align(ch_lbl, LV_ALIGN_TOP_LEFT, SX(430), SY(350));
    lv_obj_t * ch_sw = lv_switch_create(panel);
    lv_obj_align(ch_sw, LV_ALIGN_TOP_LEFT, SX(600), SY(344));
    if (settings.update_channel) lv_obj_add_state(ch_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(ch_sw, on_channel_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    /* Button row */
    lv_obj_t * b_check = lv_btn_create(panel);
    lv_obj_set_size(b_check, SX(180), SY(56));
    lv_obj_align(b_check, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(b_check, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_check, do_check_updates, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(b_check);
    lv_obj_set_style_text_font(cl, SF(18), 0);
    lv_label_set_text(cl, tr("Controleer", "Check"));
    lv_obj_center(cl);

    s_update_modal_has_install = g_update_state.available ? 1 : 0;
    if (g_update_state.available) {
        lv_obj_t * b_inst = lv_btn_create(panel);
        lv_obj_set_size(b_inst, SX(150), SY(56));
        lv_obj_align(b_inst, LV_ALIGN_BOTTOM_LEFT, SX(188), 0);
        lv_obj_set_style_bg_color(b_inst, lv_color_hex(0x2e6e3a), 0);
        lv_obj_add_event_cb(b_inst, do_install_now, LV_EVENT_CLICKED, NULL);
        lv_obj_t * il = lv_label_create(b_inst);
        lv_obj_set_style_text_font(il, SF(18), 0);
        lv_label_set_text(il, tr("Nu installeren", "Install now"));
        lv_obj_center(il);

        lv_obj_t * b_skip = lv_btn_create(panel);
        lv_obj_set_size(b_skip, SX(96), SY(56));
        lv_obj_align(b_skip, LV_ALIGN_BOTTOM_LEFT, SX(346), 0);
        lv_obj_set_style_bg_color(b_skip, lv_color_hex(0x6a5424), 0);
        lv_obj_add_event_cb(b_skip, do_skip_version, LV_EVENT_CLICKED, NULL);
        lv_obj_t * sl = lv_label_create(b_skip);
        lv_obj_set_style_text_font(sl, SF(18), 0);
        lv_label_set_text(sl, tr("Overslaan", "Skip"));
        lv_obj_center(sl);

        lv_obj_t * b_dis = lv_btn_create(panel);
        lv_obj_set_size(b_dis, SX(130), SY(56));
        lv_obj_align(b_dis, LV_ALIGN_BOTTOM_LEFT, SX(450), 0);
        lv_obj_set_style_bg_color(b_dis, lv_color_hex(0x33445a), 0);
        lv_obj_add_event_cb(b_dis, do_dismiss_to_envelope, LV_EVENT_CLICKED, NULL);
        lv_obj_t * dl = lv_label_create(b_dis);
        lv_obj_set_style_text_font(dl, SF(18), 0);
        lv_label_set_text(dl, tr(LV_SYMBOL_ENVELOPE " Verbergen", LV_SYMBOL_ENVELOPE " Dismiss"));
        lv_obj_center(dl);
    }

    lv_obj_t * x = lv_btn_create(panel);
    lv_obj_set_size(x, SX(120), SY(56));
    lv_obj_align(x, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(0x33445a), 0);
    lv_obj_add_event_cb(x, on_modal_close_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(x);
    lv_obj_set_style_text_font(xl, SF(22), 0);
    lv_label_set_text(xl, tr("Sluiten", "Close"));
    lv_obj_center(xl);

    /* Initial status update and schedule timer */
    update_modal_tick();
    s_modal_timer = lv_timer_create(modal_timer_cb, 500, NULL);
}
