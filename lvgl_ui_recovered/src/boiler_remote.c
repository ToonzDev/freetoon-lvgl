/* Boiler on another Toon: see boiler_remote.h. */
#include "boiler_remote.h"
#include "settings.h"
#include "http.h"
#include "minijson.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define TICK_S        5
#define LOCAL_EVERY   10      /* s: read the own heat demand */
#define REFRESH_S     60      /* s: re-send the boiler Toon's setpoint */
#define UNKNOWN_OFF_S 300     /* own happ silent this long: send "off" */
#define HB_SILENCE_S  900     /* boiler side: no heartbeat this long -> safe setpoint */
#define HB_PORT       10081

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static boiler_remote_status_t g_st = { .demand = -1, .remote_temp = -99, .remote_burner = -1 };
static int g_hb_safe = 600;

int boiler_remote_host_ok(const char * h) {
    if (!h || !h[0] || strlen(h) > 63) return 0;
    for (const char * p = h; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              *p == '.' || *p == '-' || *p == ':')) return 0;
    return 1;
}

void boiler_remote_status(boiler_remote_status_t * out) {
    pthread_mutex_lock(&g_mtx);
    *out = g_st;
    pthread_mutex_unlock(&g_mtx);
}

static const char * local_base(void) {
    const char * e = getenv("BOILER_REMOTE_LOCAL");   /* tests: a mock happ */
    return e && e[0] ? e : "http://127.0.0.1";
}

/* Own heat demand from happ_thermstat: 1 heat, 0 none, -1 unknown. */
static int own_demand(void) {
    char url[128], body[1024] = "";
    snprintf(url, sizeof url, "%s/happ_thermstat?action=getThermostatInfo", local_base());
    if (http_fetch_to(url, body, sizeof body, 5) != 0) return -1;
    double bi;
    if (!mj_num(body, "burnerInfo", &bi)) return -1;
    return (bi == 1 || bi == 3) ? 1 : 0;
}

static int happ_get(const char * host, const char * query, char * out, size_t n) {
    char url[200];
    snprintf(url, sizeof url, "http://%s/happ_thermstat?action=%s", host, query);
    out[0] = 0;
    return http_fetch_to(url, out, n, 6);
}

/* POST the heartbeat to FreeToon on the boiler Toon (IPv4 only). 1 = answered 200. */
static int post_heartbeat(const char * host, int on, int safe) {
    char ip[64];
    snprintf(ip, sizeof ip, "%s", host);
    char * c = strchr(ip, ':');
    if (c) *c = 0;                                  /* host:port (tests): the port is happ's */
    int port = HB_PORT;
    const char * e = getenv("BOILER_REMOTE_HBPORT");
    if (e && atoi(e) > 0) port = atoi(e);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return 0;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    struct timeval tv = { 4, 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int ok = 0;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        char body[64], req[320];
        int bl = snprintf(body, sizeof body, "{\"on\":%d,\"safe\":%d}", on ? 1 : 0, safe);
        int n = snprintf(req, sizeof req,
            "POST /api/v1/boiler/remote HTTP/1.0\r\nHost: %s:%d\r\nContent-Type: application/json\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n%s", ip, port, bl, body);
        if (n > 0 && n < (int)sizeof req && send(s, req, (size_t)n, 0) == n) {
            char resp[64];
            ssize_t r = recv(s, resp, sizeof resp - 1, 0);
            if (r > 12) { resp[r] = 0; ok = strstr(resp, " 200 ") != NULL; }
        }
    }
    close(s);
    return ok;
}

static void set_error(const char * msg) {
    pthread_mutex_lock(&g_mtx);
    snprintf(g_st.error, sizeof g_st.error, "%s", msg);
    pthread_mutex_unlock(&g_mtx);
}

/* Send the wanted setpoint to the boiler Toon, its programme off; read back
 * its state for the status. */
static void send_remote(const char * host, int on) {
    int sp = on ? settings.boiler_remote_on : settings.boiler_remote_off;
    char q[64], body[1024];
    snprintf(q, sizeof q, "setSetpoint&Setpoint=%d", sp);
    int ok = happ_get(host, q, body, sizeof body) == 0 && strstr(body, "ok");
    double ps = -1, ct = -9900, bi = -1, cs = -1;
    if (ok && happ_get(host, "getThermostatInfo", body, sizeof body) == 0) {
        mj_num(body, "programState", &ps);
        mj_num(body, "currentTemp", &ct);
        mj_num(body, "burnerInfo", &bi);
        mj_num(body, "currentSetpoint", &cs);
        if (ps > 0) happ_get(host, "changeSchemeState&state=0", body, sizeof body);
    }
    int hb = post_heartbeat(host, on, settings.boiler_remote_off);
    pthread_mutex_lock(&g_mtx);
    int was = g_st.reachable, prev_sp = g_st.sent_setpoint;
    g_st.reachable = ok;
    g_st.last_sent = time(NULL);
    g_st.sent_setpoint = sp;
    g_st.remote_temp = ct > -9900 ? ct / 100.0 : -99;
    g_st.remote_burner = (int)bi;
    g_st.remote_freetoon = hb;
    if (ok) g_st.error[0] = 0;
    else snprintf(g_st.error, sizeof g_st.error, "ketel-Toon %s niet bereikbaar", host);
    pthread_mutex_unlock(&g_mtx);
    if (ok != was || sp != prev_sp)
        fprintf(stderr, "[boiler-remote] %s -> %s: setpoint %.1f%s%s\n", ok ? "sent" : "FAILED", host,
                sp / 100.0, ps > 0 ? ", programme switched off" : "", hb ? ", safety net on" : "");
}

/* Boiler side: heartbeats stopped -> own happ on the safe setpoint, once. */
static void watchdog(time_t now) {
    pthread_mutex_lock(&g_mtx);
    const char * es = getenv("BOILER_REMOTE_HBSILENCE");   /* tests */
    long silence = es && atol(es) > 0 ? atol(es) : HB_SILENCE_S;
    int trip = g_st.hb_last && !g_st.hb_tripped && now - g_st.hb_last > silence;
    int safe = g_hb_safe;
    char from[48];
    snprintf(from, sizeof from, "%s", g_st.hb_from);
    if (trip) g_st.hb_tripped = 1;
    pthread_mutex_unlock(&g_mtx);
    if (!trip) return;
    char url[128], body[256] = "";
    snprintf(url, sizeof url, "%s/happ_thermstat?action=setSetpoint&Setpoint=%d", local_base(), safe);
    int rc = http_fetch_to(url, body, sizeof body, 5);
    fprintf(stderr, "[boiler-remote] no word from %s for %lds: safe setpoint %.1f (%s)\n",
            from, silence, safe / 100.0, rc == 0 && strstr(body, "ok") ? "ok" : "FAILED");
}

int boiler_remote_heartbeat(const char * peer_ip, const char * body) {
    double on = -1, safe = 600;
    if (!mj_num(body, "on", &on)) return -1;
    mj_num(body, "safe", &safe);
    if (safe < 600 || safe > 3000) safe = 600;
    pthread_mutex_lock(&g_mtx);
    if (g_st.hb_tripped || !g_st.hb_last)
        fprintf(stderr, "[boiler-remote] heartbeat from %s: this Toon's boiler is driven remotely "
                        "(safety net: %.1f after %d min silence)\n", peer_ip, safe / 100.0, HB_SILENCE_S / 60);
    g_st.hb_last = time(NULL);
    g_st.hb_tripped = 0;
    g_hb_safe = (int)safe;
    snprintf(g_st.hb_from, sizeof g_st.hb_from, "%s", peer_ip);
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

static void * thread_fn(void * arg) {
    (void)arg;
    const char * ed = getenv("BOILER_REMOTE_DELAY");       /* tests */
    sleep(ed ? (unsigned)atoi(ed) : 30);           /* happ / network up */
    time_t last_local = 0, unknown_since = 0;
    int demand = -1, sent_demand = -1;
    char sent_host[64] = "";
    for (;;) {
        time_t now = time(NULL);
        watchdog(now);
        const char * host = settings.boiler_remote_host;
        int configured = host[0] && boiler_remote_host_ok(host);
        pthread_mutex_lock(&g_mtx);
        g_st.configured = configured;
        pthread_mutex_unlock(&g_mtx);
        if (!configured) {
            if (sent_host[0]) fprintf(stderr, "[boiler-remote] off (no boiler Toon set)\n");
            sent_host[0] = 0; sent_demand = -1; demand = -1;
            sleep(TICK_S);
            continue;
        }
        if (now - last_local >= LOCAL_EVERY) {
            last_local = now;
            int d = own_demand();
            if (d >= 0) { demand = d; unknown_since = 0; }
            else {
                if (!unknown_since) unknown_since = now;
                if (now - unknown_since > UNKNOWN_OFF_S && demand != 0) {
                    fprintf(stderr, "[boiler-remote] own thermostat silent for %d min: boiler off\n", UNKNOWN_OFF_S / 60);
                    demand = 0;
                }
                if (demand < 0) set_error("eigen thermostaat (happ) geeft geen antwoord");
            }
            pthread_mutex_lock(&g_mtx);
            g_st.demand = demand;
            pthread_mutex_unlock(&g_mtx);
        }
        if (demand >= 0) {
            time_t last_sent;
            pthread_mutex_lock(&g_mtx);
            last_sent = g_st.last_sent;
            pthread_mutex_unlock(&g_mtx);
            if (demand != sent_demand || strcmp(sent_host, host) || now - last_sent >= REFRESH_S) {
                if (demand != sent_demand)
                    fprintf(stderr, "[boiler-remote] heat demand %s\n", demand ? "on" : "off");
                send_remote(host, demand);
                sent_demand = demand;
                snprintf(sent_host, sizeof sent_host, "%s", host);
            }
        }
        sleep(TICK_S);
    }
    return NULL;
}

int boiler_remote_start(void) {
    static int started;
    if (started) return 0;
    started = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, thread_fn, NULL) != 0) { started = 0; return -1; }
    pthread_detach(t);
    return 0;
}
