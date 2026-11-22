/* Athom Homey Pro (2023) local Web API poller (BACKLOG #28). See homey.h.
 *
 *   GET http://<homey>/api/manager/devices/device/      every HOMEY_POLL_S
 *   GET http://<homey>/api/manager/zones/zone/          every HOMEY_META_S
 *   GET http://<homey>/api/manager/flow/flow/           every HOMEY_META_S
 *   GET http://<homey>/api/manager/flow/advancedflow/   every HOMEY_META_S
 *   Authorization: Bearer <API key>
 *
 * The device list of a real install is several hundred kB (app settings,
 * insights, UI hints), so the body goes into a growing heap buffer that is
 * freed after parsing; only the compact parsed table stays resident. */
#include "homey.h"
#include "settings.h"
#include "shell_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>

#define HOMEY_KEY_PATH  "/mnt/data/homey.key"
#define HOMEY_MAX_BODY  (4 * 1024 * 1024)

homey_state_t homey_state;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
void homey_lock(void)   { pthread_mutex_lock(&g_mu); }
void homey_unlock(void) { pthread_mutex_unlock(&g_mu); }

static volatile int g_kick = 0;
/* Bumped on every settings change. The status of a poll that started with
 * the old host/key is thrown away, so screens show "connecting" until the
 * new values have been tried, not a stale "connected" or "key refused". */
static volatile unsigned g_cfg_gen = 0;
void homey_kick(void) {
    __sync_fetch_and_add(&g_cfg_gen, 1);
    homey_state.status = HOMEY_ST_OFF;   /* shown as "connecting" while enabled */
    g_kick = 1;
}

/* Set by the control worker after each request: poll devices soon (without
 * reloading zones/flows) to confirm the optimistic value. */
static volatile int g_refresh = 0;
/* Bumped when an action is queued and when it completes. A device poll that
 * overlapped an action is discarded, so it cannot briefly undo the value the
 * user just set (the refresh poll right after brings the real state). */
static volatile unsigned g_act_gen = 0;

/* ------------------------------------------------------------- API key */

static char g_key[160];

/* Homey API keys look like "<uuid>:<uuid>:<hex>". Anything outside this set
 * cannot be a key and is refused (it also keeps the curl header simple). */
static int key_safe(const char * k) {
    if (!k || !*k) return 0;
    for (const char * p = k; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == ':' || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static void load_key(void) {
#ifdef SIM
    { const char * t = getenv("HOMEY_SIM_KEY");
      if (t && *t) { snprintf(g_key, sizeof g_key, "%s", t); return; } }
#endif
    g_key[0] = 0;
    FILE * f = fopen(HOMEY_KEY_PATH, "r");
    if (!f) return;
    if (fgets(g_key, sizeof g_key, f)) g_key[strcspn(g_key, " \r\n\t")] = 0;
    fclose(f);
    if (!key_safe(g_key)) {
        if (g_key[0]) fprintf(stderr, "[homey] key in %s has invalid characters - ignored\n", HOMEY_KEY_PATH);
        g_key[0] = 0;
    }
}

void homey_get_key(char * out, size_t n) {
    if (!out || !n) return;
    if (!g_key[0]) load_key();
    snprintf(out, n, "%s", g_key);
}

int homey_set_key(const char * key) {
    if (!key) return -1;
    while (*key == ' ' || *key == '\t') key++;
    char clean[sizeof g_key];
    snprintf(clean, sizeof clean, "%s", key);
    clean[strcspn(clean, " \r\n\t")] = 0;
    if (clean[0] && !key_safe(clean)) return -1;
    mode_t old = umask(077);
    FILE * f = fopen(HOMEY_KEY_PATH, "w");
    umask(old);
#ifdef SIM
    if (!f) { snprintf(g_key, sizeof g_key, "%s", clean); homey_kick(); return 0; }  /* no /mnt/data on a PC */
#endif
    if (!f) return -1;
    fprintf(f, "%s\n", clean);
    fclose(f);
    chmod(HOMEY_KEY_PATH, 0600);
    snprintf(g_key, sizeof g_key, "%s", clean);
    homey_kick();
    return 0;
}

/* --------------------------------------------------------------- fetch */

/* "ip", "ip:port", "http://ip[:port]" or "https://..." -> scheme + hostport.
 * Returns 0 if the host is usable. */
static int base_url(char * out, size_t n, int * tls) {
    const char * h = settings.homey_host;
    *tls = 0;
    if (!strncmp(h, "http://", 7)) h += 7;
    else if (!strncmp(h, "https://", 8)) { h += 8; *tls = 1; }
    char hp[sizeof settings.homey_host];
    size_t L = 0;                            /* bounded copy: h points into settings */
    while (L < sizeof hp - 1 && h[L]) L++;
    memcpy(hp, h, L);
    hp[L] = 0;
    hp[strcspn(hp, "/")] = 0;
    if (!hp[0] || !sh_is_hostport(hp)) return -1;
    snprintf(out, n, "%s://%s", *tls ? "https" : "http", hp);
    return 0;
}

/* <method> <base>/api/<path> with the bearer key (and a JSON body for
 * PUT/POST). On success returns the HTTP status (200, 401, ...) and *body
 * (heap, NUL-terminated, caller frees) with *len; returns 0 if the host could
 * not be reached. */
static int homey_req(const char * method, const char * path, const char * json,
                     char ** body, size_t * len) {
    *body = NULL; *len = 0;
    char base[128];
    int tls;
    if (!g_key[0] || base_url(base, sizeof base, &tls) != 0) return 0;

    char url[256], qurl[300], hdr[200], qhdr[240], qjson[200], cmd[1024];
    snprintf(url, sizeof url, "%s/api/%s", base, path);
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s", g_key);
    if (!sh_quote(qurl, sizeof qurl, url) || !sh_quote(qhdr, sizeof qhdr, hdr)) return 0;
    char extra[260] = "";
    if (strcmp(method, "GET") != 0) {
        /* Only fixed method names; the body is built here from validated
         * parts and quoted all the same. */
        if (strcmp(method, "PUT") && strcmp(method, "POST")) return 0;
        if (!sh_quote(qjson, sizeof qjson, json ? json : "{}")) return 0;
        snprintf(extra, sizeof extra, "-X %s -H 'Content-Type: application/json' --data %s ",
                 method, qjson);
    }
    /* -k for https: a Homey on the LAN has no publicly trusted certificate.
     * The status code is appended on its own last line. */
    int cl = snprintf(cmd, sizeof cmd,
             "/usr/bin/curl -s -4 %s--compressed --max-time 20 --connect-timeout 4 "
             "-H %s %s-w '\\n%%{http_code}' %s 2>/dev/null",
             tls ? "-k " : "", qhdr, extra, qurl);
    if (cl < 0 || (size_t)cl >= sizeof cmd) return 0;   /* never run a cut-off command */

    FILE * p = popen(cmd, "r");
    if (!p) return 0;
    size_t cap = 256 * 1024, n = 0;
    char * buf = malloc(cap);
    if (!buf) { pclose(p); return 0; }
    for (;;) {
        if (n + 1 >= cap) {
            if (cap >= HOMEY_MAX_BODY) break;          /* too big: stop reading */
            size_t nc = cap * 2 > HOMEY_MAX_BODY ? HOMEY_MAX_BODY : cap * 2;
            char * nb = realloc(buf, nc);
            if (!nb) break;
            buf = nb; cap = nc;
        }
        size_t k = fread(buf + n, 1, cap - 1 - n, p);
        if (k == 0) break;
        n += k;
    }
    buf[n] = 0;
    pclose(p);

    /* Split off the status line. */
    char * nl = n ? strrchr(buf, '\n') : NULL;
    int code = nl ? atoi(nl + 1) : 0;
    if (nl) { *nl = 0; n = (size_t)(nl - buf); }
    if (code <= 0) { free(buf); return 0; }
    *body = buf; *len = n;
    return code;
}

static int homey_get(const char * path, char ** body, size_t * len) {
    return homey_req("GET", path, NULL, body, len);
}

/* -------------------------------------------------------------- poller */

static hy_dev_t  g_dev[HY_MAX_DEV];     /* parse targets, then copied */
static hy_zone_t g_zone[HY_MAX_ZONE];
static hy_flow_t g_flow[HY_MAX_FLOW];

/* One device poll. Returns HOMEY_ST_* and stores the table on success. */
/* Serialises fetch+parse: the poller thread and homey_probe() (Settings)
 * share the static parse targets. */
static pthread_mutex_t g_poll_mu = PTHREAD_MUTEX_INITIALIZER;

static int poll_devices_locked(int * count_out);
static int poll_devices(int * count_out) {
    pthread_mutex_lock(&g_poll_mu);
    int st = poll_devices_locked(count_out);
    pthread_mutex_unlock(&g_poll_mu);
    return st;
}

static int poll_devices_locked(int * count_out) {
    unsigned act_before = g_act_gen;
    char * body; size_t len;
    int code = homey_get("manager/devices/device/", &body, &len);
    if (code == 0) return HOMEY_ST_NOCONN;
    if (code == 401 || code == 403) { free(body); return HOMEY_ST_AUTH; }
    int n = code == 200 ? hy_parse_devices(body, len, g_dev, HY_MAX_DEV) : -1;
    free(body);
    if (n < 0) return HOMEY_ST_BADDATA;
    if (count_out) *count_out = n;
    if (g_act_gen != act_before) {
        g_refresh = 1;                 /* overlapped an action: re-poll instead */
        return HOMEY_ST_OK;
    }
    homey_lock();
    memcpy(homey_state.dev, g_dev, sizeof(hy_dev_t) * (size_t)n);
    homey_state.ndev = n;
    homey_state.gen++;
    homey_state.last_ok = (long)time(NULL);
    homey_unlock();
    return HOMEY_ST_OK;
}

static void poll_meta(void) {
    pthread_mutex_lock(&g_poll_mu);
    char * body; size_t len;
    if (homey_get("manager/zones/zone/", &body, &len) == 200) {
        int n = hy_parse_zones(body, len, g_zone, HY_MAX_ZONE);
        if (n >= 0) {
            homey_lock();
            memcpy(homey_state.zone, g_zone, sizeof(hy_zone_t) * (size_t)n);
            homey_state.nzone = n;
            homey_unlock();
        }
    }
    free(body);

    int nf = 0, ok = 0;
    if (homey_get("manager/flow/flow/", &body, &len) == 200) {
        int n = hy_parse_flows(body, len, 0, g_flow, 0, HY_MAX_FLOW);
        if (n >= 0) { nf = n; ok = 1; }
    }
    free(body);
    if (homey_get("manager/flow/advancedflow/", &body, &len) == 200) {
        int n = hy_parse_flows(body, len, 1, g_flow, nf, HY_MAX_FLOW);
        if (n >= 0) { nf = n; ok = 1; }
    }
    free(body);
    if (ok) {
        homey_lock();
        memcpy(homey_state.flow, g_flow, sizeof(hy_flow_t) * (size_t)nf);
        homey_state.nflow = nf;
        homey_unlock();
    }
    pthread_mutex_unlock(&g_poll_mu);
}

const char * homey_zone_name(const char * zone_id) {
    if (!zone_id || !zone_id[0]) return "";
    for (int i = 0; i < homey_state.nzone; i++)
        if (!strcmp(homey_state.zone[i].id, zone_id)) return homey_state.zone[i].name;
    return "";
}

static int configured(void) {
    if (!settings.enable_homey || !settings.homey_host[0]) return 0;
    if (!g_key[0]) load_key();
    return g_key[0] != 0;
}

static void * homey_thread(void * arg) {
    (void)arg;
    time_t last_meta = 0;
    int last_status = -1;
    for (;;) {
        if (!configured()) {
            homey_state.status = HOMEY_ST_OFF;
            last_meta = 0;
            for (int i = 0; i < 5 && !g_kick; i++) sleep(1);
            g_kick = 0;
            continue;
        }
        int n = 0;
        unsigned cfg = g_cfg_gen;
        int st = poll_devices(&n);
        if (cfg != g_cfg_gen) { g_kick = 0; last_meta = 0; continue; }   /* settings changed meanwhile: poll again with the new ones */
        homey_state.status = st;
        if (st != last_status) {
            if (st == HOMEY_ST_OK) fprintf(stderr, "[homey] connected: %d devices\n", n);
            else fprintf(stderr, "[homey] poll failed: %s\n",
                         st == HOMEY_ST_AUTH ? "API key refused (401/403)" :
                         st == HOMEY_ST_NOCONN ? "host unreachable" : "unexpected response");
            last_status = st;
        }
        if (st == HOMEY_ST_OK && (last_meta == 0 || time(NULL) - last_meta >= HOMEY_META_S)) {
            poll_meta();
            last_meta = time(NULL);
        }
        /* Back off when the Homey is unreachable or refuses the key. */
        int wait = st == HOMEY_ST_OK ? HOMEY_POLL_S : 30;
        for (int i = 0; i < wait && !g_kick && !g_refresh; i++) sleep(1);
        g_refresh = 0;
        if (g_kick) { g_kick = 0; last_meta = 0; }
    }
    return NULL;
}

int homey_start(void) {
    static volatile int started = 0;
    if (!__sync_bool_compare_and_swap(&started, 0, 1)) { homey_kick(); return 0; }
    pthread_t t;
    if (pthread_create(&t, NULL, homey_thread, NULL) != 0) { started = 0; return -1; }
    pthread_detach(t);
    return 0;
}

int homey_probe(void) {
    if (!settings.homey_host[0]) return -HOMEY_ST_NOCONN;
    if (!g_key[0]) load_key();
    if (!g_key[0]) return -HOMEY_ST_AUTH;
    int n = 0, st = HOMEY_ST_NOCONN;
    for (int tries = 0; tries < 2; tries++) {
        unsigned cfg = g_cfg_gen;
        st = poll_devices(&n);
        if (cfg != g_cfg_gen) continue;          /* host/key changed during the test: test again */
        homey_state.status = st;                 /* the status line agrees with the test result */
        break;
    }
    return st == HOMEY_ST_OK ? n : -st;
}

/* ------------------------------------------------------------- control
 *
 *   PUT  /api/manager/devices/device/<id>/capability/<cap>   {"value": X}
 *   POST /api/manager/flow/flow/<id>/trigger                 {}
 *   POST /api/manager/flow/advancedflow/<id>/trigger         {}
 *
 * Calls come from the LVGL thread and must never block it: they are queued
 * and sent by one worker thread, in order. A newer value for the same device
 * and capability replaces a queued one, so dragging a dim slider sends the
 * last position instead of every step. The local state is updated at once
 * (the screen reacts immediately); a poll shortly after the request confirms
 * or corrects it. */

enum { ACT_CAP = 0, ACT_FLOW };

typedef struct {
    int      used;
    unsigned seq;
    int      type;
    int      advanced;
    char     id[HY_ID_LEN];
    char     cap[32];
    char     json[64];
} act_t;

#define ACT_Q 16
static act_t           g_q[ACT_Q];
static unsigned        g_qseq;
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_qcv = PTHREAD_COND_INITIALIZER;

static int id_safe(const char * s) {
    size_t n = 0;
    for (; s && s[n]; n++) {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
            return 0;
    }
    return n > 0 && n < HY_ID_LEN;
}

static int cap_safe(const char * s) {
    size_t n = 0;
    for (; s && s[n]; n++) {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return 0;
    }
    return n > 0 && n < 32;
}

static int enum_safe(const char * s) {
    size_t n = 0;
    for (; s && s[n]; n++) {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return n > 0 && n < 24;
}

static void * act_thread(void * arg);

static void act_start_once(void) {
    static volatile int started = 0;
    if (!__sync_bool_compare_and_swap(&started, 0, 1)) return;
    pthread_t t;
    if (pthread_create(&t, NULL, act_thread, NULL) != 0) { started = 0; return; }
    pthread_detach(t);
}

static int enqueue(const act_t * a) {
    act_start_once();
    pthread_mutex_lock(&g_qmu);
    if (a->type == ACT_CAP) {
        for (int i = 0; i < ACT_Q; i++)
            if (g_q[i].used && g_q[i].type == ACT_CAP &&
                !strcmp(g_q[i].id, a->id) && !strcmp(g_q[i].cap, a->cap)) {
                /* Coalesce: keep the queue position, take the newest value. */
                snprintf(g_q[i].json, sizeof g_q[i].json, "%s", a->json);
                g_act_gen++;
                pthread_mutex_unlock(&g_qmu);
                return 0;
            }
    }
    act_t * slot = NULL;
    for (int i = 0; i < ACT_Q && !slot; i++) if (!g_q[i].used) slot = &g_q[i];
    if (!slot) {
        pthread_mutex_unlock(&g_qmu);
        fprintf(stderr, "[homey] action queue full, dropped\n");
        return -1;
    }
    *slot = *a;
    slot->used = 1;
    slot->seq = ++g_qseq;
    g_act_gen++;
    pthread_cond_signal(&g_qcv);
    pthread_mutex_unlock(&g_qmu);
    return 0;
}

static void * act_thread(void * arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_qmu);
        act_t * next;
        for (;;) {
            next = NULL;
            for (int i = 0; i < ACT_Q; i++)
                if (g_q[i].used && (!next || g_q[i].seq < next->seq)) next = &g_q[i];
            if (next) break;
            pthread_cond_wait(&g_qcv, &g_qmu);
        }
        act_t a = *next;
        next->used = 0;
        pthread_mutex_unlock(&g_qmu);

        char path[200];
        if (a.type == ACT_CAP)
            snprintf(path, sizeof path, "manager/devices/device/%s/capability/%s", a.id, a.cap);
        else
            snprintf(path, sizeof path, "manager/flow/%s/%s/trigger",
                     a.advanced ? "advancedflow" : "flow", a.id);
        char * body; size_t len;
        int code = homey_req(a.type == ACT_CAP ? "PUT" : "POST", path, a.json, &body, &len);
        free(body);
        homey_state.last_action_code = code;
        if (code >= 200 && code < 300) {
            fprintf(stderr, "[homey] %s %s%s%s -> %d\n", a.type == ACT_CAP ? "set" : "flow",
                    a.id, a.type == ACT_CAP ? " " : "", a.type == ACT_CAP ? a.cap : "", code);
        } else {
            homey_state.action_fail_gen++;
            if (code == 401 || code == 403) homey_state.status = HOMEY_ST_AUTH;
            fprintf(stderr, "[homey] %s %s%s%s failed: %s%d\n", a.type == ACT_CAP ? "set" : "flow",
                    a.id, a.type == ACT_CAP ? " " : "", a.type == ACT_CAP ? a.cap : "",
                    code ? "HTTP " : "no connection ", code);
        }
        g_act_gen++;
        g_refresh = 1;   /* confirm (or correct the optimistic value) soon */
    }
    return NULL;
}

/* Optimistic local update. Returns 0 if the device has the capability and it
 * can be set; -1 otherwise (nothing is queued then). */
static int local_set(const char * dev_id, const char * cap, int type, double num, const char * str) {
    int rc = -1;
    homey_lock();
    for (int i = 0; i < homey_state.ndev; i++) {
        hy_dev_t * d = &homey_state.dev[i];
        if (strcmp(d->id, dev_id)) continue;
        if (!d->available) break;
        if (homey_read_only(d)) break;   /* never unlock / open a garage door from here */
        for (int c = 0; c < d->ncap; c++) {
            hy_cap_t * k = &d->cap[c];
            if (strcmp(k->id, cap)) continue;
            if (!k->setable) break;
            /* Momentary capabilities have no state to show. */
            if (strcmp(cap, "play_scene") && strcmp(cap, "button")) {
                k->type = (unsigned char)type;
                k->num = num;
                if (type == HY_V_STR) snprintf(k->str, sizeof k->str, "%s", str);
            }
            homey_state.gen++;
            rc = 0;
            break;
        }
        break;
    }
    homey_unlock();
    return rc;
}

static int set_cap(const char * dev_id, const char * cap, int type, double num, const char * str) {
    if (!id_safe(dev_id) || !cap_safe(cap)) return -1;
    act_t a;
    memset(&a, 0, sizeof a);
    a.type = ACT_CAP;
    snprintf(a.id, sizeof a.id, "%s", dev_id);
    snprintf(a.cap, sizeof a.cap, "%s", cap);
    if (type == HY_V_BOOL) {
        snprintf(a.json, sizeof a.json, "{\"value\":%s}", num != 0 ? "true" : "false");
    } else if (type == HY_V_NUM) {
        snprintf(a.json, sizeof a.json, "{\"value\":%.4g}", num);
    } else {
        if (!enum_safe(str)) return -1;
        snprintf(a.json, sizeof a.json, "{\"value\":\"%s\"}", str);
    }
    if (local_set(dev_id, cap, type, num, str) != 0) return -1;
    return enqueue(&a);
}

static double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

int homey_set_bool(const char * dev_id, const char * cap, int on) {
    return set_cap(dev_id, cap, HY_V_BOOL, on ? 1 : 0, NULL);
}

int homey_set_num(const char * dev_id, const char * cap, double v) {
    /* Homey's 0..1 capabilities. */
    if (!strcmp(cap, "dim") || !strncmp(cap, "light_", 6) || !strcmp(cap, "windowcoverings_set"))
        v = clamp(v, 0, 1);
    else if (!strcmp(cap, "target_temperature"))
        v = clamp(v, 4, 35);
    return set_cap(dev_id, cap, HY_V_NUM, v, NULL);
}

int homey_set_enum(const char * dev_id, const char * cap, const char * value) {
    return set_cap(dev_id, cap, HY_V_STR, 0, value);
}

int homey_onoff(const char * dev_id, int on)        { return homey_set_bool(dev_id, "onoff", on); }
int homey_dim(const char * dev_id, double level)    { return homey_set_num(dev_id, "dim", level); }

int homey_cover(const char * dev_id, const char * state) {
    if (!state || (strcmp(state, "up") && strcmp(state, "idle") && strcmp(state, "down"))) return -1;
    return homey_set_enum(dev_id, "windowcoverings_state", state);
}

int homey_press(const char * dev_id) {
    /* Scenes (play_scene) and buttons (WOL etc.): write true. */
    if (homey_set_bool(dev_id, "play_scene", 1) == 0) return 0;
    return homey_set_bool(dev_id, "button", 1);
}

int homey_read_only(const hy_dev_t * d) {
    return d->kind == HY_K_LOCK || !strcmp(d->cls, "garagedoor") || !strcmp(d->vcls, "garagedoor");
}

int homey_switchable(const hy_dev_t * d) {
    if (homey_read_only(d) || d->kind == HY_K_COVER) return 0;
    const hy_cap_t * c = hy_dev_cap(d, "onoff");
    return c && c->setable;
}

int homey_start_flow(const char * flow_id) {
    if (!id_safe(flow_id)) return -1;
    int adv = -1;
    homey_lock();
    for (int i = 0; i < homey_state.nflow; i++)
        if (!strcmp(homey_state.flow[i].id, flow_id)) { adv = homey_state.flow[i].advanced; break; }
    homey_unlock();
    if (adv < 0) return -1;            /* only flows that can be started by hand */
    act_t a;
    memset(&a, 0, sizeof a);
    a.type = ACT_FLOW;
    a.advanced = adv;
    snprintf(a.id, sizeof a.id, "%s", flow_id);
    snprintf(a.json, sizeof a.json, "{}");
    return enqueue(&a);
}
