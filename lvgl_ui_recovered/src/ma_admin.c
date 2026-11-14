/*
 * Meter-adapter management — see ma_admin.h for the protocol summary.
 * Source of truth: stock qt-gui apps/eMetersSettings (EMetersSettingsApp.qml,
 * AddDeviceScreen.qml, RemoveDeviceScreen.qml, ManualConfigurationScreen.qml)
 * and qb/utils/ZWaveUtils.qml; dataset shapes checked on a real Toon 2.
 */
#include "ma_admin.h"
#include "boxtalk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static ma_info_t     g_info;
static ma_admin_t    g_admin;
static ma_sensconf_t g_sens;
static ma_caps_t     g_caps;
static char          g_caps_want[48];
static int           g_set_seq, g_set_ok;
static char          g_admin_rid[32];   /* requestid of the pending admin call */
static int           g_rid_n;

/* ---------------------------------------------------------------- helpers */

static void p1_uuid(char * out, size_t sz) {
    snprintf(out, sz, "%s:hdrv_p1", boxtalk_dev_base());
}

int ma_is_p1_uuid(const char * uuid) {
    char u[96]; p1_uuid(u, sizeof u);
    return uuid && strcmp(uuid, u) == 0;
}

/* uuids and sensor names go into XML/requests verbatim: allow only a safe set */
static int safe_token(const char * s) {
    if (!s || !*s) return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '-' || *s == ':' || *s == '_' || *s == '.')) return 0;
    return 1;
}

/* Text of the first <tag>...</tag> within [p, end) (end NULL = to NUL). */
static int elem(const char * p, const char * end, const char * tag, char * out, size_t sz) {
    char o[48], c[48];
    snprintf(o, sizeof o, "<%s>", tag);
    snprintf(c, sizeof c, "</%s>", tag);
    const char * a = strstr(p, o);
    if (!a || (end && a >= end)) return 0;
    a += strlen(o);
    const char * b = strstr(a, c);
    if (!b || (end && b > end)) return 0;
    size_t n = (size_t)(b - a);
    if (n >= sz) n = sz - 1;
    memcpy(out, a, n);
    out[n] = 0;
    return 1;
}

/* In-place minimal XML unescape. */
static void unescape(char * s) {
    static const struct { const char * e; char c; } m[] = {
        { "&quot;", '"' }, { "&apos;", '\'' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&amp;", '&' } };
    char * w = s;
    for (char * r = s; *r; ) {
        int hit = 0;
        if (*r == '&')
            for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
                size_t l = strlen(m[i].e);
                if (!strncmp(r, m[i].e, l)) { *w++ = m[i].c; r += l; hit = 1; break; }
            }
        if (!hit) *w++ = *r++;
    }
    *w = 0;
}

/* "key":"value" from a flat JSON text. */
static int json_str(const char * js, const char * key, char * out, size_t sz) {
    char k[48]; snprintf(k, sizeof k, "\"%s\"", key);
    const char * p = strstr(js, k);
    if (!p) return 0;
    p += strlen(k);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < sz) out[n++] = *p++;
    out[n] = 0;
    return 1;
}

/* Raw JSON value of "key" (object/array/number), for echoing keyGrant keys. */
static int json_raw(const char * js, const char * key, char * out, size_t sz) {
    char k[48]; snprintf(k, sizeof k, "\"%s\"", key);
    const char * p = strstr(js, k);
    if (!p) return 0;
    p += strlen(k);
    while (*p == ' ' || *p == ':') p++;
    const char * s = p;
    if (*p == '[' || *p == '{') {
        int depth = 0;
        for (; *p; p++) {
            if (*p == '[' || *p == '{') depth++;
            else if ((*p == ']' || *p == '}') && --depth == 0) { p++; break; }
        }
    } else {
        while (*p && *p != ',' && *p != '}') p++;
    }
    size_t n = (size_t)(p - s);
    if (n == 0 || n >= sz) return 0;
    memcpy(out, s, n);
    out[n] = 0;
    return 1;
}

static int send_p1(const char * verb, const char * inner, const char * tag) {
    char dst[96]; p1_uuid(dst, sizeof dst);
    static char xml[4096];
    snprintf(xml, sizeof xml,
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:specific1\" requestid=\"%d-%s\">"
        "<u:%s xmlns:u=\"urn:hcb-hae-com:service:specific1:1\">%s</u:%s></action>",
        boxtalk_our_uuid(), dst, getpid(), tag, verb, inner ? inner : "", verb);
    return boxtalk_send_raw_xml(xml);
}

/* Administration-service call on hdrv_zwave; remembers its requestid so only
 * the reply to the latest call counts. Caller holds g_mtx. */
static int send_admin_locked(const char * verb, const char * inner) {
    snprintf(g_admin_rid, sizeof g_admin_rid, "%d-ma%d", (int)getpid(), ++g_rid_n);
    char xml[2048];
    snprintf(xml, sizeof xml,
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:Administration\" requestid=\"%s\">"
        "<u:%s xmlns:u=\"urn:hcb-hae-com:service:Administration:1\">%s</u:%s></action>",
        boxtalk_our_uuid(), boxtalk_zwave_uuid(), g_admin_rid, verb, inner, verb);
    return boxtalk_send_raw_xml(xml);
}

/* ------------------------------------------------------------ subscribe */

void ma_subscribe(void) {
    char dst[96]; p1_uuid(dst, sizeof dst);
    char xml[1024];
    snprintf(xml, sizeof xml,
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:specific1\">"
        "<u:UpdateDataSetSubscription xmlns:u=\"urn:hcb-hae-com:service:specific1:1\">"
        "<updateAction>add</updateAction><dataSet>usageDevicesInfo</dataSet>"
        "<autoExpire>0</autoExpire><supportsNaN>1</supportsNaN><supportsPartial>0</supportsPartial>"
        "<requestFullSet>1</requestFullSet></u:UpdateDataSetSubscription></action>",
        boxtalk_our_uuid(), dst);
    boxtalk_send_raw_xml(xml);
}

/* ------------------------------------------------------------- parsers */

static void parse_usage_devices(const char * xml) {
    const char * root = strstr(xml, "<usageDevicesInfo>");
    if (!root) return;
    ma_info_t n; memset(&n, 0, sizeof n);
    const char * p = root, * last_end = root;
    while (n.n < MA_MAX_DEV) {
        const char * a = strstr(p, "<usageDeviceInfo>");
        if (!a) break;
        const char * b = strstr(a, "</usageDeviceInfo>");
        if (!b) break;
        ma_dev_t * d = &n.dev[n.n];
        char v[64];
        elem(a, b, "deviceUuid", d->uuid, sizeof d->uuid);
        elem(a, b, "deviceIdentifier", d->ident, sizeof d->ident);
        elem(a, b, "statusString", d->status_str, sizeof d->status_str);
        unescape(d->ident); unescape(d->status_str);
        if (elem(a, b, "deviceStatus", v, sizeof v)) d->dev_status = atoi(v);
        const char * u = a;
        while (d->n_usage < MA_MAX_USAGE) {
            const char * ua = strstr(u, "<usage>");
            if (!ua || ua >= b) break;
            const char * ub = strstr(ua, "</usage>");
            if (!ub || ub > b) break;
            ma_usage_t * us = &d->usage[d->n_usage];
            elem(ua, ub, "type", us->type, sizeof us->type);
            if (elem(ua, ub, "measureType", v, sizeof v)) us->measure = atoi(v);
            if (elem(ua, ub, "status", v, sizeof v)) us->status = atoi(v);
            if (us->type[0]) d->n_usage++;
            u = ub + 8;
        }
        if (d->uuid[0]) n.n++;
        p = last_end = b + 18;
    }
    if (elem(last_end, NULL, "statusString", n.status_str, sizeof n.status_str))
        unescape(n.status_str);
    pthread_mutex_lock(&g_mtx);
    n.seq = g_info.seq + 1;
    g_info = n;
    pthread_mutex_unlock(&g_mtx);
    fprintf(stderr, "[ma] usageDevicesInfo: %d adapter(s), '%s'\n", n.n, n.status_str);
}

/* Iterate <device>...</device> blocks (not <devices>). */
static const char * next_device(const char * p, const char ** end) {
    for (;;) {
        const char * a = strstr(p, "<device>");
        if (!a) return NULL;
        const char * b = strstr(a, "</device>");
        if (!b) return NULL;
        *end = b;
        return a;
    }
}

static void parse_sensor_config(const char * xml) {
    ma_sensconf_t n; memset(&n, 0, sizeof n);
    const char * p = xml, * a, * b;
    while (n.n < MA_MAX_DEV && (a = next_device(p, &b))) {
        ma_sens_t * d = &n.dev[n.n];
        elem(a, b, "deviceUuid", d->uuid, sizeof d->uuid);
        const char * s = a;
        while (d->n < MA_MAX_SENS) {
            const char * sa = strstr(s, "<sensor>");
            if (!sa || sa >= b) break;
            if (!elem(sa, b, "sensor", d->sensor[d->n], sizeof d->sensor[0])) break;
            d->n++;
            s = sa + 8;
        }
        if (d->uuid[0]) n.n++;
        p = b + 9;
    }
    pthread_mutex_lock(&g_mtx);
    n.seq = g_sens.seq + 1;
    g_sens = n;
    pthread_mutex_unlock(&g_mtx);
    fprintf(stderr, "[ma] sensor configuration: %d adapter(s)\n", n.n);
}

static void parse_caps(const char * xml) {
    ma_caps_t n; memset(&n, 0, sizeof n);
    pthread_mutex_lock(&g_mtx);
    snprintf(n.uuid, sizeof n.uuid, "%s", g_caps_want);
    pthread_mutex_unlock(&g_mtx);
    const char * p = xml, * a, * b;
    while ((a = next_device(p, &b))) {
        char u[48] = "";
        elem(a, b, "deviceUuid", u, sizeof u);
        if (!n.uuid[0] || !strcmp(u, n.uuid)) {
            const char * s = a;
            while (n.n < MA_MAX_CAPS) {
                const char * ca = strstr(s, "<capability>");
                if (!ca || ca >= b) break;
                if (!elem(ca, b, "capability", n.cap[n.n], sizeof n.cap[0])) break;
                n.n++;
                s = ca + 12;
            }
            break;
        }
        p = b + 9;
    }
    pthread_mutex_lock(&g_mtx);
    n.seq = g_caps.seq + 1;
    g_caps = n;
    pthread_mutex_unlock(&g_mtx);
    fprintf(stderr, "[ma] capabilities for %s: %d\n", n.uuid, n.n);
}

/* Include/Exclude reply: <pb> holds JSON {status, devType, uuid, ...}. */
static void parse_admin_reply(const char * xml, int is_include) {
    char rid[48] = "";
    const char * r = strstr(xml, "requestid=\"");
    if (r) { r += 11; size_t n = strcspn(r, "\""); if (n < sizeof rid) { memcpy(rid, r, n); rid[n] = 0; } }

    static char js[4096];
    if (!elem(xml, NULL, "pb", js, sizeof js)) snprintf(js, sizeof js, "%s", xml);
    unescape(js);
    char status[24] = "", typ[48] = "", uuid[48] = "";
    json_str(js, "status", status, sizeof status);
    json_str(js, "devType", typ, sizeof typ);
    json_str(js, "uuid", uuid, sizeof uuid);

    pthread_mutex_lock(&g_mtx);
    if (strcmp(rid, g_admin_rid) != 0) {          /* reply to a superseded call (e.g. stop) */
        pthread_mutex_unlock(&g_mtx);
        fprintf(stderr, "[ma] ignoring stale admin reply %s status=%s\n", rid, status);
        return;
    }
    if (is_include && !strcmp(status, "keyGrant")) {
        /* S2: grant every requested key, as the stock UI does. */
        char keys[512] = "[]", inner[768];
        json_raw(js, "keys", keys, sizeof keys);
        snprintf(inner, sizeof inner,
                 "<event>keyGrant</event><accept>1</accept><keys>%s</keys><csa>0</csa>", keys);
        send_admin_locked("IncludeDevice", inner);
        pthread_mutex_unlock(&g_mtx);
        fprintf(stderr, "[ma] include: keyGrant -> accepted keys %s\n", keys);
        return;
    }
    if (is_include && !strcmp(status, "ssaAuth")) {
        /* S2 PIN entry: not supported, decline so the driver ends the attempt. */
        g_admin.ssa_declined = 1;
        send_admin_locked("IncludeDevice",
            "<event>ssaAuth</event><eventTimeout>60</eventTimeout><accept>0</accept><dskUserInput></dskUserInput>");
        pthread_mutex_unlock(&g_mtx);
        fprintf(stderr, "[ma] include: ssaAuth requested -> declined\n");
        return;
    }
    g_admin.busy = 0;
    snprintf(g_admin.status, sizeof g_admin.status, "%s", status[0] ? status : "failed");
    snprintf(g_admin.dev_type, sizeof g_admin.dev_type, "%s", typ);
    snprintf(g_admin.uuid, sizeof g_admin.uuid, "%s", uuid);
    g_admin.seq++;
    pthread_mutex_unlock(&g_mtx);
    fprintf(stderr, "[ma] %s result: status=%s devType=%s uuid=%s\n",
            is_include ? "include" : "exclude", status, typ, uuid);
}

void ma_handle_action(const char * xml) {
    if (strstr(xml, "usageDevicesInfo") && strstr(xml, "UpdateDataSet")) {
        parse_usage_devices(xml);
        return;
    }
    if (!strstr(xml, "class=\"response\"")) return;
    if      (strstr(xml, "IncludeDeviceResponse"))          parse_admin_reply(xml, 1);
    else if (strstr(xml, "ExcludeDeviceResponse"))          parse_admin_reply(xml, 0);
    else if (strstr(xml, "GetSensorConfigurationResponse")) parse_sensor_config(xml);
    else if (strstr(xml, "GetMeasureCapabilitiesResponse")) parse_caps(xml);
    else if (strstr(xml, "SetSensorConfigurationResponse")) {
        int ok = !strstr(xml, "false") && !strstr(xml, "rror");
        pthread_mutex_lock(&g_mtx);
        g_set_ok = ok; g_set_seq++;
        pthread_mutex_unlock(&g_mtx);
        fprintf(stderr, "[ma] SetSensorConfiguration -> %s: %.300s\n", ok ? "ok" : "FAILED", xml);
    }
}

/* -------------------------------------------------------------- public */

void ma_get_info(ma_info_t * out)           { pthread_mutex_lock(&g_mtx); *out = g_info;  pthread_mutex_unlock(&g_mtx); }
void ma_get_admin(ma_admin_t * out)         { pthread_mutex_lock(&g_mtx); *out = g_admin; pthread_mutex_unlock(&g_mtx); }
void ma_get_sensor_config(ma_sensconf_t * o){ pthread_mutex_lock(&g_mtx); *o = g_sens;    pthread_mutex_unlock(&g_mtx); }
void ma_get_caps(ma_caps_t * out)           { pthread_mutex_lock(&g_mtx); *out = g_caps;  pthread_mutex_unlock(&g_mtx); }
void ma_get_set_result(int * seq, int * ok) { pthread_mutex_lock(&g_mtx); *seq = g_set_seq; *ok = g_set_ok; pthread_mutex_unlock(&g_mtx); }

static int admin_start(int op, const char * verb, const char * inner) {
    pthread_mutex_lock(&g_mtx);
    g_admin.op = op;
    g_admin.busy = 1;
    g_admin.ssa_declined = 0;
    g_admin.status[0] = g_admin.dev_type[0] = g_admin.uuid[0] = 0;
    int rc = send_admin_locked(verb, inner);
    if (rc != 0) g_admin.busy = 0;
    pthread_mutex_unlock(&g_mtx);
    fprintf(stderr, "[ma] %s %s rc=%d\n", verb, inner, rc);
    return rc;
}

static int admin_stop(const char * verb, const char * inner) {
    pthread_mutex_lock(&g_mtx);
    int rc = send_admin_locked(verb, inner);  /* supersedes the running call */
    g_admin.busy = 0;
    snprintf(g_admin.status, sizeof g_admin.status, "canceled");
    g_admin.seq++;
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

int ma_include_start(void) { return admin_start(MA_OP_INCLUDE, "IncludeDevice", "<event>add</event><eventTimeout>120</eventTimeout>"); }
int ma_include_stop(void)  { return admin_stop("IncludeDevice", "<event>stop</event><eventTimeout>120</eventTimeout>"); }
int ma_exclude_start(void) { return admin_start(MA_OP_EXCLUDE, "ExcludeDevice", "<event>delete</event><eventTimeout>30</eventTimeout>"); }
int ma_exclude_stop(void)  { return admin_stop("ExcludeDevice", "<event>stop</event><eventTimeout>30</eventTimeout>"); }

void ma_admin_timeout(void) {
    pthread_mutex_lock(&g_mtx);
    if (g_admin.busy) {
        g_admin.busy = 0;
        snprintf(g_admin.status, sizeof g_admin.status, "timeout");
        g_admin_rid[0] = 0;
        g_admin.seq++;
    }
    pthread_mutex_unlock(&g_mtx);
}

int ma_force_remove(const char * dev_uuid) {
    if (!safe_token(dev_uuid)) return -1;
    char xml[1024];
    snprintf(xml, sizeof xml,
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:specific1\" requestid=\"%d-marm\">"
        "<u:RemoveDevice xmlns:u=\"urn:hcb-hae-com:service:specific1:1\">"
        "<uuid>%s</uuid></u:RemoveDevice></action>",
        boxtalk_our_uuid(), boxtalk_zwave_uuid(), getpid(), dev_uuid);
    fprintf(stderr, "[ma] force RemoveDevice %s\n", dev_uuid);
    return boxtalk_send_raw_xml(xml);
}

/* Same list the stock AddDeviceScreen accepts as a metering device. */
int ma_is_meter_type(const char * t) {
    static const char * const ok[] = { "HAE_METER", "HOME_ENERGY_METER", "ZMNHTD1", "ZMNHXD1", "ECM_MODULE" };
    if (!t) return 0;
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (strstr(t, ok[i])) return 1;
    return 0;
}

int ma_request_sensor_config(void) { return send_p1("GetSensorConfiguration", NULL, "magsc"); }

int ma_request_caps(const char * dev_uuid) {
    if (!safe_token(dev_uuid)) return -1;
    pthread_mutex_lock(&g_mtx);
    snprintf(g_caps_want, sizeof g_caps_want, "%s", dev_uuid);
    pthread_mutex_unlock(&g_mtx);
    char inner[128];
    snprintf(inner, sizeof inner, "<deviceUuid>%s</deviceUuid>", dev_uuid);
    return send_p1("GetMeasureCapabilities", inner, "magmc");
}

int ma_set_sensor_config(const ma_sensconf_t * all) {
    static char inner[3072];
    size_t o = 0;
    o += snprintf(inner + o, sizeof inner - o, "<devices>");
    for (int i = 0; i < all->n && o < sizeof inner; i++) {
        const ma_sens_t * d = &all->dev[i];
        if (!safe_token(d->uuid)) continue;
        o += snprintf(inner + o, sizeof inner - o,
                      "<device><deviceUuid>%s</deviceUuid><sensors>", d->uuid);
        for (int s = 0; s < d->n && o < sizeof inner; s++)
            if (safe_token(d->sensor[s]))
                o += snprintf(inner + o, sizeof inner - o, "<sensor>%s</sensor>", d->sensor[s]);
        if (o < sizeof inner) o += snprintf(inner + o, sizeof inner - o, "</sensors></device>");
    }
    if (o >= sizeof inner - 16) return -1;
    snprintf(inner + o, sizeof inner - o, "</devices>");
    fprintf(stderr, "[ma] SetSensorConfiguration %s\n", inner);
    return send_p1("SetSensorConfiguration", inner, "massc");
}
