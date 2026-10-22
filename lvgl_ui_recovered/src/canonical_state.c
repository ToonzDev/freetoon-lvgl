/*
 * canonical_state.c — FreeToon Canonical State Engine & Event Bus
 */

#include "canonical_state.h"
#include "boxtalk.h"
#include "homewizard.h"
#include "meteradapter.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define MAX_SUBSCRIBERS 16

typedef struct {
    canonical_state_cb_t cb;
    void * user_data;
    int in_use;
} subscriber_entry_t;

static subscriber_entry_t s_subscribers[MAX_SUBSCRIBERS];
static pthread_mutex_t    s_state_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- Observer Registry --- */

int canonical_state_subscribe(canonical_state_cb_t cb, void * user_data) {
    if (!cb) return -1;
    pthread_mutex_lock(&s_state_lock);
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (!s_subscribers[i].in_use) {
            s_subscribers[i].cb = cb;
            s_subscribers[i].user_data = user_data;
            s_subscribers[i].in_use = 1;
            pthread_mutex_unlock(&s_state_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&s_state_lock);
    return -1; /* Table full */
}

int canonical_state_unsubscribe(canonical_state_cb_t cb) {
    if (!cb) return -1;
    pthread_mutex_lock(&s_state_lock);
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (s_subscribers[i].in_use && s_subscribers[i].cb == cb) {
            s_subscribers[i].in_use = 0;
            s_subscribers[i].cb = NULL;
            s_subscribers[i].user_data = NULL;
            pthread_mutex_unlock(&s_state_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&s_state_lock);
    return -1; /* Not found */
}

void canonical_state_notify(uint32_t mask) {
    subscriber_entry_t copy[MAX_SUBSCRIBERS];
    pthread_mutex_lock(&s_state_lock);
    memcpy(copy, s_subscribers, sizeof(copy));
    pthread_mutex_unlock(&s_state_lock);

    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (copy[i].in_use && copy[i].cb) {
            copy[i].cb(mask, copy[i].user_data);
        }
    }
}

/* --- Snapshots --- */

void canonical_state_get_thermostat(canonical_thermostat_t * out) {
    if (!out) return;
    toon_state_t snap;
    boxtalk_get_snapshot(&snap);

    out->room_temp     = snap.indoor_temp;
    out->setpoint      = snap.setpoint;
    out->ch_setpoint   = snap.ch_setpoint;
    out->active_state  = snap.active_state;
    out->program_state = snap.program_state;
    out->burner_on     = snap.burner_on;
    out->dhw_on        = snap.dhw_on;
    out->connected     = snap.connected;
    out->ot_comm_error = snap.ot_comm_error;
}

void canonical_state_get_boiler(canonical_boiler_t * out) {
    if (!out) return;
    toon_state_t snap;
    boxtalk_get_snapshot(&snap);

    out->protocol       = (snap.boiler_type == 0) ? 0 : 1;
    out->modulation_pct = snap.modulation_level;
    out->in_temp        = snap.boiler_in_temp;
    out->out_temp       = snap.boiler_out_temp;
    out->water_pressure = snap.water_pressure;
    out->burner_on      = snap.burner_on;
    out->dhw_on         = snap.dhw_on;
    out->comm_error     = snap.ot_comm_error;
}

void canonical_state_get_sensors(canonical_sensors_t * out) {
    if (!out) return;
    toon_state_t snap;
    boxtalk_get_snapshot(&snap);

    out->humidity  = snap.humidity;
    out->eco2      = snap.eco2;
    out->tvoc      = snap.tvoc;
    out->air_label = air_quality_label(snap.eco2, snap.tvoc);
}

void canonical_state_get_energy(canonical_energy_t * out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    if (hw_state.connected_p1) {
        out->power_w          = hw_state.power_w;
        out->kwh_import_t1    = hw_state.kwh_import_t1;
        out->kwh_import_t2    = hw_state.kwh_import_t2;
        out->kwh_import_total = hw_state.kwh_import_total;
        out->kwh_export_total = hw_state.kwh_export_total;
        out->gas_m3           = hw_state.gas_m3;
        out->gas_hour_m3      = hw_state.gas_hour_m3;
        out->voltage_l1       = hw_state.voltage_l1_v;
        out->current_l1       = hw_state.current_l1_a;
        out->tariff           = hw_state.tariff;
        out->connected        = 1;
    } else if (meter_state.connected) {
        out->power_w   = meter_state.power_w;
        out->connected = 1;
    }
}

/* --- Actions & Setters --- */

int canonical_state_set_setpoint(float temp_c) {
    if (temp_c < 6.0f) temp_c = 6.0f;
    if (temp_c > 30.0f) temp_c = 30.0f;
    int rc = boxtalk_set_setpoint(temp_c);
    if (rc == 0) {
        canonical_state_notify(CSTATE_EVT_THERMOSTAT);
    }
    return rc;
}

int canonical_state_set_preset(int preset) {
    if (preset < -1 || preset > 3) return -1;
    int rc = boxtalk_set_program(preset);
    if (rc == 0) {
        canonical_state_notify(CSTATE_EVT_THERMOSTAT);
    }
    return rc;
}

int canonical_state_set_boiler_mode(int mode) {
    if (mode != 0 && mode != 1) return -1;
    int rc = boxtalk_set_boiler_type(mode);
    if (rc == 0) {
        canonical_state_notify(CSTATE_EVT_BOILER);
    }
    return rc;
}

/* --- JSON Renderers --- */

int canonical_state_render_thermostat_json(char * buf, size_t maxlen) {
    canonical_thermostat_t t;
    canonical_state_get_thermostat(&t);
    return snprintf(buf, maxlen,
        "{\"room_temp\":%.2f,\"setpoint\":%.2f,\"ch_setpoint\":%.2f,"
        "\"active_state\":%d,\"program_state\":%d,\"burner_on\":%d,"
        "\"dhw_on\":%d,\"connected\":%d,\"ot_comm_error\":%d}",
        t.room_temp, t.setpoint, t.ch_setpoint,
        t.active_state, t.program_state, t.burner_on,
        t.dhw_on, t.connected, t.ot_comm_error);
}

int canonical_state_render_boiler_json(char * buf, size_t maxlen) {
    canonical_boiler_t b;
    canonical_state_get_boiler(&b);
    return snprintf(buf, maxlen,
        "{\"protocol\":\"%s\",\"modulation_pct\":%.1f,\"inlet_temp\":%.2f,"
        "\"outlet_temp\":%.2f,\"water_pressure\":%.2f,\"burner_on\":%d,"
        "\"dhw_on\":%d,\"comm_error\":%d}",
        (b.protocol == 0) ? "opentherm" : "on_off",
        b.modulation_pct, b.in_temp, b.out_temp, b.water_pressure,
        b.burner_on, b.dhw_on, b.comm_error);
}

int canonical_state_render_sensors_json(char * buf, size_t maxlen) {
    canonical_sensors_t s;
    canonical_state_get_sensors(&s);
    return snprintf(buf, maxlen,
        "{\"humidity\":%.1f,\"eco2\":%d,\"tvoc\":%d,\"air_quality\":\"%s\"}",
        s.humidity, s.eco2, s.tvoc, s.air_label ? s.air_label : "");
}

int canonical_state_render_energy_json(char * buf, size_t maxlen) {
    canonical_energy_t e;
    canonical_state_get_energy(&e);
    return snprintf(buf, maxlen,
        "{\"power_w\":%.1f,\"kwh_import_t1\":%.3f,\"kwh_import_t2\":%.3f,"
        "\"kwh_import_total\":%.3f,\"kwh_export_total\":%.3f,"
        "\"gas_m3\":%.3f,\"gas_hour_m3\":%.3f,\"voltage_l1\":%.1f,"
        "\"current_l1\":%.2f,\"tariff\":%d,\"connected\":%d}",
        e.power_w, e.kwh_import_t1, e.kwh_import_t2,
        e.kwh_import_total, e.kwh_export_total,
        e.gas_m3, e.gas_hour_m3, e.voltage_l1,
        e.current_l1, e.tariff, e.connected);
}

int canonical_state_render_full_json(char * buf, size_t maxlen) {
    char therm[256], boiler[256], sensors[160], energy[320];
    canonical_state_render_thermostat_json(therm, sizeof(therm));
    canonical_state_render_boiler_json(boiler, sizeof(boiler));
    canonical_state_render_sensors_json(sensors, sizeof(sensors));
    canonical_state_render_energy_json(energy, sizeof(energy));

    const char * hw_name =
#ifdef TOON1
        "toon1";
#else
        "toon2";
#endif

    return snprintf(buf, maxlen,
        "{\"system\":{\"version\":\"" BUILD_VERSION "\",\"hardware\":\"%s\","
        "\"theme\":\"%s\"},"
        "\"thermostat\":%s,"
        "\"boiler\":%s,"
        "\"sensors\":%s,"
        "\"energy\":%s}",
        hw_name, settings.home_light_mode ? "light" : "dark",
        therm, boiler, sensors, energy);
}

