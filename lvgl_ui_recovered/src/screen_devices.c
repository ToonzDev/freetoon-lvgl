/*
 * Devices hub ("Apparaten", BACKLOG #53 stage 1) — one screen listing the
 * switchable devices of every enabled backend: Hue lamps, Home Assistant
 * devices, Domoticz switches/dimmers/blinds, Z-Wave plugs, HomeWizard Energy
 * Sockets and the efanlamp. Grouped by room: Hue rooms and HA areas are
 * merged by name; devices without a room are grouped under their source.
 * Each row has a name, a state line with a small source badge, and the
 * control (on/off, open/stop/close, run). The per-backend screens stay
 * reachable through the chips at the top (dimming, groups, scenes, pairing).
 *
 * Opened from home by swipe-right, the devices button next to the envelope,
 * and the Verlichting / Slimme stekker tiles. Built fresh per open
 * (ui_push_owned) so it costs no memory while closed; rows are capped at
 * HUB_MAX_ITEMS for the Toon 1.
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "ui_lazy.h"
#include "hue.h"
#include "homeassistant.h"
#include "domoticz.h"
#include "homewizard.h"
#include "efanlamp.h"
#include "screen_zwave.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>

#define COL_BG        THEME_BG
#define COL_CARD      THEME_CARD_BG
#define COL_TEXT_HI   THEME_TEXT_HI
#define COL_TEXT_DIM  THEME_TEXT_DIM
#define COL_ON        THEME_ACCENT
#define COL_OFF       THEME_OFF
#define COL_OFFLINE   THEME_OFFLINE

enum { SRC_HUE, SRC_HA, SRC_DZ, SRC_ZW, SRC_HWS, SRC_EFAN, SRC_COUNT };
enum { CTL_TOGGLE, CTL_COVER, CTL_RUN };

#define HUB_MAX_ITEMS 96

typedef struct {
    uint8_t    src, ctl;
    int        idx;          /* HA: device idx, DZ: device idx, HWS: socket slot, EFAN: 0 light / 1 fan */
    char       key[64];      /* stable id: Hue light id / HA entity / Z-Wave uuid */
    char       name[48];
    char       room[48];     /* "" = no room (grouped under its source) */
    lv_obj_t * lbl_state;
    lv_obj_t * btn;
    lv_obj_t * btn_lbl;
} hub_item_t;

static lv_obj_t   * scr_root   = NULL;
static lv_obj_t   * list       = NULL;
static lv_obj_t   * empty_hint = NULL;
static lv_timer_t * refresh_timer = NULL;
static hub_item_t   items[HUB_MAX_ITEMS];
static int          item_count = 0;
static unsigned long built_sig = 0;
static int          zw_ticks = 0;

static const char * src_badge(int s) {
    switch (s) {
    case SRC_HUE:  return "Hue";
    case SRC_HA:   return "HA";
    case SRC_DZ:   return "Domoticz";
    case SRC_ZW:   return "Z-Wave";
    case SRC_HWS:  return "HomeWizard";
    default:       return tr("Ventilator", "Fan");
    }
}
static const char * src_group(int s) {
    switch (s) {
    case SRC_HUE:  return "Philips Hue";
    case SRC_HA:   return "Home Assistant";
    case SRC_DZ:   return "Domoticz";
    case SRC_ZW:   return tr("Z-Wave stekkers", "Z-Wave plugs");
    case SRC_HWS:  return tr("HomeWizard stekkers", "HomeWizard sockets");
    default:       return tr("Ventilatorlamp", "Fan lamp");
    }
}

/* ---- which backends are live (same conditions the old routing used) ---- */
static int hue_on(void) { return settings.hue_enabled || hue_state.count > 0; }
static int ha_on(void)  { return settings.enable_ha && ha_device_count > 0; }
static int dz_on(void)  { return settings.enable_domoticz || (settings.client_mode && domoticz_state.count > 0); }
static int efan_on(void){ return settings.efanlamp_host[0] != 0; }

/* Fingerprint of everything that changes the row set or the grouping. When
 * it moves (a backend finished loading, HA areas arrived, a device was added
 * on a sub-screen) the list is rebuilt. */
static unsigned long hub_sig(void) {
    unsigned long h = 5381u;
#define MIX(v) (h = h * 33u + (unsigned long)(v))
    MIX(hue_on() ? hue_state.count : -1);
    MIX(hue_on() ? hue_state.group_count : -1);
    MIX(settings.enable_ha ? ha_device_count : -1);
    MIX(ha_device_area_gen);
    MIX(dz_on() ? domoticz_state.count : -1);
    MIX(zwave_switch_count_cached());
    for (int i = 0; i < HW_SOCKET_MAX; i++) MIX(homewizard_socket_present(i));
    MIX(efan_on());
#undef MIX
    return h;
}

/* ---- resolving an item back to its backend entry ---- */
static hue_light_t * find_hue(const hub_item_t * it) {
    for (int i = 0; i < hue_state.count; i++)
        if (strcmp(hue_state.lights[i].id, it->key) == 0) return &hue_state.lights[i];
    return NULL;
}
static ha_device_t * find_ha(const hub_item_t * it) {
    if (it->idx >= 0 && it->idx < ha_device_count &&
        strcmp(ha_devices[it->idx].entity_id, it->key) == 0) return &ha_devices[it->idx];
    return NULL;
}
static domoticz_dev_t * find_dz(const hub_item_t * it) {
    int dz_idx = atoi(it->key);
    for (int i = 0; i < domoticz_state.count; i++)
        if (domoticz_state.dev[i].idx == dz_idx) return &domoticz_state.dev[i];
    return NULL;
}
/* Z-Wave: returns the switch ordinal (what zwave_toggle_switch takes), -1 if gone. */
static int find_zw(const hub_item_t * it, zwave_dev_t * out) {
    for (int i = 0; zwave_get_switch(i, out); i++)
        if (strcmp(out->uuid, it->key) == 0) return i;
    return -1;
}

/* ---- actions ---- */
static hub_item_t * ev_item(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    return (i >= 0 && i < item_count) ? &items[i] : NULL;
}

static void on_toggle(lv_event_t * e) {
    hub_item_t * it = ev_item(e);
    if (!it) return;
    switch (it->src) {
    case SRC_HUE: hue_light_toggle_async(it->key); break;
    case SRC_HA: {
        ha_device_t * D = find_ha(it);
        if (!D) break;
        if (it->ctl == CTL_RUN) ha_device_run_async(D->type, D->entity_id);
        else                    ha_device_toggle_async(D->type, D->entity_id);
        break;
    }
    case SRC_DZ: {
        domoticz_dev_t * d = find_dz(it);
        if (d) domoticz_switch_async(d->idx, "Toggle");
        break;
    }
    case SRC_ZW: {
        zwave_dev_t zd;
        int n = find_zw(it, &zd);
        if (n >= 0) zwave_toggle_switch(n);
        break;
    }
    case SRC_HWS:
        if (homewizard_socket_present(it->idx))
            homewizard_socket_set_async(it->idx, !hw_sockets[it->idx].on);
        break;
    case SRC_EFAN:
        if (it->idx == 0)
            efanlamp_light_set(!efanlamp.light_on,
                               efanlamp.light_brightness > 0 ? efanlamp.light_brightness : 100);
        else
            efanlamp_fan_toggle();
        break;
    }
}

/* Cover buttons: user_data = item index, the command sits in the button's
 * own user field via a tiny lookup on the label text would be fragile — so
 * three separate callbacks. */
static void cover_cmd(lv_event_t * e, const char * ha_cmd, const char * dz_cmd) {
    hub_item_t * it = ev_item(e);
    if (!it) return;
    if (it->src == SRC_HA) {
        ha_device_t * D = find_ha(it);
        if (D) ha_device_cover_async(D->entity_id, ha_cmd);
    } else if (it->src == SRC_DZ) {
        domoticz_dev_t * d = find_dz(it);
        if (d) domoticz_switch_async(d->idx, dz_cmd);
    }
}
static void on_cover_open (lv_event_t * e) { cover_cmd(e, "open",  "Open");  }
static void on_cover_stop (lv_event_t * e) { cover_cmd(e, "stop",  "Stop");  }
static void on_cover_close(lv_event_t * e) { cover_cmd(e, "close", "Close"); }

/* ---- navigation ---- */
static void back_async(void * unused) { (void)unused; ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; lv_async_call(back_async, NULL); }

static void on_chip(lv_event_t * e) {
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case SRC_HUE:  ui_push_owned(screen_hue_create());      break;
    case SRC_HA:   ui_push(screen_lights_create());         break;
    case SRC_DZ:   ui_push(screen_domoticz_create());       break;
    case SRC_ZW:   ui_push(screen_zwave_create());          break;
    case SRC_EFAN: ui_push_owned(screen_efanlamp_create()); break;
    default: break;
    }
}

/* ---- collecting the items ---- */
static hub_item_t * add_item(int src, int ctl, int idx, const char * key,
                             const char * name, const char * room) {
    if (item_count >= HUB_MAX_ITEMS) return NULL;
    hub_item_t * it = &items[item_count++];
    memset(it, 0, sizeof *it);
    it->src = (uint8_t)src;
    it->ctl = (uint8_t)ctl;
    it->idx = idx;
    snprintf(it->key,  sizeof it->key,  "%s", key  ? key  : "");
    snprintf(it->name, sizeof it->name, "%s", name ? name : "");
    snprintf(it->room, sizeof it->room, "%s", room ? room : "");
    return it;
}

static void collect_items(void) {
    item_count = 0;
    char buf[64];
    if (hue_on())
        for (int i = 0; i < hue_state.count; i++) {
            const hue_light_t * L = &hue_state.lights[i];
            add_item(SRC_HUE, CTL_TOGGLE, i, L->id, L->name, hue_light_room(L->id));
        }
    if (ha_on())
        for (int i = 0; i < ha_device_count; i++) {
            const ha_device_t * D = &ha_devices[i];
            int ctl = D->type == HADEV_COVER ? CTL_COVER
                    : (D->type == HADEV_SCRIPT || D->type == HADEV_SCENE) ? CTL_RUN : CTL_TOGGLE;
            ha_device_get_area(i, buf, sizeof buf);
            add_item(SRC_HA, ctl, i, D->entity_id, D->name, buf);
        }
    if (dz_on())
        for (int i = 0; i < domoticz_state.count; i++) {
            const domoticz_dev_t * d = &domoticz_state.dev[i];
            snprintf(buf, sizeof buf, "%d", d->idx);
            add_item(SRC_DZ, d->kind == DZ_BLIND ? CTL_COVER : CTL_TOGGLE, i, buf, d->name, "");
        }
    {
        zwave_dev_t zd;
        for (int i = 0; zwave_get_switch(i, &zd); i++) {
            char nm[64];
            if (!zwave_get_custom_name(zd.uuid, zd.node_id, nm, sizeof nm) || !nm[0])
                snprintf(nm, sizeof nm, "%s", zd.name[0] ? zd.name : "Z-Wave");
            add_item(SRC_ZW, CTL_TOGGLE, i, zd.uuid, nm, "");
        }
    }
    for (int i = 0; i < HW_SOCKET_MAX; i++)
        if (homewizard_socket_present(i))
            add_item(SRC_HWS, CTL_TOGGLE, i, "", homewizard_socket_name(i), "");
    if (efan_on()) {
        add_item(SRC_EFAN, CTL_TOGGLE, 0, "", tr("Lamp", "Light"), "");
        add_item(SRC_EFAN, CTL_TOGGLE, 1, "", tr("Ventilator", "Fan"), "");
    }
}

/* ---- widgets ---- */
static lv_obj_t * mk_btn(lv_obj_t * parent, const char * txt, int w, int h,
                         lv_event_cb_t cb, int item) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_OFF), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)item);
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(l, SF(14), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

static void add_header(const char * txt) {
    lv_obj_t * h = lv_label_create(list);
    lv_obj_set_width(h, lv_pct(100));           /* full width: starts a new flex line */
    lv_obj_set_style_text_color(h, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(h, SF(16), 0);
    lv_obj_set_style_pad_top(h, 6, 0);
    lv_label_set_text(h, txt);
}

static void build_row(int i) {
    hub_item_t * it = &items[i];
    lv_obj_t * c = lv_obj_create(list);
    lv_obj_set_size(c, lv_pct(49), SY(64));
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    int ctl_w = it->ctl == CTL_COVER ? 3 * 52 + 8 : (it->ctl == CTL_RUN ? 84 : 64);

    lv_obj_t * nm = lv_label_create(c);
    lv_obj_set_width(nm, lv_pct(100));
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_right(nm, ctl_w + 4, 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(nm, SF(18), 0);
    lv_label_set_text(nm, it->name);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, -2);

    it->lbl_state = lv_label_create(c);
    lv_obj_set_style_text_color(it->lbl_state, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(it->lbl_state, SF(14), 0);
    lv_label_set_text(it->lbl_state, src_badge(it->src));
    lv_obj_align(it->lbl_state, LV_ALIGN_BOTTOM_LEFT, 0, 2);

    if (it->ctl == CTL_COVER) {
        lv_obj_t * b;
        b = mk_btn(c, tr("Dicht", "Close"), 52, 36, on_cover_close, i);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
        b = mk_btn(c, "Stop", 52, 36, on_cover_stop, i);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -56, 0);
        b = mk_btn(c, "Open", 52, 36, on_cover_open, i);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -112, 0);
    } else if (it->ctl == CTL_RUN) {
        int scene = it->src == SRC_HA && find_ha(it) && find_ha(it)->type == HADEV_SCENE;
        it->btn = mk_btn(c, scene ? tr("Activeer", "Activate") : tr("Start", "Run"), 84, 38, on_toggle, i);
        lv_obj_set_style_bg_color(it->btn, lv_color_hex(THEME_ACCENT_FILL), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(it->btn, 0), lv_color_hex(THEME_ON_ACCENT), 0);
        lv_obj_align(it->btn, LV_ALIGN_RIGHT_MID, 0, 0);
        it->btn = NULL;                         /* stateless: refresh leaves it alone */
    } else {
        it->btn = mk_btn(c, "--", 64, 38, on_toggle, i);
        it->btn_lbl = lv_obj_get_child(it->btn, 0);
        lv_obj_align(it->btn, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static int cmp_room(const void * a, const void * b) {
    return strcasecmp(*(const char * const *)a, *(const char * const *)b);
}

static void build_chips(void) {
    static const int order[] = { SRC_HUE, SRC_HA, SRC_DZ, SRC_ZW, SRC_EFAN };
    int live[] = { hue_on(), settings.enable_ha, dz_on(), zwave_switch_count_cached() > 0, efan_on() };
    int any = 0;
    for (int k = 0; k < 5; k++) any |= live[k];
    if (!any) return;
    lv_obj_t * row = lv_obj_create(list);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_pad_row(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (int k = 0; k < 5; k++) {
        if (!live[k]) continue;
        lv_obj_t * b = lv_btn_create(row);
        lv_obj_set_height(b, SY(40));
        lv_obj_set_width(b, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(b, 14, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_ACCENT), 0);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, on_chip, LV_EVENT_CLICKED, (void *)(intptr_t)order[k]);
        lv_obj_t * l = lv_label_create(b);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT_HI), 0);
        lv_obj_set_style_text_font(l, SF(16), 0);
        lv_label_set_text_fmt(l, LV_SYMBOL_SETTINGS "  %s", src_group(order[k]));
        lv_obj_center(l);
    }
}

static void refresh_rows(void);

static void rebuild(void) {
    if (!list) return;
    lv_obj_clean(list);
    built_sig = hub_sig();
    collect_items();
    build_chips();

    if (empty_hint) {
        if (item_count) lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_clear_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
    }

    /* Rooms: distinct names (case-insensitive), alphabetical. */
    const char * rooms[HUB_MAX_ITEMS];
    int nrooms = 0;
    for (int i = 0; i < item_count; i++) {
        if (!items[i].room[0]) continue;
        int dup = 0;
        for (int r = 0; r < nrooms && !dup; r++) dup = !strcasecmp(rooms[r], items[i].room);
        if (!dup) rooms[nrooms++] = items[i].room;
    }
    qsort(rooms, (size_t)nrooms, sizeof rooms[0], cmp_room);
    for (int r = 0; r < nrooms; r++) {
        add_header(rooms[r]);
        for (int i = 0; i < item_count; i++)
            if (items[i].room[0] && !strcasecmp(items[i].room, rooms[r])) build_row(i);
    }
    /* No room: per source. */
    for (int s = 0; s < SRC_COUNT; s++) {
        int any = 0;
        for (int i = 0; i < item_count; i++) {
            if (items[i].room[0] || items[i].src != s) continue;
            if (!any) {
                add_header(nrooms ? (s == SRC_HUE || s == SRC_HA
                                     ? (s == SRC_HUE ? tr("Hue - zonder ruimte", "Hue - no room")
                                                     : tr("Home Assistant - zonder ruimte", "Home Assistant - no area"))
                                     : src_group(s))
                                  : src_group(s));
                any = 1;
            }
            build_row(i);
        }
    }
    refresh_rows();
}

/* ---- live state ---- */
typedef struct { char txt[48]; int state; } row_state_t;   /* state: 1 on, 0 off, -1 offline/unknown */

static void item_state(const hub_item_t * it, row_state_t * o) {
    const char * badge = src_badge(it->src);
    o->state = -1;
    snprintf(o->txt, sizeof o->txt, "%s - --", badge);
    switch (it->src) {
    case SRC_HUE: {
        const hue_light_t * L = find_hue(it);
        if (!L) break;
        if (!L->reachable) { snprintf(o->txt, sizeof o->txt, "%s - %s", badge, tr("onbereikbaar", "unreachable")); break; }
        o->state = L->on ? 1 : 0;
        if (L->on && L->is_dimmable) snprintf(o->txt, sizeof o->txt, "%s - %s %d%%", badge, tr("aan", "on"), L->bri_pct);
        else snprintf(o->txt, sizeof o->txt, "%s - %s", badge, L->on ? tr("aan", "on") : tr("uit", "off"));
        break;
    }
    case SRC_HA: {
        const ha_device_t * D = find_ha(it);
        if (!D) break;
        if (it->ctl == CTL_RUN) {
            snprintf(o->txt, sizeof o->txt, "%s - %s", badge,
                     D->type == HADEV_SCENE ? tr("scene", "scene") : "script");
            o->state = 0;
            break;
        }
        if (!D->available) { snprintf(o->txt, sizeof o->txt, "%s - offline", badge); break; }
        if (it->ctl == CTL_COVER) {
            char st[16];
            ha_device_get_state(it->idx, st, sizeof st);
            if (D->position >= 0) snprintf(o->txt, sizeof o->txt, "%s - %s %d%%", badge, st[0] ? st : "", D->position);
            else snprintf(o->txt, sizeof o->txt, "%s - %s", badge, st[0] ? st : "--");
            o->state = 0;
            break;
        }
        o->state = D->on ? 1 : 0;
        if (D->on && D->type == HADEV_LIGHT && D->brightness > 0)
            snprintf(o->txt, sizeof o->txt, "%s - %s %d%%", badge, tr("aan", "on"), D->brightness * 100 / 255);
        else snprintf(o->txt, sizeof o->txt, "%s - %s", badge, D->on ? tr("aan", "on") : tr("uit", "off"));
        break;
    }
    case SRC_DZ: {
        const domoticz_dev_t * d = find_dz(it);
        if (!d) break;
        if (d->kind == DZ_BLIND) {
            o->state = 0;
            if (d->level >= 0) snprintf(o->txt, sizeof o->txt, "%s - %s %d%%", badge, d->on ? "open" : tr("dicht", "closed"), d->level);
            else snprintf(o->txt, sizeof o->txt, "%s - %s", badge, d->on ? "open" : tr("dicht", "closed"));
            break;
        }
        o->state = d->on ? 1 : 0;
        if (d->on && d->kind == DZ_DIMMER && d->level > 0)
            snprintf(o->txt, sizeof o->txt, "%s - %s %d%%", badge, tr("aan", "on"), d->level);
        else snprintf(o->txt, sizeof o->txt, "%s - %s", badge, d->on ? tr("aan", "on") : tr("uit", "off"));
        break;
    }
    case SRC_ZW: {
        zwave_dev_t zd;
        if (find_zw(it, &zd) < 0 || zd.state < 0) break;
        o->state = zd.state ? 1 : 0;
        if (zd.state && zd.power_w > 0.5f) snprintf(o->txt, sizeof o->txt, "%s - %s %.0f W", badge, tr("aan", "on"), zd.power_w);
        else snprintf(o->txt, sizeof o->txt, "%s - %s", badge, zd.state ? tr("aan", "on") : tr("uit", "off"));
        break;
    }
    case SRC_HWS: {
        const hw_socket_t * S = &hw_sockets[it->idx];
        if (!settings.client_mode && !S->connected) {
            snprintf(o->txt, sizeof o->txt, "%s - %s", badge, S->polled ? "offline" : "--");
            break;
        }
        o->state = S->on ? 1 : 0;
        if (S->on) snprintf(o->txt, sizeof o->txt, "%s - %s %.0f W", badge, tr("aan", "on"), S->power_w);
        else snprintf(o->txt, sizeof o->txt, "%s - %s", badge, tr("uit", "off"));
        break;
    }
    case SRC_EFAN:
        if (!efanlamp.connected) { snprintf(o->txt, sizeof o->txt, "%s - offline", badge); break; }
        if (it->idx == 0) {
            o->state = efanlamp.light_on ? 1 : 0;
            if (efanlamp.light_on) snprintf(o->txt, sizeof o->txt, "%s - %s %d%%", badge, tr("aan", "on"), efanlamp.light_brightness);
            else snprintf(o->txt, sizeof o->txt, "%s - %s", badge, tr("uit", "off"));
        } else {
            o->state = efanlamp.fan_on ? 1 : 0;
            if (efanlamp.fan_on) snprintf(o->txt, sizeof o->txt, "%s - %s %d", badge, tr("stand", "speed"), efanlamp.fan_speed);
            else snprintf(o->txt, sizeof o->txt, "%s - %s", badge, tr("uit", "off"));
        }
        break;
    }
}

static void refresh_rows(void) {
    for (int i = 0; i < item_count; i++) {
        hub_item_t * it = &items[i];
        if (!it->lbl_state) continue;
        row_state_t st;
        item_state(it, &st);
        ui_label_set_text(it->lbl_state, st.txt);
        ui_obj_set_style_text_color(it->lbl_state,
            lv_color_hex(st.state == 1 ? COL_ON : st.state == 0 ? COL_TEXT_DIM : COL_OFFLINE), 0);
        if (!it->btn) continue;
        const char * bl = st.state == 1 ? tr("Aan", "On") : st.state == 0 ? tr("Uit", "Off") : "--";
        ui_label_set_text(it->btn_lbl, bl);
        ui_obj_set_style_bg_color(it->btn, lv_color_hex(st.state == 1 ? COL_ON : COL_OFF), 0);
        ui_obj_set_style_text_color(it->btn_lbl,
            lv_color_hex(st.state == 1 ? THEME_ON_ACCENT : COL_TEXT_HI), 0);
    }
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (!scr_root || lv_scr_act() != scr_root) return;
    if (++zw_ticks >= 4) { zw_ticks = 0; zwave_poll(); }   /* Z-Wave state is pulled, not pushed */
    if (hub_sig() != built_sig) rebuild();
    else refresh_rows();
}

static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        /* Back from a backend screen: devices may have been added/renamed. */
        rebuild();
        if (refresh_timer) lv_timer_resume(refresh_timer);
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) lv_timer_pause(refresh_timer);
    } else if (c == LV_EVENT_DELETE) {
        if (refresh_timer) { lv_timer_del(refresh_timer); refresh_timer = NULL; }
        scr_root = list = empty_hint = NULL;
        item_count = 0;
    }
}

lv_obj_t * screen_devices_create(void) {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_LOADED,   NULL);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_DELETE,          NULL);
    item_count = 0;

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("Apparaten", "Devices"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 16);

    int x_w = (DISP_VER < 600) ? 60 : 70;
    int x_h = (DISP_VER < 600) ? 46 : 50;
    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, x_w, x_h);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, SX(-12), SY(12));
    lv_obj_set_style_bg_color(back, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_set_ext_click_area(back, 25);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(bl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(bl, SF(22), 0);
    lv_obj_center(bl);

    list = lv_obj_create(scr_root);
    lv_obj_set_size(list, DISP_HOR - 24, DISP_VER - SY(78));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, SY(70));
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_style_pad_column(list, 8, 0);

    empty_hint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(empty_hint, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(empty_hint, SF(22), 0);
    lv_obj_set_width(empty_hint, DISP_HOR - 120);
    lv_label_set_long_mode(empty_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(empty_hint,
        tr("Nog geen apparaten.\nKoppel Philips Hue, Home Assistant, Domoticz,\nZ-Wave stekkers of HomeWizard stekkers via Instellingen.",
           "No devices yet.\nConnect Philips Hue, Home Assistant, Domoticz,\nZ-Wave plugs or HomeWizard sockets in Settings."));
    lv_obj_align(empty_hint, LV_ALIGN_CENTER, 0, SY(30));
    lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    rebuild();
    return scr_root;
}

/* Home shows its devices button only when there is something to control.
 * Cheap (no polling): called from the home refresh tick. */
int devices_hub_available(void) {
    if (hue_on() || settings.enable_ha || dz_on() || efan_on()) return 1;
    if (zwave_switch_count_cached() > 0) return 1;
    return homewizard_socket_count() > 0;
}
