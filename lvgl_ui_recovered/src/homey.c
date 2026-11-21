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
void homey_kick(void) { g_kick = 1; }

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
    snprintf(hp, sizeof hp, "%s", h);
    hp[strcspn(hp, "/")] = 0;
    if (!hp[0] || !sh_is_hostport(hp)) return -1;
    snprintf(out, n, "%s://%s", *tls ? "https" : "http", hp);
    return 0;
}

/* GET <base>/api/<path> with the bearer key. On success returns the HTTP
 * status (200, 401, ...) and *body (heap, NUL-terminated, caller frees) with
 * *len; returns 0 if the host could not be reached. */
static int homey_get(const char * path, char ** body, size_t * len) {
    *body = NULL; *len = 0;
    char base[128];
    int tls;
    if (!g_key[0] || base_url(base, sizeof base, &tls) != 0) return 0;

    char url[256], qurl[300], hdr[200], qhdr[240], cmd[700];
    snprintf(url, sizeof url, "%s/api/%s", base, path);
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s", g_key);
    if (!sh_quote(qurl, sizeof qurl, url) || !sh_quote(qhdr, sizeof qhdr, hdr)) return 0;
    /* -k for https: a Homey on the LAN has no publicly trusted certificate.
     * The status code is appended on its own last line. */
    snprintf(cmd, sizeof cmd,
             "/usr/bin/curl -s -4 %s--compressed --max-time 20 --connect-timeout 4 "
             "-H %s -w '\\n%%{http_code}' %s 2>/dev/null",
             tls ? "-k " : "", qhdr, qurl);

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
    char * body; size_t len;
    int code = homey_get("manager/devices/device/", &body, &len);
    if (code == 0) return HOMEY_ST_NOCONN;
    if (code == 401 || code == 403) { free(body); return HOMEY_ST_AUTH; }
    int n = code == 200 ? hy_parse_devices(body, len, g_dev, HY_MAX_DEV) : -1;
    free(body);
    if (n < 0) return HOMEY_ST_BADDATA;
    homey_lock();
    memcpy(homey_state.dev, g_dev, sizeof(hy_dev_t) * (size_t)n);
    homey_state.ndev = n;
    homey_state.gen++;
    homey_state.last_ok = (long)time(NULL);
    homey_unlock();
    if (count_out) *count_out = n;
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
        int st = poll_devices(&n);
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
        for (int i = 0; i < wait && !g_kick; i++) sleep(1);
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
    int n = 0;
    int st = poll_devices(&n);
    return st == HOMEY_ST_OK ? n : -st;
}
