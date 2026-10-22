/*
 * Marketplace screen — browses the catalog at
 *   https://raw.githubusercontent.com/ToonSoftwareCollective/freetoon-integrations/main/catalog/index.json
 * and lets the user install or uninstall integrations on-device with one tap.
 *
 * Install path: tap "Install" → runs `/mnt/data/integrations-install.sh <id>`
 * Uninstall path: tap "Verwijderen" → runs `/mnt/data/integrations-install.sh remove <id>`
 * via system() in a background thread.
 *
 * The catalog parse is intentionally brittle (grep + sed-style on the JSON
 * text) because we run on a busybox userland with no jq and don't want to
 * pull in cJSON for a single screen. Catalog shape is fixed enough that
 * "find next id/name/description/version/url" walks reliably.
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "http.h"
#include "settings.h"
#include "theme.h"
#include "icons.h"
#include "tile_slots.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CATALOG_URL "https://raw.githubusercontent.com/ToonzDev/freetoon-integrations/main/catalog/index.json"
#define INSTALL_HELPER "/mnt/data/integrations-install.sh"

#define COL_BG       THEME_BG
#define COL_CARD     THEME_CARD_BG
#define COL_TEXT_HI  THEME_TEXT_HI
#define COL_TEXT_DIM THEME_TEXT_DIM
#define COL_OK       (settings.home_light_mode ? 0x16a34a : 0x2e6e3a)
#define COL_BUSY     (settings.home_light_mode ? 0xd97706 : 0x6a5424)
#define COL_WARN     (settings.home_light_mode ? 0xdc2626 : 0x6e3a3a)

static lv_obj_t * scr_root = NULL;
static lv_obj_t * list_box = NULL;
static lv_obj_t * status_lbl = NULL;
static char       catalog_buf[16384];
static int        catalog_len = 0;

/* One row of the rendered list. */
typedef struct {
    char id[64];
    char name[64];
    char description[256];
    char version[32];
    lv_obj_t * row;
    lv_obj_t * badge_lbl;
    lv_obj_t * btn_action;
    lv_obj_t * btn_action_lbl;
    volatile int action_status; /* 0=idle, 1=installing, 2=uninstalling, 3=err_install, 4=err_uninstall */
    int is_installed;
} integration_t;

#define MAX_INTEGRATIONS 16
static integration_t entries[MAX_INTEGRATIONS];
static int           entry_count = 0;
static char          g_fetch_msg[128] = "";
static volatile int  g_fetch_dirty = 0;

/* Built-in fallback catalog items in case remote catalog is unreachable */
static const struct {
    const char * id;
    const char * name;
    const char * version;
    const char * description;
} g_builtin_catalog[] = {
    { "temperaturelogger", "Temperature Logger", "1.0.0",
      "24-uurs temperatuurlogger met grafiek (kamer, setpoint, buiten)." },
    { "crypto", "Crypto Tracker", "1.0.0",
      "Live crypto koersen en 24h verandering via CoinGecko." },
    { "sonos", "Sonos Player", "1.0.0",
      "Native Sonos UPnP / SOAP controller met favorieten, radio en volume." },
    { "domoticz", "Domoticz Home Automation", "1.0.0",
      "Schakelaars, dimmers en zonwering via Domoticz HTTP JSON API." },
    { "homeassistant", "Home Assistant", "1.0.0",
      "Verbindt FreeToon met Home Assistant via REST & WebSocket." },
    { "agenda", "Agenda & Kalender", "1.0.0",
      "Synchroniseert afspraken uit Home Assistant en iCal/Google Calendar." },
    { "ventilation", "Itho Ventilatie", "1.0.0",
      "Itho CVE-S ventilatieregeling via MQTT en virtuele afstandsbediening." },
    { "camera", "Deurbel & Camera", "1.0.0",
      "Deurbel- en camerameldingen via webhook op poort 8765 en live video overlay." },
};

/* ===================================================================== */
/* JSON walker — finds the next "field" : "value" or "field": value pair. */
/* ===================================================================== */
static const char * find_json_str(const char * src, const char * key,
                                  char * out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char * p = strstr(src, pat);
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\n' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outsz) {
        if (*p == '\\' && p[1]) { p++; }   /* tolerate basic escapes */
        out[n++] = *p++;
    }
    out[n] = 0;
    return p;
}

/* Map unsupported Unicode glyphs from catalog JSON to ASCII equivalents */
static void sanitize_unicode_to_ascii(char * s) {
    if (!s) return;
    char * r = s;
    char * w = s;
    while (*r) {
        if ((unsigned char)r[0] == 0xE2 && (unsigned char)r[1] == 0x80 && (unsigned char)r[2] == 0x94) {
            *w++ = '-';
            r += 3;
        } else if ((unsigned char)r[0] == 0xE2 && (unsigned char)r[1] == 0x80 && (unsigned char)r[2] == 0xA6) {
            *w++ = '.'; *w++ = '.'; *w++ = '.';
            r += 3;
        } else if ((unsigned char)r[0] == 0xE2 && (unsigned char)r[1] == 0x86 && (unsigned char)r[2] == 0x92) {
            *w++ = '-'; *w++ = '>';
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Parse catalog_buf into entries[]. Tolerant of formatting and missing
 * fields — anything we couldn't find stays as empty string. */
static void parse_catalog(void) {
    entry_count = 0;
    const char * scan = strstr(catalog_buf, "\"integrations\"");
    if (!scan) return;
    while (entry_count < MAX_INTEGRATIONS) {
        const char * next = strstr(scan, "\"id\"");
        if (!next) break;
        integration_t * e = &entries[entry_count];
        memset(e, 0, sizeof *e);
        const char * after_id = find_json_str(next, "id", e->id, sizeof e->id);
        if (!after_id) break;
        find_json_str(next, "name",        e->name,        sizeof e->name);
        find_json_str(next, "description", e->description, sizeof e->description);
        find_json_str(next, "version",     e->version,     sizeof e->version);
        sanitize_unicode_to_ascii(e->name);
        sanitize_unicode_to_ascii(e->description);
        entry_count++;
        scan = after_id;   /* move past this id so the next pass picks the next */
    }
}

/* ===================================================================== */
/* Background fetcher                                                    */
/* ===================================================================== */
static void * fetch_thread(void * arg) {
    (void)arg;
    snprintf(g_fetch_msg, sizeof(g_fetch_msg), "%s", tr("Catalogus ophalen...", "Fetching catalog..."));
    g_fetch_dirty = 1;
    /* Try to fetch online catalog */
    int rc = http_fetch(CATALOG_URL, catalog_buf, sizeof catalog_buf);
    catalog_len = (int)strlen(catalog_buf);
    if (rc == 0 && catalog_len > 0) {
        parse_catalog();
    }

    /* Merge standard built-in catalog fallbacks so uninstalled apps don't vanish */
    for (size_t k = 0; k < sizeof(g_builtin_catalog)/sizeof(g_builtin_catalog[0]) && entry_count < MAX_INTEGRATIONS; k++) {
        int exists = 0;
        for (int j = 0; j < entry_count; j++) {
            if (strcmp(entries[j].id, g_builtin_catalog[k].id) == 0) { exists = 1; break; }
        }
        if (!exists) {
            integration_t * e = &entries[entry_count];
            memset(e, 0, sizeof *e);
            snprintf(e->id, sizeof e->id, "%s", g_builtin_catalog[k].id);
            snprintf(e->name, sizeof e->name, "%s", g_builtin_catalog[k].name);
            snprintf(e->version, sizeof e->version, "%s", g_builtin_catalog[k].version);
            snprintf(e->description, sizeof e->description, "%s", g_builtin_catalog[k].description);
            entry_count++;
        }
    }

    /* Merge any additional locally installed integrations from tile_slots */
    int n = tile_slots_integration_count();
    for (int i = 0; i < n && entry_count < MAX_INTEGRATIONS; i++) {
        const integration_meta_t * meta = tile_slots_integration_at(i);
        if (!meta) continue;
        int exists = 0;
        for (int j = 0; j < entry_count; j++) {
            if (strcmp(entries[j].id, meta->id) == 0) { exists = 1; break; }
        }
        if (exists) continue;

        integration_t * e = &entries[entry_count];
        memset(e, 0, sizeof *e);
        snprintf(e->id, sizeof e->id, "%s", meta->id);
        snprintf(e->name, sizeof e->name, "%s", meta->name[0] ? meta->name : meta->id);
        if (strcmp(meta->id, "temperaturelogger") == 0) {
            snprintf(e->description, sizeof e->description, "%s",
                     "24-uurs temperatuurlogger met grafiek (kamer, setpoint, buiten).");
        } else if (strcmp(meta->id, "crypto") == 0) {
            snprintf(e->description, sizeof e->description, "%s",
                     "Crypto tracker via CoinGecko API.");
        } else {
            snprintf(e->description, sizeof e->description, "%s", meta->tile_title);
        }
        snprintf(e->version, sizeof e->version, "1.0.0");
        entry_count++;
    }

    snprintf(g_fetch_msg, sizeof(g_fetch_msg),
             tr("%d integraties beschikbaar", "%d integrations available"),
             entry_count);
    g_fetch_dirty = 1;
    return NULL;
}

/* ===================================================================== */
/* Install / Uninstall actions                                           */
/* ===================================================================== */
static int install_helper_ok(void) {
    struct stat st;
    return stat(INSTALL_HELPER, &st) == 0 && (st.st_mode & S_IXUSR);
}

#ifdef WASM_BUILD
/* WASM client has no shell — route through master Toon */
extern void wasm_push_event(const char * topic, const char * payload);
#endif

static void * install_thread(void * arg) {
    integration_t * e = (integration_t *)arg;
#ifdef WASM_BUILD
    char payload[96];
    snprintf(payload, sizeof payload, "{\"id\":\"%s\"}", e->id);
    wasm_push_event("/api/install", payload);
    e->action_status = 0;
    return NULL;
#else
    char cmd[256];
    snprintf(cmd, sizeof cmd, "%s %s", INSTALL_HELPER, e->id);
    int rc = system(cmd);
    tile_slots_init();
    e->action_status = (rc == 0) ? 0 : 3;
    return NULL;
#endif
}

static void * uninstall_thread(void * arg) {
    integration_t * e = (integration_t *)arg;
#ifdef WASM_BUILD
    char payload[96];
    snprintf(payload, sizeof payload, "{\"id\":\"%s\"}", e->id);
    wasm_push_event("/api/uninstall", payload);
    e->action_status = 0;
    return NULL;
#else
    char cmd[256];
    snprintf(cmd, sizeof cmd, "%s remove %s", INSTALL_HELPER, e->id);
    int rc = system(cmd);

    /* Safety fallback: ensure integration dir is cleared */
    char path[128];
    snprintf(path, sizeof path, "/mnt/data/integrations/%s", e->id);
    struct stat st;
    if (stat(path, &st) == 0) {
        snprintf(cmd, sizeof cmd, "rm -rf /mnt/data/integrations/%s", e->id);
        system(cmd);
    }

    /* Unbind from any tile slot in settings if currently bound */
    for (int s = 0; s < TILE_SLOT_COUNT; s++) {
        const char * b = tile_slots_binding(s);
        if (b && (strcmp(b, e->id) == 0 || (strncmp(b, "local:", 6) == 0 && strcmp(b + 6, e->id) == 0))) {
            tile_slots_bind(s, "");
        }
    }

    /* Rescan installed integrations so tile_slots drops it */
    tile_slots_init();

    if (rc == 0 || tile_slots_integration_by_id(e->id) == NULL) {
        e->action_status = 0;
    } else {
        e->action_status = 4;
    }
    return NULL;
#endif
}

static void on_action_clicked(lv_event_t * e) {
    integration_t * ent = (integration_t *)lv_event_get_user_data(e);
    if (!ent) return;
    if (ent->action_status != 0) return; /* busy */

    int is_inst = (tile_slots_integration_by_id(ent->id) != NULL);
    if (is_inst) {
#ifdef WASM_BUILD
        char payload[96];
        snprintf(payload, sizeof payload, "{\"id\":\"%s\"}", ent->id);
        wasm_push_event("/api/uninstall", payload);
        if (ent->btn_action_lbl) lv_label_set_text(ent->btn_action_lbl, tr("Naar master gestuurd", "Sent to master"));
#else
        if (!install_helper_ok()) {
            if (ent->btn_action_lbl)
                lv_label_set_text(ent->btn_action_lbl, tr("Helper nodig", "Need helper"));
            return;
        }
        ent->action_status = 2;
        if (ent->btn_action_lbl) lv_label_set_text(ent->btn_action_lbl, tr("Verwijderen...", "Removing..."));
        if (ent->btn_action)
            lv_obj_set_style_bg_color(ent->btn_action, lv_color_hex(COL_BUSY), 0);
        pthread_t t;
        if (pthread_create(&t, NULL, uninstall_thread, ent) == 0)
            pthread_detach(t);
        else
            ent->action_status = 4;
#endif
    } else {
#ifdef WASM_BUILD
        char payload[96];
        snprintf(payload, sizeof payload, "{\"id\":\"%s\"}", ent->id);
        wasm_push_event("/api/install", payload);
        if (ent->btn_action_lbl) lv_label_set_text(ent->btn_action_lbl, tr("Naar master gestuurd", "Sent to master"));
#else
        if (!install_helper_ok()) {
            if (ent->btn_action_lbl)
                lv_label_set_text(ent->btn_action_lbl, tr("Helper nodig", "Need helper"));
            return;
        }
        ent->action_status = 1;
        if (ent->btn_action_lbl) lv_label_set_text(ent->btn_action_lbl, tr("Installeren...", "Installing..."));
        if (ent->btn_action)
            lv_obj_set_style_bg_color(ent->btn_action, lv_color_hex(COL_BUSY), 0);
        pthread_t t;
        if (pthread_create(&t, NULL, install_thread, ent) == 0)
            pthread_detach(t);
        else
            ent->action_status = 3;
#endif
    }
}

static void update_row_state(integration_t * ent) {
    if (!ent || !ent->row) return;

    if (ent->action_status == 1) {
        if (ent->badge_lbl) lv_label_set_text(ent->badge_lbl, "");
        if (ent->btn_action_lbl) lv_label_set_text(ent->btn_action_lbl, tr("Installeren...", "Installing..."));
        if (ent->btn_action) lv_obj_set_style_bg_color(ent->btn_action, lv_color_hex(COL_BUSY), 0);
        return;
    }
    if (ent->action_status == 2) {
        if (ent->btn_action_lbl) lv_label_set_text(ent->btn_action_lbl, tr("Verwijderen...", "Removing..."));
        if (ent->btn_action) lv_obj_set_style_bg_color(ent->btn_action, lv_color_hex(COL_BUSY), 0);
        return;
    }
    if (ent->action_status == 3 || ent->action_status == 4) {
        if (ent->btn_action_lbl) lv_label_set_text(ent->btn_action_lbl, tr("Mislukt", "Failed"));
        if (ent->btn_action) lv_obj_set_style_bg_color(ent->btn_action, lv_color_hex(COL_WARN), 0);
        return;
    }

    int is_inst = (tile_slots_integration_by_id(ent->id) != NULL);
    ent->is_installed = is_inst;

    if (is_inst) {
        if (ent->badge_lbl) {
            lv_label_set_text_fmt(ent->badge_lbl, LV_SYMBOL_OK " %s", tr("Geinstalleerd", "Installed"));
            lv_obj_set_style_text_color(ent->badge_lbl, lv_color_hex(0x3ec263), 0);
        }
        if (ent->btn_action_lbl) {
            lv_label_set_text(ent->btn_action_lbl, tr("Verwijderen", "Uninstall"));
        }
        if (ent->btn_action) {
            lv_obj_set_style_bg_color(ent->btn_action, lv_color_hex(COL_WARN), 0);
        }
    } else {
        if (ent->badge_lbl) {
            lv_label_set_text(ent->badge_lbl, "");
        }
        if (ent->btn_action_lbl) {
            lv_label_set_text(ent->btn_action_lbl,
                              install_helper_ok() ? tr("Installeren", "Install")
                                                  : tr("Geen helper", "No helper"));
        }
        if (ent->btn_action) {
            lv_obj_set_style_bg_color(ent->btn_action, lv_color_hex(0x2a4060), 0);
        }
    }
}

/* ===================================================================== */
/* Screen build                                                          */
/* ===================================================================== */
static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

static void build_rows(void) {
    /* Wipe any previous rows */
    lv_obj_clean(list_box);

    for (int i = 0; i < entry_count; i++) {
        integration_t * ent = &entries[i];
        ent->row = lv_obj_create(list_box);
        lv_obj_set_size(ent->row, SX(940), SY(90));
        lv_obj_set_style_bg_color(ent->row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_color(ent->row, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_border_width(ent->row, settings.home_light_mode ? 1 : 0, 0);
        lv_obj_set_style_radius(ent->row, 12, 0);
        lv_obj_set_style_pad_all(ent->row, 10, 0);
        lv_obj_clear_flag(ent->row, LV_OBJ_FLAG_SCROLLABLE);

        /* Name + version on top row */
        lv_obj_t * lbl = lv_label_create(ent->row);
        lv_obj_set_style_text_font(lbl, SF(22), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_HI), 0);
        if (ent->version[0])
            lv_label_set_text_fmt(lbl, "%s  v%s", ent->name, ent->version);
        else
            lv_label_set_text(lbl, ent->name);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(4), 0);

        /* Description, scrolling if long */
        lv_obj_t * desc = lv_label_create(ent->row);
        lv_obj_set_style_text_font(desc, SF(18), 0);
        lv_obj_set_style_text_color(desc, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_width(desc, SX(560));
        lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
        lv_label_set_text(desc, ent->description);
        lv_obj_align(desc, LV_ALIGN_TOP_LEFT, SX(4), SY(30));

        /* Status badge (e.g. "✓ Geïnstalleerd") */
        ent->badge_lbl = lv_label_create(ent->row);
        lv_obj_set_style_text_font(ent->badge_lbl, SF(18), 0);
        lv_obj_align(ent->badge_lbl, LV_ALIGN_RIGHT_MID, SX(-180), 0);

        /* Action button (Install or Uninstall) */
        ent->btn_action = lv_btn_create(ent->row);
        lv_obj_set_size(ent->btn_action, SX(160), SY(52));
        lv_obj_align(ent->btn_action, LV_ALIGN_RIGHT_MID, SX(-8), 0);
        lv_obj_set_style_radius(ent->btn_action, 10, 0);
        lv_obj_add_event_cb(ent->btn_action, on_action_clicked,
                            LV_EVENT_CLICKED, ent);

        ent->btn_action_lbl = lv_label_create(ent->btn_action);
        lv_obj_set_style_text_color(ent->btn_action_lbl,
                                    lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(ent->btn_action_lbl,
                                    SF(19), 0);
        lv_obj_center(ent->btn_action_lbl);

        update_row_state(ent);
    }
}

/* Called by the periodic refresh timer (every 500 ms). */
static void refresh_cb(lv_timer_t * t) {
    (void)t;
    static int prev_count = -1;
    if (g_fetch_dirty) {
        if (status_lbl) lv_label_set_text(status_lbl, g_fetch_msg);
        g_fetch_dirty = 0;
    }
    if (entry_count != prev_count) {
        build_rows();
        prev_count = entry_count;
    }
    for (int i = 0; i < entry_count; i++) {
        update_row_state(&entries[i]);
    }
}

static int scr_light_mode = -1;

lv_obj_t * screen_marketplace_create(void) {
    if (scr_root && scr_light_mode == settings.home_light_mode) return scr_root;
    if (scr_root) { lv_obj_del(scr_root); scr_root = NULL; }
    scr_light_mode = settings.home_light_mode;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Header */
    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("Marktplaats", "Marketplace"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(180), SY(24));

    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, SX(140), SY(56));
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, SX(20), SY(14));
    lv_obj_set_style_bg_color(back, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_ext_click_area(back, 20);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(bl, SF(22), 0);
    lv_label_set_text(bl, tr("< Terug", "< Back"));
    lv_obj_center(bl);

    /* Status line shown above the list while we fetch + report errors. */
    status_lbl = lv_label_create(scr_root);
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(status_lbl, SF(18), 0);
    lv_label_set_text(status_lbl, tr("Catalogus ophalen...", "Fetching catalog..."));
    lv_obj_align(status_lbl, LV_ALIGN_TOP_LEFT, SX(22), SY(80));

    /* Scrollable container for the rows */
    list_box = lv_obj_create(scr_root);
    lv_obj_set_size(list_box, SX(980), SY(470));
    lv_obj_align(list_box, LV_ALIGN_TOP_LEFT, SX(22), SY(110));
    lv_obj_set_style_bg_opa(list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box, 0, 0);
    lv_obj_set_style_pad_all(list_box, 4, 0);
    lv_obj_set_style_pad_row(list_box, 10, 0);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);

    /* Kick off the catalog fetch on entry. */
    catalog_len = 0;
    entry_count = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, fetch_thread, NULL) == 0)
        pthread_detach(t);

    lv_timer_create(refresh_cb, 500, NULL);

    return scr_root;
}
