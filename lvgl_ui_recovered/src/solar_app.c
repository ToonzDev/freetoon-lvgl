/* Solar production from a marketplace app - see solar_app.h. */
#include "solar_app.h"
#include "minijson.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static solar_app_state_t g_st;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

#define SOLAR_APP_POST_STALE_S  600
#define SOLAR_APP_OK_KEEP_S     (24 * 3600)
#define SOLAR_APP_POWER_FRESH_S 300

int solar_app_post(const char * json) {
    solar_app_state_t n;
    memset(&n, 0, sizeof n);
    double v;
    if (!mj_str(json, "app", n.app, sizeof n.app) || !n.app[0]) return -1;
    mj_str(json, "brand", n.brand, sizeof n.brand);
    n.ok = mj_bool(json, "ok");
    n.power_w  = mj_num(json, "power_w", &v)  && v >= 0 && v < 1e6  ? v : 0;
    n.today_wh = mj_num(json, "today_wh", &v) && v >= 0 && v < 1e7  ? v : -1;
    n.total_wh = mj_num(json, "total_wh", &v) && v >= 0 && v < 1e12 ? v : -1;
    mj_str(json, "msg", n.msg, sizeof n.msg);
    for (char * c = n.msg; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';
    time_t now = time(NULL);

    pthread_mutex_lock(&g_mu);
    int first = !g_st.last_ok && n.ok;
    int changed = strcmp(g_st.app, n.app) || strcmp(g_st.brand, n.brand) ||
                  g_st.ok != n.ok || (!n.ok && strcmp(g_st.msg, n.msg));
    n.last_post = now;
    n.last_ok = n.ok ? now : (strcmp(g_st.brand, n.brand) ? 0 : g_st.last_ok);
    if (!n.ok) {          /* keep the last reading's totals (same inverter) for the page */
        n.today_wh = n.last_ok ? g_st.today_wh : -1;
        n.total_wh = n.last_ok ? g_st.total_wh : -1;
        n.power_w = 0;
    }
    g_st = n;
    pthread_mutex_unlock(&g_mu);

    if (first || changed) {
        if (n.ok) fprintf(stderr, "[solar] %s/%s: %.0f W, today %.0f Wh, total %.0f Wh\n",
                          n.app, n.brand[0] ? n.brand : "-", n.power_w, n.today_wh, n.total_wh);
        else fprintf(stderr, "[solar] %s/%s: no reading: %s\n", n.app, n.brand[0] ? n.brand : "-", n.msg);
    }
    return 0;
}

void solar_app_get(solar_app_state_t * out) {
    pthread_mutex_lock(&g_mu);
    *out = g_st;
    pthread_mutex_unlock(&g_mu);
}

int solar_app_active(void) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_mu);
    int a = g_st.brand[0] && g_st.last_post && now - g_st.last_post < SOLAR_APP_POST_STALE_S &&
            g_st.last_ok && now - g_st.last_ok < SOLAR_APP_OK_KEEP_S;
    pthread_mutex_unlock(&g_mu);
    return a;
}

float solar_app_power_w(void) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_mu);
    float w = (g_st.ok && now - g_st.last_ok < SOLAR_APP_POWER_FRESH_S) ? (float)g_st.power_w : 0.0f;
    pthread_mutex_unlock(&g_mu);
    return w;
}
