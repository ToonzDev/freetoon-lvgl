/* The places on the Verlichting home tile: see tile_lights.h. */
#include "tile_lights.h"
#include "settings.h"
#include "domoticz.h"
#include "homeassistant.h"
#include "homey.h"
#include "hue.h"
#include "screen_zwave.h"
#include "homewizard.h"
#include "i18n.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char * slot_str(int slot) {
    static char * const s[TL_SLOTS] = {
        settings.tile_light1, settings.tile_light2, settings.tile_light3, settings.tile_light4,
        settings.tile_light5, settings.tile_light6, settings.tile_light7, settings.tile_light8,
    };
    return s[slot < 0 ? 0 : slot >= TL_SLOTS ? TL_SLOTS - 1 : slot];
}
#define SLOT_SZ (sizeof settings.tile_light1)

static int g_capacity = 2;
void tl_set_capacity(int n) { g_capacity = n < 1 ? 1 : n > TL_SLOTS ? TL_SLOTS : n; }
int  tl_capacity(void)      { return g_capacity; }

/* n-th Z-Wave switch from the list already loaded. zwave_get_switch() would
 * fetch the list over HTTP when it is empty; the home screen's own poll
 * loads it, and this is called per row and per second. */
static int zw_get(int i, zwave_dev_t * d) {
    return zwave_switch_count_cached() > 0 && zwave_get_switch(i, d);
}

static int dz_active(void) {
    return settings.enable_domoticz || (settings.client_mode && domoticz_state.count > 0);
}

static int ha_index(const char * eid) {
    for (int i = 0; i < ha_device_count && i < HA_DEVICE_MAX; i++)
        if (!strcmp(ha_devices[i].entity_id, eid)) return i;
    return -1;
}

static int zw_index(const char * uuid) {
    zwave_dev_t d;
    for (int i = 0; zw_get(i, &d); i++)
        if (!strcmp(d.uuid, uuid)) return i;
    return -1;
}

static int hue_light_index(const char * id) {
    for (int i = 0; i < hue_state.count && i < HUE_MAX_LIGHTS; i++)
        if (!strcmp(hue_state.lights[i].id, id)) return i;
    return -1;
}

/* HomeWizard Energy Socket slot with this host, -1 if none. */
static int hws_index(const char * host) {
    for (int i = 0; i < HW_SOCKET_MAX; i++)
        if (settings.hw_socket_host[i][0] && !strcmp(settings.hw_socket_host[i], host)) return i;
    return -1;
}

static int hue_group_index(const char * id) {
    for (int i = 0; i < hue_state.group_count && i < HUE_MAX_GROUPS; i++)
        if (!strcmp(hue_state.groups[i].id, id)) return i;
    return -1;
}

/* The rule from before the chooser: Domoticz T1/T2, then HA lights/switches
 * flagged Home, then Z-Wave switches. The n-th candidate fills slot n. */
static tl_src_t auto_src(int slot) {
    tl_src_t c[2];
    int k = 0;
    memset(c, 0, sizeof c);
    if (dz_active()) {
        int ids[2] = { settings.domoticz_tile_idx1, settings.domoticz_tile_idx2 };
        for (int j = 0; j < 2 && k < 2; j++)
            if (ids[j] >= 0) { c[k].kind = TL_DZ; c[k].dz_idx = ids[j]; k++; }
    }
    if (settings.enable_ha) {
        for (int i = 0; i < ha_device_count && i < HA_DEVICE_MAX && k < 2; i++) {
            const ha_device_t * d = &ha_devices[i];
            if (!d->pin_home || (d->type != HADEV_LIGHT && d->type != HADEV_SWITCH)) continue;
            c[k].kind = TL_HA; c[k].ha_i = i;
            snprintf(c[k].id, sizeof c[k].id, "%s", d->entity_id);
            k++;
        }
    }
    zwave_dev_t zd;
    for (int i = 0; k < 2 && zw_get(i, &zd); i++) {
        c[k].kind = TL_ZW; c[k].zw_i = i;
        snprintf(c[k].id, sizeof c[k].id, "%s", zd.uuid);
        k++;
    }
    tl_src_t none = { TL_NONE, -1, -1, -1, -1, "" };
    return slot < k ? c[slot] : none;
}

static tl_src_t parse_ref(const char * r) {
    tl_src_t s = { TL_NONE, -1, -1, -1, -1, "" };
    if (!r || strlen(r) < 4 || r[2] != ':') return s;
    const char * v = r + 3;
    if (!strncmp(r, "dz", 2)) {
        char * end;
        long idx = strtol(v, &end, 10);
        if (end != v && !*end && idx >= 0) { s.kind = TL_DZ; s.dz_idx = (int)idx; }
        return s;
    }
    snprintf(s.id, sizeof s.id, "%s", v);
    if (!strncmp(r, "ha", 2))      { s.kind = TL_HA; s.ha_i = ha_index(v); }
    else if (!strncmp(r, "hy", 2)) { s.kind = TL_HY; }
    else if (!strncmp(r, "zw", 2)) { s.kind = TL_ZW; s.zw_i = zw_index(v); }
    else if (!strncmp(r, "hu", 2)) { s.kind = TL_HUE; s.hue_i = hue_light_index(v); }
    else if (!strncmp(r, "hg", 2)) { s.kind = TL_HUEG; s.hue_i = hue_group_index(v); }
    else if (!strncmp(r, "hs", 2)) { s.kind = TL_HWS; s.hue_i = hws_index(v); }
    else s.id[0] = 0;
    return s;
}

tl_src_t tl_resolve(int slot) {
    if (slot < 0 || slot >= TL_SLOTS) return parse_ref("");
    const char * r = slot_str(slot);
    if (slot < 2 && !strcmp(r, "auto")) return auto_src(slot);
    return parse_ref(r);
}

void tl_ref(const tl_src_t * s, char * out, size_t n) {
    switch (s->kind) {
    case TL_DZ:   snprintf(out, n, "dz:%d", s->dz_idx); break;
    case TL_HA:   snprintf(out, n, "ha:%s", s->id); break;
    case TL_HY:   snprintf(out, n, "hy:%s", s->id); break;
    case TL_ZW:   snprintf(out, n, "zw:%s", s->id); break;
    case TL_HUE:  snprintf(out, n, "hu:%s", s->id); break;
    case TL_HUEG: snprintf(out, n, "hg:%s", s->id); break;
    case TL_HWS:  snprintf(out, n, "hs:%s", s->id); break;
    default:      if (n) out[0] = 0;
    }
}

int tl_slot_of(const char * ref) {
    if (!ref || !ref[0]) return -1;
    for (int k = 0; k < TL_SLOTS; k++) {
        tl_src_t s = tl_resolve(k);
        char r[TL_REF_LEN];
        tl_ref(&s, r, sizeof r);
        if (!strcmp(r, ref)) return k;
    }
    return -1;
}

/* Replace "auto" by what slots 1 and 2 show now, so changing one slot does
 * not change what the other shows. */
static void freeze(void) {
    tl_src_t s[2] = { tl_resolve(0), tl_resolve(1) };
    for (int k = 0; k < 2; k++) {
        char * d = slot_str(k);
        if (strcmp(d, "auto")) continue;
        char r[TL_REF_LEN];
        tl_ref(&s[k], r, sizeof r);
        snprintf(d, SLOT_SZ, "%s", r);
    }
}

void tl_set(int slot, const char * ref) {
    if (slot < 0 || slot >= TL_SLOTS) return;
    if (!ref) ref = "";
    freeze();
    if (ref[0])
        for (int k = 0; k < TL_SLOTS; k++)
            if (k != slot && !strcmp(slot_str(k), ref)) slot_str(k)[0] = 0;
    snprintf(slot_str(slot), SLOT_SZ, "%s", ref);
    settings_save();
}

int tl_pin(const char * ref, int on, int drop_oldest) {
    if (!ref || !ref[0]) return -1;
    freeze();
    int cur = tl_slot_of(ref);
    if (!on) {
        if (cur >= 0) { slot_str(cur)[0] = 0; settings_save(); }
        return -1;
    }
    if (cur >= 0) { settings_save(); return cur; }
    int cap = tl_capacity();
    for (int k = 0; k < cap; k++)
        if (!slot_str(k)[0]) { tl_set(k, ref); return k; }
    if (!drop_oldest) { settings_save(); return -1; }
    for (int k = 0; k + 1 < cap; k++)
        snprintf(slot_str(k), SLOT_SZ, "%s", slot_str(k + 1));
    snprintf(slot_str(cap - 1), SLOT_SZ, "%s", ref);
    settings_save();
    return cap - 1;
}

static const domoticz_dev_t * dz_dev(int idx) {
    for (int i = 0; i < domoticz_state.count && i < DOMOTICZ_MAX_DEV; i++)
        if (domoticz_state.dev[i].idx == idx) return &domoticz_state.dev[i];
    return NULL;
}

int tl_status(const tl_src_t * s, char * name, size_t n, int * on) {
    int known = 0, o = 0;
    if (n) name[0] = 0;
    if (s->kind == TL_DZ) {
        const domoticz_dev_t * d = dz_dev(s->dz_idx);
        if (d) { known = 1; o = d->on; snprintf(name, n, "%s", d->name); }
        else snprintf(name, n, "Dev #%d", s->dz_idx);
    } else if (s->kind == TL_HA) {
        if (s->ha_i >= 0 && s->ha_i < ha_device_count) {
            const ha_device_t * d = &ha_devices[s->ha_i];
            known = d->available; o = d->on;
            snprintf(name, n, "%s", d->name);
        } else snprintf(name, n, "%s", s->id);
    } else if (s->kind == TL_ZW) {
        zwave_dev_t d;
        if (s->zw_i >= 0 && zw_get(s->zw_i, &d)) {
            known = 1; o = (d.state == 1);
            snprintf(name, n, "%s", d.name[0] ? d.name : d.type);
        } else snprintf(name, n, "Z-Wave");
    } else if (s->kind == TL_HY) {
        snprintf(name, n, "Homey");
        homey_lock();
        for (int i = 0; i < homey_state.ndev; i++) {
            const hy_dev_t * d = &homey_state.dev[i];
            if (strcmp(d->id, s->id)) continue;
            snprintf(name, n, "%s", d->name);
            const hy_cap_t * c = hy_dev_cap(d, "onoff");
            if (c && d->available) { known = 1; o = c->num != 0; }
            break;
        }
        homey_unlock();
    } else if (s->kind == TL_HUE) {
        if (s->hue_i >= 0 && s->hue_i < hue_state.count) {
            const hue_light_t * L = &hue_state.lights[s->hue_i];
            known = hue_state.connected && L->reachable; o = L->on;
            snprintf(name, n, "%s", L->name);
        } else snprintf(name, n, "Hue");
    } else if (s->kind == TL_HUEG) {
        if (s->hue_i >= 0 && s->hue_i < hue_state.group_count) {
            const hue_group_t * g = &hue_state.groups[s->hue_i];
            known = hue_state.connected; o = g->any_on;
            snprintf(name, n, "%s", g->name);
        } else snprintf(name, n, "Hue");
    } else if (s->kind == TL_HWS) {
        if (s->hue_i >= 0 && homewizard_socket_present(s->hue_i)) {
            known = hw_sockets[s->hue_i].connected; o = hw_sockets[s->hue_i].on;
            snprintf(name, n, "%s", homewizard_socket_name(s->hue_i));
        } else snprintf(name, n, "%s", s->id);
    }
    if (on) *on = o;
    return known;
}

int tl_level(const tl_src_t * s) {
    int lv = -1;
    if (s->kind == TL_DZ) {
        const domoticz_dev_t * d = dz_dev(s->dz_idx);
        if (d && d->kind == DZ_DIMMER) lv = d->level < 0 ? 0 : d->level;
    } else if (s->kind == TL_HA) {
        if (s->ha_i >= 0 && s->ha_i < ha_device_count) {
            const ha_device_t * d = &ha_devices[s->ha_i];
            if (d->type == HADEV_LIGHT && d->dimmable)
                lv = d->brightness < 0 ? 0 : (d->brightness * 100 + 127) / 255;
        }
    } else if (s->kind == TL_HY) {
        homey_lock();
        for (int i = 0; i < homey_state.ndev; i++) {
            const hy_dev_t * d = &homey_state.dev[i];
            if (strcmp(d->id, s->id)) continue;
            const hy_cap_t * c = hy_dev_cap(d, "dim");
            if (c && c->setable) lv = (int)(c->num * 100 + 0.5);
            break;
        }
        homey_unlock();
    } else if (s->kind == TL_HUE) {
        if (s->hue_i >= 0 && s->hue_i < hue_state.count && hue_state.lights[s->hue_i].is_dimmable)
            lv = hue_state.lights[s->hue_i].bri_pct;
    } else if (s->kind == TL_HUEG) {
        if (s->hue_i >= 0 && s->hue_i < hue_state.group_count)
            lv = hue_state.groups[s->hue_i].bri_pct;
    }
    if (lv > 100) lv = 100;
    return lv;
}

void tl_toggle(int slot) {
    tl_src_t s = tl_resolve(slot);
    if (s.kind == TL_DZ) domoticz_switch_async(s.dz_idx, "Toggle");
    else if (s.kind == TL_HA && s.ha_i >= 0) ha_device_toggle_async(ha_devices[s.ha_i].type, ha_devices[s.ha_i].entity_id);
    else if (s.kind == TL_ZW && s.zw_i >= 0) zwave_toggle_switch(s.zw_i);
    else if (s.kind == TL_HY) {
        char nm[8];
        int on = 0;
        if (tl_status(&s, nm, sizeof nm, &on)) homey_onoff(s.id, !on);
    } else if (s.kind == TL_HUE && s.hue_i >= 0) hue_light_toggle_async(s.id);
    else if (s.kind == TL_HUEG && s.hue_i >= 0) hue_group_set_on_async(s.id, !hue_state.groups[s.hue_i].any_on);
    else if (s.kind == TL_HWS && s.hue_i >= 0) homewizard_socket_set_async(s.hue_i, !hw_sockets[s.hue_i].on);
}

void tl_set_level(int slot, int pct) {
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    tl_src_t s = tl_resolve(slot);
    if (tl_level(&s) < 0) return;
    if (s.kind == TL_DZ) domoticz_set_level_async(s.dz_idx, pct);
    else if (s.kind == TL_HA && s.ha_i >= 0) {
        ha_device_t * d = &ha_devices[s.ha_i];
        d->brightness = (pct * 255 + 50) / 100;      /* the tile shows it until the next poll */
        d->on = 1;
        ha_light_set_brightness_async(d->entity_id, pct);
    } else if (s.kind == TL_HY) {
        char nm[8];
        int on = 0;
        if (tl_status(&s, nm, sizeof nm, &on) && !on) homey_onoff(s.id, 1);
        homey_dim(s.id, pct / 100.0);
    } else if (s.kind == TL_HUE) hue_light_set_bri_async(s.id, pct);
    else if (s.kind == TL_HUEG) hue_group_set_bri_async(s.id, pct);
}

tl_src_t tl_parse(const char * ref) { return parse_ref(ref); }

int tl_set_on_ref(const char * ref, int on) {
    tl_src_t s = parse_ref(ref);
    on = on ? 1 : 0;
    switch (s.kind) {
    case TL_DZ:   domoticz_switch_async(s.dz_idx, on ? "On" : "Off"); return 1;
    case TL_HA:
        if (s.ha_i < 0) return 0;
        ha_device_set_on_async(ha_devices[s.ha_i].type, ha_devices[s.ha_i].entity_id, on);
        return 1;
    case TL_HY:   return homey_onoff(s.id, on) == 0;
    case TL_ZW:   return zwave_set_switch_uuid(s.id, on);
    case TL_HUE:  if (s.hue_i < 0) return 0; hue_light_set_on_async(s.id, on); return 1;
    case TL_HUEG: if (s.hue_i < 0) return 0; hue_group_set_on_async(s.id, on); return 1;
    case TL_HWS:  if (s.hue_i < 0) return 0; homewizard_socket_set_async(s.hue_i, on); return 1;
    default:      return 0;
    }
}

void tl_enumerate(tl_enum_cb cb, void * ctx) {
    char ref[TL_REF_LEN + 4];
    if (dz_active()) {
        for (int i = 0; i < domoticz_state.count && i < DOMOTICZ_MAX_DEV; i++) {
            const domoticz_dev_t * d = &domoticz_state.dev[i];
            if (d->kind == DZ_BLIND) continue;
            snprintf(ref, sizeof ref, "dz:%d", d->idx);
            cb(ctx, "Domoticz", ref, d->name, 1, d->kind == DZ_DIMMER);
        }
    }
    if (settings.enable_ha) {
        for (int i = 0; i < ha_device_count && i < HA_DEVICE_MAX; i++) {
            const ha_device_t * d = &ha_devices[i];
            if (d->type != HADEV_LIGHT && d->type != HADEV_SWITCH) continue;
            snprintf(ref, sizeof ref, "ha:%s", d->entity_id);
            cb(ctx, "Home Assistant", ref, d->name, d->available, d->type == HADEV_LIGHT && d->dimmable);
        }
    }
    if (settings.enable_homey) {
        homey_lock();
        for (int i = 0; i < homey_state.ndev; i++) {
            const hy_dev_t * d = &homey_state.dev[i];
            if (!homey_switchable(d)) continue;
            snprintf(ref, sizeof ref, "hy:%s", d->id);
            const hy_cap_t * dc = hy_dev_cap(d, "dim");
            cb(ctx, "Homey", ref, d->name, d->available, dc && dc->setable);
        }
        homey_unlock();
    }
    if (settings.hue_enabled && hue_state.connected) {
        for (int i = 0; i < hue_state.group_count && i < HUE_MAX_GROUPS; i++) {
            const hue_group_t * g = &hue_state.groups[i];
            snprintf(ref, sizeof ref, "hg:%s", g->id);
            cb(ctx, tr("Hue - kamers en zones", "Hue - rooms and zones"), ref, g->name, 1, 1);
        }
        for (int i = 0; i < hue_state.count && i < HUE_MAX_LIGHTS; i++) {
            const hue_light_t * L = &hue_state.lights[i];
            snprintf(ref, sizeof ref, "hu:%s", L->id);
            cb(ctx, tr("Hue - lampen", "Hue - lamps"), ref, L->name, L->reachable, L->is_dimmable);
        }
    }
    for (int i = 0; i < HW_SOCKET_MAX; i++) {
        if (!settings.hw_socket_host[i][0]) continue;
        snprintf(ref, sizeof ref, "hs:%s", settings.hw_socket_host[i]);
        cb(ctx, "HomeWizard Energy Socket", ref, homewizard_socket_name(i), hw_sockets[i].connected, 0);
    }
    if (zwave_switch_count_cached() > 0) {
        zwave_dev_t d;
        for (int i = 0; zwave_get_switch(i, &d); i++) {
            snprintf(ref, sizeof ref, "zw:%s", d.uuid);
            cb(ctx, "Z-Wave", ref, d.name[0] ? d.name : d.type, 1, 0);
        }
    }
}
