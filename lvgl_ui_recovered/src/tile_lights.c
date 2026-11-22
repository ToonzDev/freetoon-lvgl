/* The two switches on the Verlichting home tile: see tile_lights.h. */
#include "tile_lights.h"
#include "settings.h"
#include "domoticz.h"
#include "homeassistant.h"
#include "homey.h"
#include "screen_zwave.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char * slot_str(int slot) { return slot ? settings.tile_light2 : settings.tile_light1; }

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
    tl_src_t none = { TL_NONE, -1, -1, -1, "" };
    return slot < k ? c[slot] : none;
}

static tl_src_t parse_ref(const char * r) {
    tl_src_t s = { TL_NONE, -1, -1, -1, "" };
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
    else s.id[0] = 0;
    return s;
}

tl_src_t tl_resolve(int slot) {
    const char * r = slot_str(slot ? 1 : 0);
    if (!strcmp(r, "auto")) return auto_src(slot ? 1 : 0);
    return parse_ref(r);
}

void tl_ref(const tl_src_t * s, char * out, size_t n) {
    switch (s->kind) {
    case TL_DZ: snprintf(out, n, "dz:%d", s->dz_idx); break;
    case TL_HA: snprintf(out, n, "ha:%s", s->id); break;
    case TL_HY: snprintf(out, n, "hy:%s", s->id); break;
    case TL_ZW: snprintf(out, n, "zw:%s", s->id); break;
    default:    if (n) out[0] = 0;
    }
}

int tl_slot_of(const char * ref) {
    if (!ref || !ref[0]) return -1;
    for (int k = 0; k < 2; k++) {
        tl_src_t s = tl_resolve(k);
        char r[TL_REF_LEN];
        tl_ref(&s, r, sizeof r);
        if (!strcmp(r, ref)) return k;
    }
    return -1;
}

/* Replace "auto" by what the slots show now, so changing one slot does not
 * change what the other shows. */
static void freeze(void) {
    tl_src_t s[2] = { tl_resolve(0), tl_resolve(1) };
    for (int k = 0; k < 2; k++) {
        char * d = slot_str(k);
        if (strcmp(d, "auto")) continue;
        char r[TL_REF_LEN];
        tl_ref(&s[k], r, sizeof r);
        snprintf(d, sizeof settings.tile_light1, "%s", r);
    }
}

void tl_set(int slot, const char * ref) {
    slot = slot ? 1 : 0;
    if (!ref) ref = "";
    freeze();
    if (ref[0] && !strcmp(slot_str(!slot), ref)) slot_str(!slot)[0] = 0;
    snprintf(slot_str(slot), sizeof settings.tile_light1, "%s", ref);
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
    for (int k = 0; k < 2; k++)
        if (!slot_str(k)[0]) { tl_set(k, ref); return k; }
    if (!drop_oldest) { settings_save(); return -1; }
    snprintf(settings.tile_light1, sizeof settings.tile_light1, "%s", settings.tile_light2);
    snprintf(settings.tile_light2, sizeof settings.tile_light2, "%s", ref);
    settings_save();
    return 1;
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
    }
    if (on) *on = o;
    return known;
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
    }
}
