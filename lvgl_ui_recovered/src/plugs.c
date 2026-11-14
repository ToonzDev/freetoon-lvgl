/*
 * Z-Wave smart plugs for Statistics — see plugs.h.
 */
#include "plugs.h"
#include "stats.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Text of the first <tag>...</tag> in [p, end). */
static int xml_elem(const char * p, const char * end, const char * tag, char * out, size_t sz) {
    char o[40], c[40];
    snprintf(o, sizeof o, "<%s>", tag);
    snprintf(c, sizeof c, "</%s>", tag);
    const char * a = strstr(p, o);
    if (!a || a >= end) return 0;
    a += strlen(o);
    const char * b = strstr(a, c);
    if (!b || b > end) return 0;
    size_t n = (size_t)(b - a);
    if (n >= sz) n = sz - 1;
    memcpy(out, a, n);
    out[n] = 0;
    return 1;
}

static char * read_file(const char * path) {
    FILE * f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 64 * 1024, n = 0;
    char * buf = malloc(cap);
    while (buf) {
        size_t r = fread(buf + n, 1, cap - 1 - n, f);
        n += r;
        if (r == 0) break;
        if (n + 1 >= cap) {
            if (cap >= 1024 * 1024) break;           /* sanity cap */
            char * nb = realloc(buf, cap * 2);
            if (!nb) break;
            buf = nb; cap *= 2;
        }
    }
    fclose(f);
    if (buf) buf[n] = 0;
    return buf;
}

/* name + CurrentElectricityFlow of one device from getDevices.json:
 * {"dev_10": {"uuid": "...", "name": "Tuin", ..., "CurrentElectricityFlow": "12.59", ...}, ...} */
static void zwave_info(const char * json, plug_t * p) {
    char key[64];
    snprintf(key, sizeof key, "\"%s\"", p->dev_uuid);
    const char * u = strstr(json, key);
    if (!u) return;
    const char * end = strchr(u, '}');               /* device objects are flat */
    if (!end) return;
    const char * s = strstr(u, "\"name\": \"");
    if (s && s < end) {
        s += 9;
        size_t n = strcspn(s, "\"");
        if (n >= sizeof p->name) n = sizeof p->name - 1;
        memcpy(p->name, s, n);
        p->name[n] = 0;
    }
    const char * w = strstr(u, "\"CurrentElectricityFlow\": \"");
    if (w && w < end) {
        w += 27;
        if (strncmp(w, "NaN", 3) != 0) { p->power_w = (float)atof(w); p->have_power = 1; }
    }
}

void plugs_refresh_power(plug_t * p, int n) {
    if (n <= 0) return;
    static char body[64 * 1024];
    if (stats_http_get("hdrv_zwave?action=getDevices.json", body, sizeof body) != 0) return;
    for (int i = 0; i < n; i++) zwave_info(body, &p[i]);
}

int plugs_scan(plug_t * out, int max) {
    /* Sim / tests: TOONUI_PLUG_LOGGERS="<logger-uuid>=<device-uuid>,..." —
     * plugs addressed by logger uuid (stats "@uuid"), e.g. against a real
     * Toon's hcb_rrd in client mode. */
    const char * sim = getenv("TOONUI_PLUG_LOGGERS");
    if (sim) {
        int n = 0;
        char buf[512];
        snprintf(buf, sizeof buf, "%s", sim);
        for (char * tok = strtok(buf, ","); tok && n < max; tok = strtok(NULL, ",")) {
            char * eq = strchr(tok, '=');
            plug_t * pl = &out[n];
            memset(pl, 0, sizeof *pl);
            if (eq) { *eq = 0; snprintf(pl->dev_uuid, sizeof pl->dev_uuid, "%s", eq + 1); }
            snprintf(pl->logger, sizeof pl->logger, "%s", tok);
            snprintf(pl->logger_q, sizeof pl->logger_q, "@%s", tok);
            snprintf(pl->name, sizeof pl->name, "Stekker %d", n + 1);
            n++;
        }
        plugs_refresh_power(out, n);
        return n;
    }
    if (settings.client_mode) return 0;              /* the plugs live on the master */
    const char * env = getenv("TOONUI_RRD_XML");     /* sim / tests */
    char * xml = env ? read_file(env) : read_file("/mnt/data/qmf/config/config_hcb_rrd.xml");
    if (!xml) xml = read_file("/qmf/config/config_hcb_rrd.xml");
    if (!xml) return 0;
    int n = 0;
    const char * p = xml;
    while (n < max) {
        const char * a = strstr(p, "<rrdLogger>");
        if (!a) break;
        const char * b = strstr(a, "</rrdLogger>");
        if (!b) break;
        char svc[48] = "", var[48] = "";
        xml_elem(a, b, "deviceSvc", svc, sizeof svc);
        xml_elem(a, b, "deviceVar", var, sizeof var);
        /* plug loggers: kWh counter with the hcb_rrd plug archives (the meter
         * loggers use 5yrhours/10yrdays instead) */
        if (!strcmp(svc, "ElectricityQuantityMeter") && !strcmp(var, "CurrentElectricityQuantity")) {
            const char * r = strstr(a, "<name>1monthhour</name>");
            if (r && r < b) {
                plug_t * pl = &out[n];
                memset(pl, 0, sizeof *pl);
                xml_elem(a, b, "name", pl->logger, sizeof pl->logger);   /* first <name> = logger */
                xml_elem(a, b, "deviceUuid", pl->dev_uuid, sizeof pl->dev_uuid);
                /* URL-encode for the getRrdData query string */
                size_t o = 0;
                for (const char * c = pl->logger; *c && o + 4 < sizeof pl->logger_q; c++) {
                    unsigned char ch = (unsigned char)*c;
                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                        ch == '-' || ch == '_' || ch == '.')
                        pl->logger_q[o++] = (char)ch;
                    else
                        o += (size_t)snprintf(pl->logger_q + o, sizeof pl->logger_q - o, "%%%02X", ch);
                }
                pl->logger_q[o] = 0;
                if (pl->logger[0]) {
                    snprintf(pl->name, sizeof pl->name, "Stekker %d", n + 1);
                    n++;
                }
            }
        }
        p = b + 12;
    }
    free(xml);
    plugs_refresh_power(out, n);
    fprintf(stderr, "[plugs] %d smart plug logger(s) found\n", n);
    return n;
}
