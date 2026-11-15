/*
 * Z-Wave smart plugs for Statistics — see plugs.h.
 */
#include "plugs.h"
#include "stats.h"
#include "settings.h"
#include "homewizard.h"
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
    if (!u) {
        const char * colon = strrchr(p->dev_uuid, ':');
        if (colon) {
            snprintf(key, sizeof key, "\"%s\"", colon + 1);
            u = strstr(json, key);
        }
    }
    if (!u) return;
    const char * next_dev = strstr(u + 1, "\"dev_");
    const char * end = next_dev ? next_dev : u + strlen(u);

    const char * s = strstr(u, "\"DeviceName\":");
    if (!s || s > end) s = strstr(u, "\"name\":");
    if (s && s < end) {
        s += (*s == '\"' && s[1] == 'D') ? 13 : 7;
        while (*s == ' ' || *s == '\"') s++;
        size_t n = strcspn(s, "\",\r\n}");
        if (n >= sizeof p->name) n = sizeof p->name - 1;
        memcpy(p->name, s, n);
        p->name[n] = 0;
    }
    const char * w = strstr(u, "\"CurrentElectricityFlow\"");
    if (w && w < end) {
        w += strlen("\"CurrentElectricityFlow\"");
        while (*w && (*w == ' ' || *w == ':' || *w == '\"')) w++;
        if (strncmp(w, "NaN", 3) != 0 && strncmp(w, "null", 4) != 0) {
            p->power_w = (float)atof(w);
            p->have_power = 1;
        }
    }
}

/* A name given in FreeToon's Z-Wave screen (toonui_zwave_names.cfg,
 * "<zw-uuid>=<name>") wins over the Z-Wave device name, which for a plug
 * paired through FreeToon is just its type ("NAS_WR01Z"). */
static void custom_name(plug_t * p) {
    const char * d = getenv("TOONUI_DATA_DIR");
    char path[256];
    snprintf(path, sizeof path, "%s/toonui_zwave_names.cfg", (d && *d) ? d : "/mnt/data");
    FILE * f = fopen(path, "r");
    if (!f) return;
    char line[160];
    size_t ul = strlen(p->dev_uuid);
    while (ul && fgets(line, sizeof line, f)) {
        if (strncmp(line, p->dev_uuid, ul) != 0 || line[ul] != '=') continue;
        char * v = line + ul + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (*v) snprintf(p->name, sizeof p->name, "%s", v);
        break;
    }
    fclose(f);
}

void plugs_refresh_power(plug_t * p, int n) {
    if (n <= 0) return;
    static char body[64 * 1024];
    int any_zw = 0;
    for (int i = 0; i < n; i++) if (p[i].hw_slot == -1) any_zw = 1;
    if (any_zw && stats_http_get("hdrv_zwave?action=getDevices.json", body, sizeof body) == 0)
        for (int i = 0; i < n; i++) if (p[i].hw_slot == -1) zwave_info(body, &p[i]);
    for (int i = 0; i < n; i++) {
        if (p[i].hw_slot == -1) { custom_name(&p[i]); continue; }
        int s = p[i].hw_slot;                  /* HomeWizard socket */
        if (s < 0 || s >= HW_SOCKET_MAX) continue;
        if (settings.hw_socket_name[s][0]) snprintf(p[i].name, sizeof p[i].name, "%s", settings.hw_socket_name[s]);
        p[i].have_power = hw_sockets[s].connected;
        p[i].power_w = hw_sockets[s].power_w;
    }
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
            pl->hw_slot = -1;
            snprintf(pl->name, sizeof pl->name, "Stekker %d", n + 1);
            n++;
        }
        plugs_refresh_power(out, n);
        return n;
    }
    const char * env = getenv("TOONUI_RRD_XML");     /* sim / tests */
    if (settings.client_mode && !env) return 0;      /* the plugs live on the master */
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
         * loggers use 5yrhours/10yrdays instead) — Z-Wave plugs (made by
         * happ_smartplug) or HomeWizard sockets (made by FreeToon, no device) */
        char ia[64] = "";
        xml_elem(a, b, "internalAddress", ia, sizeof ia);
        int is_hw = !strncmp(ia, "hw_socket_qtty_", 15);
        if (is_hw || (!strcmp(svc, "ElectricityQuantityMeter") && !strcmp(var, "CurrentElectricityQuantity"))) {
            const char * r = strstr(a, "<name>1monthhour</name>");
            if (r && r < b) {
                plug_t * pl = &out[n];
                memset(pl, 0, sizeof *pl);
                /* Logger name: hcb_config may write the <rra> children (each
                 * with its own <name>) BEFORE the logger's <name>, so use
                 * <internalAddress> (= the name for these loggers), else the
                 * first <name> that is not inside an <rra>. */
                if (!xml_elem(a, b, "internalAddress", pl->logger, sizeof pl->logger)) {
                    const char * s = a;
                    while ((s = strstr(s, "<name>")) && s < b) {
                        const char * ro = a, * last_open = NULL, * last_close = NULL;
                        while ((ro = strstr(ro, "<rra>")) && ro < s) { last_open = ro; ro += 5; }
                        ro = a;
                        while ((ro = strstr(ro, "</rra>")) && ro < s) { last_close = ro; ro += 6; }
                        if (!last_open || (last_close && last_close > last_open)) {
                            xml_elem(s, b, "name", pl->logger, sizeof pl->logger);
                            break;
                        }
                        s += 6;
                    }
                }
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
                pl->hw_slot = -1;
                if (is_hw) {                    /* which configured socket is it? */
                    pl->hw_slot = -2;           /* socket no longer set up */
                    for (int i = 0; i < HW_SOCKET_MAX; i++) {
                        char nm[64];
                        if (homewizard_socket_logger(i, nm, sizeof nm) && !strcmp(nm, pl->logger)) { pl->hw_slot = i; break; }
                    }
                }
                if (pl->logger[0]) {
                    snprintf(pl->name, sizeof pl->name, is_hw ? "HomeWizard %d" : "Stekker %d", n + 1);
                    n++;
                }
            }
        }
        p = b + 12;
    }
    free(xml);

    if (n == 0) {
        /* Fallback: discover smart plugs directly from hdrv_zwave?action=getDevices.json
         * so live power is available even before hcb_rrd creates RRD loggers. */
        static char d_body[64 * 1024];
        if (stats_http_get("hdrv_zwave?action=getDevices.json", d_body, sizeof d_body) == 0) {
            const char * scan_p = d_body;
            while (n < max && (scan_p = strstr(scan_p, "\"dev_"))) {
                const char * next = strstr(scan_p + 5, "\"dev_");
                const char * dend = next ? next : scan_p + strlen(scan_p);
                /* Check if switch or plug */
                const char * cc = strstr(scan_p, "\"ccList\":");
                if (!cc || cc > dend) cc = strstr(scan_p, "\"supportedCC\":");
                int is_sw = 0;
                if (cc && cc < dend && (strstr(cc, "25") || strstr(cc, "37") || strstr(cc, "0x25"))) is_sw = 1;
                const char * typ = strstr(scan_p, "\"type\":");
                if (typ && typ < dend && (strstr(typ, "NAS_") || strstr(typ, "PLUG") || strstr(typ, "SWITCH") || strstr(typ, "FGWP"))) is_sw = 1;

                if (is_sw) {
                    plug_t * pl = &out[n];
                    memset(pl, 0, sizeof *pl);
                    pl->hw_slot = -1;
                    /* extract dev_uuid */
                    const char * u = strstr(scan_p, "\"uuid\":");
                    if (u && u < dend) {
                        u += 7;
                        while (*u == ' ' || *u == '\"') u++;
                        size_t ul = strcspn(u, "\",\r\n}");
                        if (ul >= sizeof pl->dev_uuid) ul = sizeof pl->dev_uuid - 1;
                        memcpy(pl->dev_uuid, u, ul);
                        pl->dev_uuid[ul] = 0;
                    }
                    /* internalAddress / node ID for logger */
                    const char * ia = strstr(scan_p, "\"internalAddress\":");
                    if (ia && ia < dend) {
                        ia += 18;
                        while (*ia == ' ' || *ia == '\"') ia++;
                        int node_id = atoi(ia);
                        snprintf(pl->logger, sizeof pl->logger, "smartplug_%d", node_id);
                        snprintf(pl->logger_q, sizeof pl->logger_q, "smartplug_%d", node_id);
                    }
                    snprintf(pl->name, sizeof pl->name, "Stekker %d", n + 1);
                    zwave_info(scan_p, pl);
                    custom_name(pl);
                    n++;
                }
                scan_p = next ? next : dend;
            }
        }
    }

    plugs_refresh_power(out, n);
    fprintf(stderr, "[plugs] %d smart plug logger(s) found\n", n);
    return n;
}
