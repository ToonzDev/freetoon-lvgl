/*
 * Pushes samples into hcb_rrd's setRrdData HTTP endpoint.
 * Format mirrors what the TSC toonWater plugin uses:
 *   GET /hcb_rrd?action=setRrdData&loggerName=X&rra=Y&samples=URLEnc{"TS":VAL}
 */
#include "rrd_push.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rrd_push(const char * logger_name, const char * rra,
             long ts, double value) {
    if (!logger_name || !rra) return -1;
    /* JSON body: {"<ts>":<value>}. Build then URL-encode the special chars. */
    char json[96];
    snprintf(json, sizeof(json), "{\"%ld\":%.3f}", ts, value);

    char enc[256];
    char * dst = enc;
    for (const char * p = json; *p && dst < enc + sizeof(enc) - 4; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= '0' && c <= '9') || c == '.' || c == '-') {
            *dst++ = (char)c;
        } else if (c == '{') { memcpy(dst, "%7B", 3); dst += 3; }
        else if (c == '}')   { memcpy(dst, "%7D", 3); dst += 3; }
        else if (c == '"')   { memcpy(dst, "%22", 3); dst += 3; }
        else if (c == ':')   { memcpy(dst, "%3A", 3); dst += 3; }
        else                 { *dst++ = '_'; }
    }
    *dst = 0;

    char cmd[640];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s --max-time 3 "
        "'http://localhost:10080/hcb_rrd?action=setRrdData&loggerName=%s&rra=%s&samples=%s' >/dev/null 2>&1",
        logger_name, rra, enc);
    int rc = system(cmd);
    return (rc == 0) ? 0 : -1;
}

/* ---- counter loggers (HomeWizard Energy Sockets, #48) ---- */
#include "boxtalk.h"

#ifndef RRD_CFG_PATH        /* overridable for the host test */
#define RRD_CFG_PATH "/mnt/data/qmf/config/config_hcb_rrd.xml"
#endif
static const char * const rrd_cfg_paths[] = { RRD_CFG_PATH, "/qmf/config/config_hcb_rrd.xml" };

int rrd_logger_exists(const char * name) {
    const char * const * paths = rrd_cfg_paths;
    char want[96];
    snprintf(want, sizeof want, "<internalAddress>%s</internalAddress>", name);
    for (size_t p = 0; p < sizeof rrd_cfg_paths / sizeof rrd_cfg_paths[0]; p++) {
        FILE * f = fopen(paths[p], "r");
        if (!f) continue;
        char line[512];
        int found = 0;
        while (!found && fgets(line, sizeof line, f))
            if (strstr(line, want)) found = 1;
        fclose(f);
        return found;            /* first readable config decides */
    }
    return 0;
}

/* Same block happ_smartplug sends for a Z-Wave plug's kWh counter (RE of
 * rrd_appendConfigFromDev), minus the device: deviceVar/Svc/Uuid empty, so
 * hcb_rrd does not sample anything and only takes setRrdData. */
int rrd_create_counter_logger(const char * name) {
    char xml[1024];
    snprintf(xml, sizeof xml,
        "<rrdLogger><package>hcb_rrd</package><name>%s</name><internalAddress>%s</internalAddress>"
        "<visibility>0</visibility><deviceVar></deviceVar><deviceSvc></deviceSvc><deviceUuid></deviceUuid>"
        "<sampleType>integer</sampleType>"
        "<rra><binLength>3600</binLength><bins>744</bins><consolidator>last</consolidator>"
        "<name>1monthhour</name><minSamplesPerBin>1</minSamplesPerBin></rra>"
        "<rra><binLength>86400</binLength><bins>365</bins><consolidator>last</consolidator>"
        "<name>1yearday</name><minSamplesPerBin>1</minSamplesPerBin></rra></rrdLogger>",
        name, name);
    return boxtalk_set_object_config(xml);
}

/* ---- stock meter loggers vs. a P1 energy source ----
 * gas_quantity, gas_flow, elec_quantity_nt/_lt and elec_flow are stock
 * loggers: hcb_rrd samples a meter-adapter device into them (the device is
 * picked by happ_pwrusage). With a HomeWizard/ESPHome P1 as energy source
 * FreeToon pushes the P1 readings into the same loggers, and a removed or
 * stale adapter's counter got mixed in — "gas vandaag" took the old counter
 * as its 00:00 value, the graphs got spikes. So with a P1 source the loggers
 * are pointed at "placeholder" (what happ_pwrusage itself sets for an unused
 * meter); switching back to the meter adapter restores the saved device.
 *
 * Verified on a Toon 2 (2026-11-18): SetObjectConfig with the existing
 * <rrdLogger> block (same <uuid>, only <deviceUuid> changed) updates it in
 * place — same .rra/.dat files, history kept, survives an hcb_rrd restart,
 * setRrdData keeps working. */
#include "settings.h"
#include <pthread.h>
#include <unistd.h>

#ifndef RRD_SAVED_DEV_PATH
#define RRD_SAVED_DEV_PATH "/mnt/data/freetoon_rrd_meterdev.cfg"
#endif
static const char * const meter_loggers[] = {
    "gas_quantity", "gas_flow", "elec_quantity_nt", "elec_quantity_lt", "elec_flow" };
#define N_METER_LOGGERS (sizeof meter_loggers / sizeof meter_loggers[0])

static char * read_file(const char * path) {
    FILE * f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 32 * 1024, n = 0;
    char * b = malloc(cap);
    while (b) {
        if (n + 1 >= cap) {
            if (cap >= 1024 * 1024) break;          /* config is ~25 KB */
            char * nb = realloc(b, cap * 2);
            if (!nb) break;
            b = nb; cap *= 2;
        }
        size_t r = fread(b + n, 1, cap - 1 - n, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);
    if (b) b[n] = 0;
    return b;
}

/* The <rrdLogger> block whose <internalAddress> is `name`, copied into out
 * with CR/LF dropped (as sent in the test). 0 = found. */
static int logger_block(const char * cfg, const char * name, char * out, size_t outsz) {
    char want[96];
    snprintf(want, sizeof want, "<internalAddress>%s</internalAddress>", name);
    const char * hit = strstr(cfg, want);
    if (!hit) return -1;
    const char * start = NULL;
    for (const char * p = cfg; (p = strstr(p, "<rrdLogger>")) && p < hit; p++) start = p;
    const char * end = strstr(hit, "</rrdLogger>");
    if (!start || !end) return -1;
    end += strlen("</rrdLogger>");
    size_t o = 0;
    for (const char * p = start; p < end; p++) {
        if (*p == '\r' || *p == '\n') continue;
        if (o + 1 >= outsz) return -1;
        out[o++] = *p;
    }
    out[o] = 0;
    return 0;
}

static int block_device(const char * blk, char * dev, size_t devsz) {
    const char * p = strstr(blk, "<deviceUuid>");
    if (!p) return -1;
    p += strlen("<deviceUuid>");
    const char * e = strchr(p, '<');
    if (!e || (size_t)(e - p) >= devsz) return -1;
    snprintf(dev, devsz, "%.*s", (int)(e - p), p);
    return 0;
}

/* hcb_rrd's own uuid of a logger (its .dat/.rra file names), 0 = found.
 * Restricted to uuid characters: it is used in a cp command line. */
int rrd_logger_uuid(const char * name, char * out, size_t outsz) {
    char * cfg = NULL;
    for (size_t i = 0; !cfg && i < sizeof rrd_cfg_paths / sizeof rrd_cfg_paths[0]; i++)
        cfg = read_file(rrd_cfg_paths[i]);
    if (!cfg) return -1;
    static char blk[4096];
    static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    int rc = -1;
    pthread_mutex_lock(&mtx);
    if (logger_block(cfg, name, blk, sizeof blk) == 0) {
        const char * p = strstr(blk, "<uuid>");
        const char * e = p ? strstr(p, "</uuid>") : NULL;
        if (e) {
            p += strlen("<uuid>");
            size_t n = (size_t)(e - p);
            int ok = n > 0 && n < outsz;
            for (const char * q = p; ok && q < e; q++)
                if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') || *q == '-')) ok = 0;
            if (ok) { snprintf(out, outsz, "%.*s", (int)n, p); rc = 0; }
        }
    }
    pthread_mutex_unlock(&mtx);
    free(cfg);
    return rc;
}

/* Send blk with its <deviceUuid> replaced by dev. */
static int send_with_device(const char * blk, const char * dev) {
    const char * p = strstr(blk, "<deviceUuid>");
    const char * e = p ? strstr(p, "</deviceUuid>") : NULL;
    if (!e) return -1;
    p += strlen("<deviceUuid>");
    size_t sz = strlen(blk) + strlen(dev) + 1;
    char * x = malloc(sz);
    if (!x) return -1;
    snprintf(x, sz, "%.*s%s%s", (int)(p - blk), blk, dev, e);
    int rc = boxtalk_set_object_config(x);
    free(x);
    return rc;
}

/* Saved original devices: one "name uuid" per line. */
static void saved_load(char dev[N_METER_LOGGERS][64]) {
    for (size_t i = 0; i < N_METER_LOGGERS; i++) dev[i][0] = 0;
    FILE * f = fopen(RRD_SAVED_DEV_PATH, "r");
    if (!f) return;
    char n[64], u[64];
    while (fscanf(f, "%63s %63s", n, u) == 2)
        for (size_t i = 0; i < N_METER_LOGGERS; i++)
            if (!strcmp(n, meter_loggers[i])) snprintf(dev[i], 64, "%s", u);
    fclose(f);
}

static void saved_store(char dev[N_METER_LOGGERS][64]) {
    int any = 0;
    for (size_t i = 0; i < N_METER_LOGGERS; i++) if (dev[i][0]) any = 1;
    if (!any) { unlink(RRD_SAVED_DEV_PATH); return; }
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", RRD_SAVED_DEV_PATH);
    FILE * f = fopen(tmp, "w");
    if (!f) return;
    for (size_t i = 0; i < N_METER_LOGGERS; i++)
        if (dev[i][0]) fprintf(f, "%s %s\n", meter_loggers[i], dev[i]);
    if (fclose(f) == 0) rename(tmp, RRD_SAVED_DEV_PATH);
}

/* A device uuid is spliced into config XML: allow uuid characters only. */
static int dev_ok(const char * d) {
    if (!d[0]) return 0;
    for (const char * p = d; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') || *p == '-')) return 0;
    return 1;
}

/* ---- generic logger helpers (solar_rrd.c) ---- */
static pthread_mutex_t g_blk_mtx = PTHREAD_MUTEX_INITIALIZER;

int rrd_logger_device(const char * name, char * dev, size_t devsz) {
    char * cfg = NULL;
    for (size_t i = 0; !cfg && i < sizeof rrd_cfg_paths / sizeof rrd_cfg_paths[0]; i++)
        cfg = read_file(rrd_cfg_paths[i]);
    if (!cfg) return -2;
    static char blk[4096];
    pthread_mutex_lock(&g_blk_mtx);
    int rc = -1;
    if (logger_block(cfg, name, blk, sizeof blk) == 0) {
        if (block_device(blk, dev, devsz) != 0 && devsz) dev[0] = 0;
        rc = 0;
    }
    pthread_mutex_unlock(&g_blk_mtx);
    free(cfg);
    return rc;
}

static int dev_ok(const char * d);
int rrd_logger_set_device(const char * name, const char * dev) {
    if (!dev_ok(dev)) return -1;
    char * cfg = NULL;
    for (size_t i = 0; !cfg && i < sizeof rrd_cfg_paths / sizeof rrd_cfg_paths[0]; i++)
        cfg = read_file(rrd_cfg_paths[i]);
    if (!cfg) return -1;
    static char blk[4096];
    pthread_mutex_lock(&g_blk_mtx);
    int rc = logger_block(cfg, name, blk, sizeof blk) == 0 ? send_with_device(blk, dev) : -1;
    pthread_mutex_unlock(&g_blk_mtx);
    free(cfg);
    return rc;
}

#include "http.h"
int rrd_last_value(const char * name, const char * rra, long from, long to, double * out) {
    static char body[24 * 1024];
    static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    char url[256];
    snprintf(url, sizeof url,
             "http://127.0.0.1:10080/hcb_rrd?action=getRrdData&loggerName=%s&rra=%s&from=%ld&to=%ld&nullForNaN=1",
             name, rra, from, to);
    pthread_mutex_lock(&mtx);
    int rc = -1;
    if (http_fetch_to(url, body, sizeof body, 8) == 0 && body[0] == '{') {
        /* {"<ts>":<value>,"<ts>":null,...} */
        long best_ts = -1;
        for (const char * p = body; (p = strchr(p, '"')) != NULL; ) {
            char * e;
            long ts = strtol(p + 1, &e, 10);
            if (e == p + 1 || *e != '"') { p++; continue; }
            const char * v = e + 1;
            while (*v == ':' || *v == ' ') v++;
            if (*v != 'n' && *v != 'N') {
                char * ve;
                double d = strtod(v, &ve);
                if (ve != v && ts > best_ts) { best_ts = ts; *out = d; rc = 0; }
            }
            p = v;
        }
    }
    pthread_mutex_unlock(&mtx);
    return rc;
}

static void meter_loggers_sync_locked(void);
void rrd_meter_loggers_sync(void) {
    /* the sync thread and the history repair (rrd_repair.c) both call this */
    static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mtx);
    meter_loggers_sync_locked();
    pthread_mutex_unlock(&mtx);
}

static void meter_loggers_sync_locked(void) {
    if (settings.client_mode) return;
    int p1 = settings.energy_source == 1 || settings.energy_source == 2;
    char * cfg = NULL;
    for (size_t i = 0; !cfg && i < sizeof rrd_cfg_paths / sizeof rrd_cfg_paths[0]; i++)
        cfg = read_file(rrd_cfg_paths[i]);
    if (!cfg) return;

    char saved[N_METER_LOGGERS][64];
    saved_load(saved);
    int changed = 0;
    static char blk[4096];   /* guarded by rrd_meter_loggers_sync's mutex */
    for (size_t i = 0; i < N_METER_LOGGERS; i++) {
        char dev[64];
        if (logger_block(cfg, meter_loggers[i], blk, sizeof blk) != 0 ||
            block_device(blk, dev, sizeof dev) != 0) continue;
        int is_ph = !strcmp(dev, "placeholder");
        if (p1 && !is_ph && dev_ok(dev)) {
            /* Remember the adapter device first, then switch it off. A
             * different device than saved = happ_pwrusage set up a new one:
             * that is the one to restore later. */
            if (strcmp(saved[i], dev)) {
                snprintf(saved[i], sizeof saved[i], "%s", dev);
                saved_store(saved);
            }
            if (send_with_device(blk, "placeholder") == 0) {
                fprintf(stderr, "[rrd] %s: P1 energy source, stop sampling meter adapter "
                                "device %s\n", meter_loggers[i], dev);
                changed = 1;
            }
        } else if (!p1 && is_ph && dev_ok(saved[i])) {
            if (send_with_device(blk, saved[i]) == 0) {
                fprintf(stderr, "[rrd] %s: meter adapter is energy source again, sample "
                                "device %s\n", meter_loggers[i], saved[i]);
                saved[i][0] = 0;
                changed = 1;
            }
        } else if (!p1 && saved[i][0]) {
            saved[i][0] = 0;        /* happ_pwrusage set a device itself */
            changed = 1;
        }
    }
    if (changed) saved_store(saved);
    free(cfg);
}

static void * meter_loggers_thread(void * arg) {
    (void)arg;
    sleep(90);                  /* BoxTalk / hcb_config up */
    for (;;) {
        rrd_meter_loggers_sync();
        sleep(600);             /* also undoes happ_pwrusage re-pointing them */
    }
    return NULL;
}

int rrd_meter_loggers_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, meter_loggers_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
