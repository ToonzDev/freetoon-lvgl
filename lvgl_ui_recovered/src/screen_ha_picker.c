/*
 * screen_ha_picker.c — HA entity picker. Called from the Settings → HA entities
 * modal's [🔍] browse buttons. Fetches entities from HA via /api/states filtered
 * by domain (cover, sensor, binary_sensor, camera, device_tracker, calendar),
 * shows them in a scrollable list, and on tap writes the chosen entity_id into
 * the calling textarea.
 */
#include "screens.h"
#include "display.h"        /* SX()/SY()/SF() */
#include "i18n.h"
#include "homeassistant.h"  /* ha_discover_entities, ha_discovered_t */
#include "settings.h"
#include "theme.h"
#include "tile_ext.h"       /* Integratie-tile mode */
#include "domoticz.h"       /* ... with a Domoticz device */
#include "homey_parse.h"    /* hy_fold_text */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t * scr_root;
static lv_obj_t * lbl_title;
static lv_obj_t * lst_results;
static lv_obj_t * spinner;

/* Set by screen_ha_picker_open*() before the screen is created.
 * g_add_type >= 0 => "add device" mode: the picked entity is appended to the
 * device list as that HADEV_* type. Otherwise the picked entity_id is written
 * into g_target_ta. */
static char          g_domain[32];
static lv_obj_t    * g_target_ta;
static int           g_add_type = -1;
/* Integratie-tile mode (g_tile_slot >= 0): the pick goes to tile_ext_bind;
 * g_tile_dz: list Domoticz devices instead of HA entities. */
static int           g_tile_slot = -1;
static int           g_tile_dz;
#define TILE_HA_DOMAINS "sensor,binary_sensor,switch,light,input_boolean"
#define TILE_LIST_MAX   500

/* Heap-stashed on each result button so on_pick can read back both fields. */
struct pick_item { char entity[64]; char fname[64]; };

static void on_back(lv_event_t * e) {
    (void)e;
    g_target_ta = NULL;
    g_add_type  = -1;
    g_tile_slot = -1;
    lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    ui_pop();
}

static void on_pick(lv_event_t * e) {
    lv_obj_t   * btn = lv_event_get_target(e);
    struct pick_item * it = (struct pick_item *)lv_obj_get_user_data(btn);
    if (it && it->entity[0]) {
        if (g_tile_slot >= 0)
            tile_ext_bind(g_tile_slot, g_tile_dz ? TEXT_DZ : TEXT_HA, it->entity, it->fname);
        else if (g_add_type >= 0)
            ha_device_add(g_add_type, it->entity, it->fname, 0);
        else if (g_target_ta)
            lv_textarea_set_text(g_target_ta, it->entity);
    }
    g_target_ta = NULL;
    g_add_type  = -1;
    g_tile_slot = -1;
    lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    ui_pop();
}

/* The pick_item blocks used to leak: on every open and on every refresh
 * (lv_obj_clean). Free each one with its button. */
static void on_item_delete(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    free(lv_obj_get_user_data(btn));
    lv_obj_set_user_data(btn, NULL);
}

/* One list row: "<name>\n<detail>"; the pick hands back entity + name. */
static void add_row(const char * name, const char * detail, const char * entity) {
    char label[200], nm[64];
    snprintf(nm, sizeof nm, "%s", name);
    hy_fold_text(nm);                         /* the UI fonts are ASCII */
    snprintf(label, sizeof label, "%s\n%s", nm[0] ? nm : entity, detail);
    lv_obj_t * btn = lv_list_add_btn(lst_results, NULL, label);
    lv_obj_set_style_bg_color(btn, lv_color_hex(THEME_ROW_BG), 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(THEME_TEXT_HI), 0);
    struct pick_item * it = malloc(sizeof *it);
    if (it) {
        snprintf(it->entity, sizeof it->entity, "%s", entity);
        snprintf(it->fname,  sizeof it->fname,  "%s", nm);
    }
    lv_obj_set_user_data(btn, it);
    lv_obj_add_event_cb(btn, on_pick, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn, on_item_delete, LV_EVENT_DELETE, NULL);
    lv_obj_t * lbl = lv_obj_get_child(btn, 0);
    if (lbl) {
        lv_obj_set_style_text_font(lbl, SF(18), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT_DIM), 0);
    }
}
static void list_message(const char * msg) {
    lv_obj_t * t = lv_list_add_text(lst_results, msg);
    if (t) lv_obj_set_style_text_color(t, lv_color_hex(THEME_TEXT_MUTED), 0);
}
/* HA entities sorted by domain, then name: sensors first, a findable list. */
static int cmp_ent(const void * a, const void * b) {
    const ha_discovered_t * x = a, * y = b;
    size_t dx = strcspn(x->entity_id, "."), dy = strcspn(y->entity_id, ".");
    int c = strncmp(x->entity_id, y->entity_id, dx < dy ? dx : dy);
    if (c || dx != dy) return c ? c : (int)dx - (int)dy;
    return strncasecmp(x->friendly_name[0] ? x->friendly_name : x->entity_id,
                       y->friendly_name[0] ? y->friendly_name : y->entity_id, 64);
}
/* Integratie-tile mode: HA entities of the tile domains, or Domoticz devices. */
static void load_tile_list(void) {
    if (g_tile_dz) {
        dz_item_t * d = malloc(sizeof *d * TILE_LIST_MAX);
        int n = d ? domoticz_list_all(d, TILE_LIST_MAX) : -1;
        lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        if (n < 0) list_message(tr("Geen antwoord van Domoticz - controleer adres en login",
                                   "No answer from Domoticz - check address and login"));
        else if (n == 0) list_message(tr("Domoticz heeft geen apparaten in gebruik", "Domoticz has no used devices"));
        for (int i = 0; i < n; i++) {
            char detail[120], idx[16];
            snprintf(detail, sizeof detail, "%s - %s (idx %d)", d[i].type, d[i].data, d[i].idx);
            hy_fold_text(detail);
            snprintf(idx, sizeof idx, "%d", d[i].idx);
            add_row(d[i].name, detail, idx);
        }
        free(d);
        return;
    }
    ha_discovered_t * ents = malloc(sizeof *ents * TILE_LIST_MAX);
    int count = 0;
    int ok = ents ? ha_discover_entities(TILE_HA_DOMAINS, ents, &count, TILE_LIST_MAX) : -1;
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    if (ok != 0) list_message(tr("Geen antwoord van Home Assistant - controleer host en token",
                                 "No answer from Home Assistant - check host and token"));
    else if (count == 0) list_message(tr("Home Assistant heeft geen sensoren of schakelaars",
                                         "Home Assistant has no sensors or switches"));
    if (ents && count > 1) qsort(ents, (size_t)count, sizeof *ents, cmp_ent);
    for (int i = 0; ents && i < count; i++)
        add_row(ents[i].friendly_name, ents[i].entity_id, ents[i].entity_id);
    free(ents);
}

/* Fetch entities from HA and populate the list. */
static void load_entities(void) {
    lv_obj_clean(lst_results);
    if (g_tile_slot >= 0) { load_tile_list(); return; }

    ha_discovered_t ents[HA_DISCOVERED_MAX];
    int count = 0;

    int ok = ha_discover_entities(g_domain, ents, &count, HA_DISCOVERED_MAX);

    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);

    if (ok != 0 || count == 0) {
        /* Two different problems: HA didn't answer (host / token), or it
         * answered and simply has no entity of this kind. */
        char msg[160];
        if (ok != 0)
            snprintf(msg, sizeof(msg), "%s",
                     tr("Geen antwoord van Home Assistant - controleer host en token",
                        "No answer from Home Assistant - check host and token"));
        else
            snprintf(msg, sizeof(msg),
                     tr("Home Assistant heeft geen entiteiten van het type %s",
                        "Home Assistant has no entities of type %s"),
                     g_domain);
        lv_obj_t * t = lv_list_add_text(lst_results, msg);
        if (t) lv_obj_set_style_text_color(t, lv_color_hex(THEME_TEXT_MUTED), 0);
        return;
    }

    for (int i = 0; i < count; i++) {
        char label[160];
        snprintf(label, sizeof(label), "%s\n%s",
                 ents[i].friendly_name, ents[i].entity_id);

        lv_obj_t * btn = lv_list_add_btn(lst_results, NULL, label);
        lv_obj_set_style_bg_color(btn, lv_color_hex(THEME_ROW_BG), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(THEME_TEXT_HI), 0);

        /* Stash entity_id + friendly_name so on_pick can read them back. */
        struct pick_item * it = malloc(sizeof *it);
        if (it) {
            snprintf(it->entity, sizeof it->entity, "%s", ents[i].entity_id);
            snprintf(it->fname,  sizeof it->fname,  "%s", ents[i].friendly_name);
        }
        lv_obj_set_user_data(btn, it);

        lv_obj_add_event_cb(btn, on_pick, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn, on_item_delete, LV_EVENT_DELETE, NULL);

        /* Style the entity_id line smaller and dimmer — it's line 1 of the
         * label. LV_LIST's default button label is a single lv_label, so the
         * newline gives us a natural two-line look. */
        lv_obj_t * lbl = lv_obj_get_child(btn, 0);
        if (lbl) {
            lv_obj_set_style_text_font(lbl, SF(18), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT_DIM), 0);
        }
    }
}

static void on_refresh(lv_event_t * e) {
    (void)e;
    lv_obj_clean(lst_results);
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    load_entities();
}

/* The stack deletes this screen when it leaves (ui_push_owned), including
 * via auto-home, which never runs on_back: restore the top layer this
 * screen hid, unless the dim screen is up (it keeps the top layer hidden
 * on purpose; waking restores it). */
static void on_scr_delete(lv_event_t * e) {
    if (!ui_is_dimmed()) lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    if (lv_event_get_target(e) != scr_root) return;
    scr_root = lbl_title = lst_results = spinner = NULL;
}

lv_obj_t * screen_ha_picker_create(void) {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_delete, LV_EVENT_DELETE, NULL);

    /* Close "X" button */
    int x_w = (DISP_VER < 600) ? 60 : 70;
    int x_h = (DISP_VER < 600) ? 46 : 50;
    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, x_w, x_h);
    lv_obj_set_style_bg_color(back, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_set_ext_click_area(back, 25);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, SX(8), SY(8));
    lv_obj_t * bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(bl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(bl, SF(22), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    /* Refresh button */
    lv_obj_t * refresh = lv_btn_create(scr_root);
    lv_obj_set_size(refresh, x_w, x_h);
    lv_obj_set_style_bg_color(refresh, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(refresh, 12, 0);
    lv_obj_set_ext_click_area(refresh, 25);
    lv_obj_align(refresh, LV_ALIGN_TOP_RIGHT, SX(-8), SY(8));
    lv_obj_t * rl = lv_label_create(refresh);
    lv_obj_set_style_text_color(rl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(rl, SF(22), 0);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_center(rl);
    lv_obj_add_event_cb(refresh, on_refresh, LV_EVENT_CLICKED, NULL);

    /* Title */
    lbl_title = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_title, SF(22), 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(THEME_TEXT_HI), 0);
    char title[64];
    if (g_tile_slot >= 0)
        snprintf(title, sizeof(title), "%s", g_tile_dz ? tr("Kies een Domoticz-apparaat", "Pick a Domoticz device")
                                                       : tr("Kies een Home Assistant-entiteit", "Pick a Home Assistant entity"));
    else
        snprintf(title, sizeof(title), tr("Kies %s-entiteit", "Pick %s entity"), g_domain);
    lv_label_set_text(lbl_title, title);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, SY(14));

    /* Results list */
    int list_y = (DISP_VER < 600) ? 60 : 70;
    int list_h = DISP_VER - list_y - ((DISP_VER < 600) ? 12 : 16);
    lst_results = lv_list_create(scr_root);
    lv_obj_set_size(lst_results, LV_PCT(94), list_h);
    lv_obj_align(lst_results, LV_ALIGN_TOP_MID, 0, list_y);
    lv_obj_set_style_bg_color(lst_results, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_color(lst_results, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(lst_results, settings.home_light_mode ? 1 : 0, 0);

    /* Spinner — shown while the curl call is in flight */
    spinner = lv_spinner_create(scr_root, 1000, 60);
    lv_obj_set_size(spinner, SY(40), SY(40));
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 0);

    /* Kick off the fetch */
    load_entities();

    return scr_root;
}

/* Called by the browse buttons in screen_settings.c. Stores the target
 * textarea + domain filter, then pushes the picker screen. */
void screen_ha_picker_open(const char * domain, lv_obj_t * target_ta) {
    snprintf(g_domain, sizeof(g_domain), "%s", domain ? domain : "");
    g_target_ta = target_ta;
    g_add_type  = -1;
    g_tile_slot = -1;
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    ui_push_owned(screen_ha_picker_create());
}

/* "Add device" mode — the picked entity is appended to the device list as
 * `dev_type` (HADEV_*) instead of being written into a textarea. The caller
 * (the device-manager screen) rebuilds its list on SCREEN_LOADED when this
 * screen pops, so the new device appears immediately. */
void screen_ha_picker_open_add(const char * domain, int dev_type) {
    snprintf(g_domain, sizeof(g_domain), "%s", domain ? domain : "");
    g_target_ta = NULL;
    g_add_type  = dev_type;
    g_tile_slot = -1;
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    ui_push_owned(screen_ha_picker_create());
}

/* Integratie-tile mode: the picked HA entity (dz=0) or Domoticz device (dz=1)
 * goes onto tile `slot` (tile_ext_bind). */
void screen_ext_picker_open_tile(int slot, int dz) {
    g_domain[0] = 0;
    g_target_ta = NULL;
    g_add_type  = -1;
    g_tile_slot = slot;
    g_tile_dz   = dz;
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    ui_push_owned(screen_ha_picker_create());
}
