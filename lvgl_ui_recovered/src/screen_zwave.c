/*
 * Z-Wave management screen — Settings -> Z-Wave.
 *
 * Talks to the Toon's built-in Z-Wave controller (hdrv_zwave) over the
 * NATIVE BoxTalk bus via boxtalk.c — the same path qt-gui uses — not HTTP.
 *   GetDevices / NodeNeighborUpdate / IncludeDevice / ExcludeDevice /
 *   basicSet / SetDeviceName  on serviceId "specific1", destuuid
 *   qb-...:hdrv_zwave (see boxtalk_zwave_* + reference_toon_zwave_api memory).
 *
 * The device-list response (boxtalk_zwave_get_devices -> zwave_response_buf)
 * is XML: <u:GetDevicesResponse><devices> <device>... </device> ... </devices>.
 * We poll zwave_response_ready in the refresh timer and parse on the UI thread.
 * Fire-and-forget actions call boxtalk_zwave_* directly (send_msg is the same
 * lock-guarded path the thermostat +/- buttons already use from the UI thread).
 *
 * "Enable control" gates the write actions and is a pure toonui setting
 * (settings.enable_zwave) — we do NOT poke the stock supportControl flag or
 * restart the driver; reads work regardless and the API responds either way.
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "boxtalk.h"
#include "ma_admin.h"
#include "stats.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COL_BG       THEME_BG
#define COL_CARD     THEME_CARD_BG
#define COL_TEXT_HI  THEME_TEXT_HI
#define COL_TEXT_DIM THEME_TEXT_DIM
#define COL_OK       THEME_SUCCESS_BG
#define COL_WARN     THEME_DANGER_BG
#define COL_OFF      THEME_OFF

#include "screen_zwave.h"

/* Internal struct + cap now live in screen_zwave.h so the master↔slave
 * bridge can serialise/deserialise the controller device list. */
#define MAX_ZW_DEV ZWAVE_DEV_MAX
typedef zwave_dev_t zw_dev_t;

static lv_obj_t * scr_root      = NULL;
static lv_obj_t * lbl_ctrl      = NULL;
static lv_obj_t * lbl_status    = NULL;
static lv_obj_t * list_box      = NULL;
static lv_obj_t * sw_enable     = NULL;
static lv_obj_t * bar_actions   = NULL;
static lv_obj_t * btn_inc_lbl   = NULL;   /* "+ Add device" / "Stop" */
static lv_obj_t * btn_exc_lbl   = NULL;
static lv_timer_t * refresh_timer = NULL;
static void build_rows(void);

static zw_dev_t g_dev[MAX_ZW_DEV];
static int      g_dev_count = 0;
static int      g_inc_active = 0;
static int      g_exc_active = 0;
static char     g_status[192] = "";
static int      g_poll_ticks = 0;

/* Add / remove go through ma_admin (stock Administration IncludeDevice /
 * ExcludeDevice with a result), so we learn what was added. A new smart plug
 * is then adopted by happ_smartplug (EnableDevice), as the stock "add plug"
 * flow does — otherwise it gets no hcb_rrd loggers and no Stekkers history. */
static int    g_admin_seq0 = -1;
static char   g_plug_uuid[48] = "";   /* added device being checked for a plug */
static time_t g_plug_deadline = 0;
static int    g_plug_tick = 0;
static time_t g_admin_t0 = 0;        /* start of the running add/remove */

static void zw_inc(int start) {
    ma_admin_t a; ma_get_admin(&a); g_admin_seq0 = a.seq; g_admin_t0 = time(NULL);
    if (start) ma_include_start(); else ma_include_stop();
}
static void zw_exc(int start) {
    ma_admin_t a; ma_get_admin(&a); g_admin_seq0 = a.seq; g_admin_t0 = time(NULL);
    if (start) ma_exclude_start(); else ma_exclude_stop();
}

/* ===================================================================== */
/* XML / string helpers                                                  */
/* ===================================================================== */
static const char * ci_strstr(const char * hay, const char * needle) {
    if (!hay || !needle) return NULL;
    size_t nl = strlen(needle);
    if (!nl) return hay;
    while (*hay) {
        if (strncasecmp(hay, needle, nl) == 0) return hay;
        hay++;
    }
    return NULL;
}

/* Copy the text of <key>...</key> within [src,end) into out. Returns 1/0. */
static int xml_elem(const char * src, const char * end, const char * key,
                    char * out, size_t outsz) {
    char open[48];
    snprintf(open, sizeof open, "<%s>", key);
    const char * p = strstr(src, open);
    if (!p || p >= end) { if (out && outsz) out[0] = 0; return 0; }
    p += strlen(open);
    char close[48];
    snprintf(close, sizeof close, "</%s>", key);
    const char * q = strstr(p, close);
    if (!q || q > end) { if (out && outsz) out[0] = 0; return 0; }
    size_t n = (size_t)(q - p);
    if (n + 1 > outsz) n = outsz - 1;
    if (out) { memcpy(out, p, n); out[n] = 0; }
    return 1;
}

/* ===================================================================== */
/* Device classification & custom names                                  */
/* ===================================================================== */
int zwave_is_switch_device(const char * type, const char * name, const char * cc, int node_id) {
    if (node_id == 7) return 0;   /* Node 7 is the Toon Z-Wave meter adapter */
    if (type) {
        if (ci_strstr(type, "METER") ||
            ci_strstr(type, "HAE_") ||
            ci_strstr(type, "SMOKE") ||
            ci_strstr(type, "GAS") ||
            ci_strstr(type, "WATER") ||
            ci_strstr(type, "SENSOR"))
            return 0;
    }
    if (name) {
        if (ci_strstr(name, "METER") ||
            ci_strstr(name, "HAE_") ||
            ci_strstr(name, "SMOKE") ||
            ci_strstr(name, "GAS") ||
            ci_strstr(name, "WATER") ||
            ci_strstr(name, "SENSOR"))
            return 0;
    }
    if (cc) {
        if (strstr(cc, " 25 ") || strstr(cc, " 25") || strstr(cc, "25 ") ||
            strstr(cc, "0x25") || strstr(cc, " 37 ") || strstr(cc, " 27 ") ||
            !strcmp(cc, "25") || !strcmp(cc, "37"))
            return 1;
    }
    if (type && type[0]) {
        if (ci_strstr(type, "NAS_") ||
            ci_strstr(type, "PLUG") ||
            ci_strstr(type, "SWITCH") ||
            ci_strstr(type, "FIBARO") ||
            ci_strstr(type, "FGWP") ||
            ci_strstr(type, "GREENWAVE") ||
            ci_strstr(type, "POWERNODE") ||
            ci_strstr(type, "QUBINO") ||
            ci_strstr(type, "AEOTEC") ||
            ci_strstr(type, "RELAY") ||
            ci_strstr(type, "SOCKET"))
            return 1;
    }
    return 0;
}

static const char * zwave_names_dir(void) {
    const char * d = getenv("TOONUI_DATA_DIR");
    if (d && *d) return d;
    if (access("/mnt/data", W_OK) == 0) return "/mnt/data";
    return ".";
}

int zwave_get_custom_name(const char * uuid, int node_id, char * out, size_t outsz) {
    if (!out || outsz == 0) return 0;
    out[0] = 0;

    /* 1. Check toonui_zwave_names.cfg */
    char path[256];
    snprintf(path, sizeof path, "%s/toonui_zwave_names.cfg", zwave_names_dir());
    FILE * f = fopen(path, "r");
    if (f) {
        char line[160];
        char dev_id_str[32] = "";
        char node_id_str[32] = "";
        if (node_id > 0) {
            snprintf(dev_id_str, sizeof dev_id_str, "dev_%d", node_id);
            snprintf(node_id_str, sizeof node_id_str, "%d", node_id);
        }
        while (fgets(line, sizeof line, f)) {
            char * eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char * key = line;
            char * val = eq + 1;
            size_t vl = strlen(val);
            while (vl > 0 && (val[vl - 1] == '\r' || val[vl - 1] == '\n')) val[--vl] = 0;
            if (!val[0]) continue;

            int match = 0;
            if (uuid && uuid[0] && strcmp(key, uuid) == 0) match = 1;
            else if (dev_id_str[0] && strcmp(key, dev_id_str) == 0) match = 1;
            else if (node_id_str[0] && strcmp(key, node_id_str) == 0) match = 1;

            if (match) {
                snprintf(out, outsz, "%s", val);
                fclose(f);
                return 1;
            }
        }
        fclose(f);
    }

    /* 2. Check stock config_happ_smartplug.xml */
    const char * xml_paths[] = {
        "/mnt/data/qmf/config/config_happ_smartplug.xml",
        "/qmf/config/config_happ_smartplug.xml",
        NULL
    };
    for (int k = 0; xml_paths[k]; k++) {
        FILE * fx = fopen(xml_paths[k], "r");
        if (!fx) continue;
        fseek(fx, 0, SEEK_END);
        long sz = ftell(fx);
        fseek(fx, 0, SEEK_SET);
        if (sz > 0 && sz < 256 * 1024) {
            char * buf = malloc((size_t)sz + 1);
            if (buf) {
                size_t rd = fread(buf, 1, (size_t)sz, fx);
                buf[rd] = 0;
                const char * scan = buf;
                char dev_str[32] = "";
                if (node_id > 0) snprintf(dev_str, sizeof dev_str, "dev_%d", node_id);

                while ((scan = strstr(scan, "<device>"))) {
                    const char * d_end = strstr(scan, "</device>");
                    if (!d_end) break;

                    char name_buf[64] = "";
                    char zw_buf[64] = "";
                    char dev_buf[64] = "";
                    xml_elem(scan, d_end, "Name", name_buf, sizeof name_buf);
                    if (!name_buf[0]) xml_elem(scan, d_end, "name", name_buf, sizeof name_buf);
                    xml_elem(scan, d_end, "ZWUuid", zw_buf, sizeof zw_buf);
                    if (!zw_buf[0]) xml_elem(scan, d_end, "zwUuid", zw_buf, sizeof zw_buf);
                    xml_elem(scan, d_end, "DevUUID", dev_buf, sizeof dev_buf);
                    if (!dev_buf[0]) xml_elem(scan, d_end, "devUuid", dev_buf, sizeof dev_buf);

                    int match = 0;
                    if (uuid && uuid[0]) {
                        if (zw_buf[0] && strstr(uuid, zw_buf)) match = 1;
                        else if (dev_buf[0] && strstr(uuid, dev_buf)) match = 1;
                        else if (zw_buf[0] && strstr(zw_buf, uuid)) match = 1;
                    }
                    if (!match && dev_str[0] && dev_buf[0] && strstr(dev_buf, dev_str)) {
                        match = 1;
                    }

                    if (match && name_buf[0]) {
                        snprintf(out, outsz, "%s", name_buf);
                        free(buf);
                        fclose(fx);
                        return 1;
                    }
                    scan = d_end + 9;
                }
                free(buf);
            }
        }
        fclose(fx);
    }

    return 0;
}

static void zwave_set_custom_name(const char * uuid, const char * name) {
    if (!uuid || !uuid[0]) return;
    char path[256], tmppath[280];
    const char * dir = zwave_names_dir();
    snprintf(path, sizeof path, "%s/toonui_zwave_names.cfg", dir);
    snprintf(tmppath, sizeof tmppath, "%s/toonui_zwave_names.cfg.tmp", dir);

    typedef struct { char u[64]; char n[64]; } entry_t;
    entry_t entries[MAX_ZW_DEV + 4];
    int count = 0;
    int updated = 0;

    FILE * f = fopen(path, "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof line, f) && count < MAX_ZW_DEV) {
            char * eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char * val = eq + 1;
            size_t vl = strlen(val);
            while (vl > 0 && (val[vl - 1] == '\r' || val[vl - 1] == '\n')) val[--vl] = 0;
            if (strcmp(line, uuid) == 0) {
                if (name && name[0]) {
                    snprintf(entries[count].u, sizeof entries[count].u, "%.63s", uuid);
                    snprintf(entries[count].n, sizeof entries[count].n, "%.63s", name);
                    count++;
                }
                updated = 1;
            } else {
                snprintf(entries[count].u, sizeof entries[count].u, "%.63s", line);
                snprintf(entries[count].n, sizeof entries[count].n, "%.63s", val);
                count++;
            }
        }
        fclose(f);
    }

    if (!updated && name && name[0] && count < MAX_ZW_DEV) {
        snprintf(entries[count].u, sizeof entries[count].u, "%.63s", uuid);
        snprintf(entries[count].n, sizeof entries[count].n, "%.63s", name);
        count++;
    }

    FILE * out = fopen(tmppath, "w");
    if (!out) return;
    for (int i = 0; i < count; i++) {
        fprintf(out, "%s=%s\n", entries[i].u, entries[i].n);
    }
    fclose(out);
    rename(tmppath, path);
}

static int g_rows_built = 0;
static time_t g_switch_settle_until = 0;

/* Sync switch on/off states and power from hdrv_zwave?action=getDevices.json */
static void sync_states_from_json_buf(const char * body) {
    if (!body) return;
    time_t now = time(NULL);
    for (int i = 0; i < g_dev_count; i++) {
        zw_dev_t * d = &g_dev[i];
        if (!d->is_switch) continue;
        char needle[64];
        snprintf(needle, sizeof needle, "\"%s\"", d->uuid);
        const char * p = strstr(body, needle);
        if (!p && d->node_id > 0) {
            snprintf(needle, sizeof needle, "\"dev_%d\"", d->node_id);
            p = strstr(body, needle);
        }
        if (p) {
            const char * next_dev = strstr(p + 1, "\"dev_");
            const char * end = next_dev ? next_dev : p + strlen(p);

            /* Don't overwrite state while waiting for recent user toggle to settle */
            if (now >= g_switch_settle_until) {
                const char * cs = strstr(p, "\"TargetStatus\":");
                if (!cs || cs > end) cs = strstr(p, "\"CurrentState\":");
                if (cs && cs < end) {
                    while (*cs && *cs != ':' && cs < end) cs++;
                    if (*cs == ':') {
                        cs++;
                        while (*cs == ' ' || *cs == '\"') cs++;
                        if (*cs == '0' || *cs == '1')
                            d->state = (*cs == '1') ? 1 : 0;
                    }
                }
            } else {
                /* If driver / device has already recorded the commanded state, settle early */
                const char * cs = strstr(p, "\"TargetStatus\":");
                if (!cs || cs > end) cs = strstr(p, "\"CurrentState\":");
                if (cs && cs < end) {
                    while (*cs && *cs != ':' && cs < end) cs++;
                    if (*cs == ':') {
                        cs++;
                        while (*cs == ' ' || *cs == '\"') cs++;
                        if ((*cs == '1' && d->state == 1) || (*cs == '0' && d->state == 0))
                            g_switch_settle_until = 0;
                    }
                }
            }

            const char * flow = strstr(p, "\"CurrentElectricityFlow\"");
            if (flow && flow < end) {
                flow += strlen("\"CurrentElectricityFlow\"");
                while (*flow && (*flow == ' ' || *flow == ':' || *flow == '\"')) flow++;
                if (strncmp(flow, "NaN", 3) != 0 && strncmp(flow, "null", 4) != 0) {
                    d->power_w = (float)atof(flow);
                }
            }
            char custom[64] = "";
            if (zwave_get_custom_name(d->uuid, d->node_id, custom, sizeof custom) && custom[0]) {
                snprintf(d->name, sizeof d->name, "%s", custom);
            }
        }
    }
}

static void parse_devices_json_buf(const char * body) {
    if (!body) return;
    const char * p = body;
    while (g_dev_count < MAX_ZW_DEV && (p = strstr(p, "\"dev_"))) {
        const char * next = strstr(p + 5, "\"dev_");
        const char * dend = next ? next : p + strlen(p);

        /* Only real nodes ("dev_4"): skip a node's endpoints ("dev_4.1" — a
         * meter adapter has one per meter) and "dev_settings_device". The
         * BoxTalk GetDevices list the Z-Wave screen shows has only nodes, so
         * these used to flash up in the list and vanish on its first refresh. */
        {
            const char * k = p + 5;
            int numeric = (*k >= '0' && *k <= '9');
            while (numeric && *k && *k != '"') { if (*k < '0' || *k > '9') numeric = 0; k++; }
            if (!numeric) { p = next ? next : dend; continue; }
        }

        zw_dev_t * d = &g_dev[g_dev_count];
        memset(d, 0, sizeof *d);

        /* Extract uuid */
        const char * u = strstr(p, "\"uuid\":");
        if (u && u < dend) {
            u += 7;
            while (*u == ' ' || *u == '\"') u++;
            size_t n = strcspn(u, "\",\r\n}");
            if (n >= sizeof d->uuid) n = sizeof d->uuid - 1;
            memcpy(d->uuid, u, n);
            d->uuid[n] = 0;
        }

        /* Extract name */
        const char * nm = strstr(p, "\"DeviceName\":");
        if (!nm || nm > dend) nm = strstr(p, "\"name\":");
        if (nm && nm < dend) {
            nm += (*nm == '\"' && nm[1] == 'D') ? 13 : 7;
            while (*nm == ' ' || *nm == '\"') nm++;
            size_t n = strcspn(nm, "\",\r\n}");
            if (n >= sizeof d->name) n = sizeof d->name - 1;
            memcpy(d->name, nm, n);
            d->name[n] = 0;
        }

        /* Extract type */
        const char * tp = strstr(p, "\"type\":");
        if (tp && tp < dend) {
            tp += 7;
            while (*tp == ' ' || *tp == '\"') tp++;
            size_t n = strcspn(tp, "\",\r\n}");
            if (n >= sizeof d->type) n = sizeof d->type - 1;
            memcpy(d->type, tp, n);
            d->type[n] = 0;
        }

        /* Extract nodeId / internalAddress */
        const char * ia = strstr(p, "\"internalAddress\":");
        if (ia && ia < dend) {
            ia += 18;
            while (*ia == ' ' || *ia == '\"') ia++;
            d->node_id = atoi(ia);
        }

        /* Check switch CC or plug model */
        const char * cc = strstr(p, "\"ccList\":");
        if (!cc || cc > dend) cc = strstr(p, "\"supportedCC\":");
        char cc_buf[256] = "";
        if (cc && cc < dend) {
            cc += (*cc == '\"' && cc[1] == 'c') ? 9 : 14;
            while (*cc == ' ' || *cc == '\"' || *cc == ':') cc++;
            size_t n = strcspn(cc, "\",\r\n}");
            if (n >= sizeof cc_buf) n = sizeof cc_buf - 1;
            memcpy(cc_buf, cc, n);
            cc_buf[n] = 0;
        }
        d->is_switch = zwave_is_switch_device(d->type, d->name, cc_buf, d->node_id);

        /* Switch state */
        d->state = -1;
        const char * cs = strstr(p, "\"TargetStatus\":");
        if (!cs || cs > dend) cs = strstr(p, "\"CurrentState\":");
        if (cs && cs < dend) {
            while (*cs && *cs != ':' && cs < dend) cs++;
            if (*cs == ':') {
                cs++;
                while (*cs == ' ' || *cs == '\"') cs++;
                if (*cs == '0' || *cs == '1')
                    d->state = (*cs == '1') ? 1 : 0;
            }
        }

        /* Live power */
        const char * flow = strstr(p, "\"CurrentElectricityFlow\"");
        if (flow && flow < dend) {
            flow += strlen("\"CurrentElectricityFlow\"");
            while (*flow && (*flow == ' ' || *flow == ':' || *flow == '\"')) flow++;
            if (strncmp(flow, "NaN", 3) != 0 && strncmp(flow, "null", 4) != 0) {
                d->power_w = (float)atof(flow);
            }
        }

        /* Apply custom name override if user renamed this device */
        char custom[64] = "";
        if (zwave_get_custom_name(d->uuid, d->node_id, custom, sizeof custom) && custom[0]) {
            snprintf(d->name, sizeof d->name, "%s", custom);
        }

        if (d->uuid[0]) g_dev_count++;
        p = next ? next : dend;
    }
}

static void sync_states_from_json(void) {
    static char body[64 * 1024];
    if (stats_http_get("hdrv_zwave?action=getDevices.json", body, sizeof body) != 0) return;
    sync_states_from_json_buf(body);
}

void zwave_poll(void) {
    static char body[64 * 1024];
    if (stats_http_get("hdrv_zwave?action=getDevices.json", body, sizeof body) != 0) return;
    if (g_dev_count == 0) {
        parse_devices_json_buf(body);
    } else {
        sync_states_from_json_buf(body);
    }
}

/* Parse zwave_response_buf (GetDevicesResponse XML) into g_dev[].
 * Returns 1 if rows need rebuilding, 0 if device list/state unchanged. */
static int parse_devices(void) {
    zw_dev_t old_dev[MAX_ZW_DEV];
    int old_count = g_dev_count;
    memcpy(old_dev, g_dev, sizeof(old_dev));

    g_dev_count = 0;
    const char * buf = zwave_response_buf;
    const char * ds = strstr(buf, "<devices>");
    const char * de = strstr(buf, "</devices>");
    if (!ds || !de) return 0;
    ds += strlen("<devices>");

    const char * scan = ds;
    while (g_dev_count < MAX_ZW_DEV) {
        const char * d0 = strstr(scan, "<device>");
        if (!d0 || d0 >= de) break;
        const char * d1 = strstr(d0, "</device>");
        if (!d1 || d1 > de) break;
        d0 += strlen("<device>");

        zw_dev_t * d = &g_dev[g_dev_count];
        memset(d, 0, sizeof *d);
        xml_elem(d0, d1, "uuid", d->uuid, sizeof d->uuid);
        if (!xml_elem(d0, d1, "name", d->name, sizeof d->name) || !d->name[0]) {
            if (!xml_elem(d0, d1, "deviceName", d->name, sizeof d->name) || !d->name[0]) {
                xml_elem(d0, d1, "devicename", d->name, sizeof d->name);
            }
        }
        xml_elem(d0, d1, "type", d->type, sizeof d->type);

        char tmp[64];
        if (xml_elem(d0, d1, "nodeId", tmp, sizeof tmp) && tmp[0])
            d->node_id = atoi(tmp);
        else if (xml_elem(d0, d1, "internalAddress", tmp, sizeof tmp))
            d->node_id = atoi(tmp);

        /* Apply custom name override if user renamed this device */
        char custom[64] = "";
        if (d->uuid[0] && zwave_get_custom_name(d->uuid, d->node_id, custom, sizeof custom) && custom[0]) {
            snprintf(d->name, sizeof d->name, "%s", custom);
        }

        /* Binary-switch capable */
        char cc[256] = "";
        if (xml_elem(d0, d1, "ccList", cc, sizeof cc) ||
            xml_elem(d0, d1, "cclist", cc, sizeof cc) ||
            xml_elem(d0, d1, "supportedCC", cc, sizeof cc) ||
            xml_elem(d0, d1, "supportedCommandClasses", cc, sizeof cc) ||
            xml_elem(d0, d1, "classes", cc, sizeof cc)) {
        }
        d->is_switch = zwave_is_switch_device(d->type, d->name, cc, d->node_id);

        /* Check previous known switch state and power so we don't clobber it */
        int prev_state = -1;
        float prev_power = 0.0f;
        for (int j = 0; j < old_count; j++) {
            if (strcmp(old_dev[j].uuid, d->uuid) == 0) {
                if (old_dev[j].state >= 0) prev_state = old_dev[j].state;
                prev_power = old_dev[j].power_w;
                break;
            }
        }
        d->power_w = prev_power;

        /* NOTE: Do NOT use <state>! In hdrv_zwave GetDevices XML, <state>1</state>
         * is the Z-Wave node reachability/alive status, NOT the switch on/off state. */
        if (xml_elem(d0, d1, "TargetStatus", tmp, sizeof tmp) && tmp[0])
            d->state = atoi(tmp);
        else if (xml_elem(d0, d1, "targetStatus", tmp, sizeof tmp) && tmp[0])
            d->state = atoi(tmp);
        else if (xml_elem(d0, d1, "CurrentState", tmp, sizeof tmp) && tmp[0])
            d->state = atoi(tmp);
        else if (xml_elem(d0, d1, "currentState", tmp, sizeof tmp) && tmp[0])
            d->state = atoi(tmp);
        else if (prev_state >= 0)
            d->state = prev_state;
        else
            d->state = 0;

        if (d->uuid[0]) g_dev_count++;
        scan = d1 + strlen("</device>");
    }

    /* On initial load (before user interaction), sync real state from driver JSON */
    if (!g_rows_built) {
        sync_states_from_json();
    }

    if (!g_rows_built || g_dev_count != old_count) return 1;
    for (int i = 0; i < g_dev_count; i++) {
        if (strcmp(g_dev[i].uuid, old_dev[i].uuid) != 0 ||
            strcmp(g_dev[i].name, old_dev[i].name) != 0 ||
            g_dev[i].state != old_dev[i].state) {
            return 1;
        }
    }
    return 0;
}

/* ===================================================================== */
/* Event handlers (all UI thread; boxtalk_zwave_* are lock-guarded)       */
/* ===================================================================== */
static void on_switch_toggled(lv_event_t * e) {
    zw_dev_t * d = (zw_dev_t *)lv_event_get_user_data(e);
    lv_obj_t * sw = lv_event_get_target(e);
    int want = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    d->state = want;
    g_switch_settle_until = time(NULL) + 5;
    boxtalk_zwave_basic_set(d->uuid, d->node_id, want);
    snprintf(g_status, sizeof g_status, "%s -> %s", d->name[0] ? d->name : d->type, want ? tr("aan", "on") : tr("uit", "off"));
    if (lbl_status) lv_label_set_text(lbl_status, g_status);
    g_poll_ticks = 4;   /* pause polling to let mesh settle */
}

/* --- rename modal --------------------------------------------------- */
static char       g_rename_uuid[40];
static lv_obj_t * g_rename_ta = NULL;
static lv_obj_t * g_rename_modal = NULL;

static void rename_close(void) {
    if (g_rename_modal) { lv_obj_del(g_rename_modal); g_rename_modal = NULL; }
    g_rename_ta = NULL;
}
static void on_rename_cancel(lv_event_t * e) { (void)e; rename_close(); }
static void on_rename_save(lv_event_t * e) {
    (void)e;
    if (!g_rename_ta) return;
    /* Strip XML metacharacters so the name is safe inside the action XML. */
    const char * src = lv_textarea_get_text(g_rename_ta);
    char clean[64]; size_t o = 0;
    for (const char * p = src; *p && o + 1 < sizeof clean; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '<' || c == '>' || c == '&' || c == '"' || c == '\'' || c < 0x20)
            continue;
        clean[o++] = (char)c;
    }
    clean[o] = 0;
    boxtalk_zwave_set_name(g_rename_uuid, clean);
    zwave_set_custom_name(g_rename_uuid, clean);

    /* Update in-memory list and redraw immediately so the new name appears right away */
    for (int i = 0; i < g_dev_count; i++) {
        if (strcmp(g_dev[i].uuid, g_rename_uuid) == 0) {
            snprintf(g_dev[i].name, sizeof g_dev[i].name, "%s", clean);
            break;
        }
    }
    build_rows();

    snprintf(g_status, sizeof g_status, tr("Hernoemd naar %s", "Renamed to %s"), clean);
    g_poll_ticks = 3;
    rename_close();
}

static void on_rename_kb_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        on_rename_save(NULL);
    } else if (code == LV_EVENT_CANCEL) {
        rename_close();
    }
}

static void on_row_rename(lv_event_t * e) {
    zw_dev_t * d = (zw_dev_t *)lv_event_get_user_data(e);
    snprintf(g_rename_uuid, sizeof g_rename_uuid, "%s", d->uuid);

    g_rename_modal = lv_obj_create(scr_root);
    lv_obj_set_size(g_rename_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_rename_modal, 0, 0);
    lv_obj_set_style_bg_color(g_rename_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_rename_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_rename_modal, 0, 0);
    lv_obj_clear_flag(g_rename_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_rename_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_rename_modal, on_rename_cancel, LV_EVENT_CLICKED, NULL);

    lv_obj_t * card = lv_obj_create(g_rename_modal);
    lv_obj_set_size(card, SX(640), SY(200));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, SY(24));
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, SX(16), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * t = lv_label_create(card);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(t, SF(22), 0);
    lv_label_set_text(t, tr("Apparaat hernoemen", "Rename device"));
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    g_rename_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(g_rename_ta, true);
    lv_textarea_set_text(g_rename_ta, d->name[0] ? d->name : d->type);
    lv_obj_set_size(g_rename_ta, SX(608), SY(46));
    lv_obj_align(g_rename_ta, LV_ALIGN_TOP_MID, 0, SY(38));
    lv_obj_set_style_text_font(g_rename_ta, SF(20), 0);

    lv_obj_t * cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, SX(130), SY(42));
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(COL_OFF), 0);
    lv_obj_add_event_cb(cancel, on_rename_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(cancel); lv_label_set_text(cl, tr("Annuleer", "Cancel"));
    lv_obj_center(cl);

    lv_obj_t * save = lv_btn_create(card);
    lv_obj_set_size(save, SX(130), SY(42));
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(COL_OK), 0);
    lv_obj_add_event_cb(save, on_rename_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(save); lv_label_set_text(sl, tr("Opslaan", "Save"));
    lv_obj_set_style_text_color(sl, lv_color_hex(THEME_ON_ACCENT), 0);
    lv_obj_center(sl);

    lv_obj_t * kb = lv_keyboard_create(g_rename_modal);
    lv_obj_set_size(kb, LV_HOR_RES, (DISP_VER < 600) ? 220 : 260);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, g_rename_ta);
    lv_obj_add_event_cb(kb, on_rename_kb_event, LV_EVENT_ALL, NULL);
}

/* --- delete modal --------------------------------------------------- */
static char       g_delete_uuid[48] = "";
static lv_obj_t * g_delete_modal = NULL;

static void delete_close(void) {
    if (g_delete_modal) { lv_obj_del(g_delete_modal); g_delete_modal = NULL; }
    g_delete_uuid[0] = 0;
}
static void on_delete_cancel(lv_event_t * e) { (void)e; delete_close(); }
static void on_delete_confirm(lv_event_t * e) {
    (void)e;
    if (g_delete_uuid[0]) {
        ma_force_remove(g_delete_uuid);
        snprintf(g_status, sizeof g_status, "%s", tr("Apparaat verwijderd.", "Device removed."));
        boxtalk_zwave_get_devices();
    }
    delete_close();
}

static void on_row_delete(lv_event_t * e) {
    zw_dev_t * d = (zw_dev_t *)lv_event_get_user_data(e);
    snprintf(g_delete_uuid, sizeof g_delete_uuid, "%s", d->uuid);

    g_delete_modal = lv_obj_create(scr_root);
    lv_obj_set_size(g_delete_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_delete_modal, 0, 0);
    lv_obj_set_style_bg_color(g_delete_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_delete_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_delete_modal, 0, 0);
    lv_obj_clear_flag(g_delete_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_delete_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_delete_modal, on_delete_cancel, LV_EVENT_CLICKED, NULL);

    lv_obj_t * card = lv_obj_create(g_delete_modal);
    lv_obj_set_size(card, SX(560), SY(230));
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, SX(16), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * t = lv_label_create(card);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(t, SF(20), 0);
    lv_label_set_text_fmt(t, tr("Geforceerd \"%s\" verwijderen?", "Force remove \"%s\"?"), d->name[0] ? d->name : d->type);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t * warn = lv_label_create(card);
    lv_obj_set_style_text_color(warn, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(warn, SF(15), 0);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, SX(528));
    lv_label_set_text(warn, tr(
        "Dit verwijdert het apparaat direct uit de Toon-database zonder radiocommunicatie. "
        "Gebruik dit alleen als normaal ontkoppelen via \"- Verwijderen\" mislukt.\n"
        "Als het apparaat nog actief is, dient u het daarna handmatig via de knop op de stekker te resetten.",
        "This deletes the device from the Toon database without radio exclusion. "
        "Only use this if normal removal via \"- Remove\" fails.\n"
        "If the device is still active, you will need to manually factory reset it using its physical button."));
    lv_obj_align(warn, LV_ALIGN_TOP_LEFT, 0, SY(32));

    lv_obj_t * cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, SX(130), SY(42));
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(COL_OFF), 0);
    lv_obj_add_event_cb(cancel, on_delete_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(cancel); lv_label_set_text(cl, tr("Annuleer", "Cancel"));
    lv_obj_center(cl);

    lv_obj_t * del = lv_btn_create(card);
    lv_obj_set_size(del, SX(170), SY(42));
    lv_obj_align(del, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(del, lv_color_hex(COL_WARN), 0);
    lv_obj_add_event_cb(del, on_delete_confirm, LV_EVENT_CLICKED, NULL);
    lv_obj_t * dl = lv_label_create(del); lv_label_set_text(dl, tr("Forceer verwijder", "Force remove"));
    lv_obj_set_style_text_color(dl, lv_color_hex(THEME_ON_ACCENT), 0);
    lv_obj_center(dl);
}

static void on_include(lv_event_t * e) {
    (void)e;
    if (g_exc_active) { zw_exc(0); g_exc_active = 0; }
    g_inc_active = !g_inc_active;
    zw_inc(g_inc_active);
    snprintf(g_status, sizeof g_status, "%s", g_inc_active
        ? tr("Toevoegen actief - start koppelen op het apparaat (stopt vanzelf na 2 min).",
             "Inclusion active - trigger pairing on the device (auto-stops in 2 min).")
        : tr("Toevoegen gestopt.", "Inclusion stopped."));
    g_poll_ticks = 0;
}
static void on_exclude(lv_event_t * e) {
    (void)e;
    if (g_inc_active) { zw_inc(0); g_inc_active = 0; }
    g_exc_active = !g_exc_active;
    zw_exc(g_exc_active);
    snprintf(g_status, sizeof g_status, "%s", g_exc_active
        ? tr("Verwijderen actief - start ontkoppelen op het apparaat (stopt vanzelf na 30 s).",
             "Removal active - trigger exclusion on the device (auto-stops in 30 s).")
        : tr("Verwijderen gestopt.", "Removal stopped."));
    g_poll_ticks = 0;
}
static void on_heal(lv_event_t * e) {
    (void)e;
    boxtalk_zwave_heal();
    snprintf(g_status, sizeof g_status, "%s", tr("Herstel aangevraagd - routering wordt op de achtergrond bijgewerkt.", "Heal requested - routing update runs in the background."));
}

static void on_enable_toggled(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    settings.enable_zwave = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
    if (!settings.enable_zwave && (g_inc_active || g_exc_active)) {
        if (g_inc_active) zw_inc(0);
        if (g_exc_active) zw_exc(0);
        g_inc_active = g_exc_active = 0;
    }
    snprintf(g_status, sizeof g_status, "%s", settings.enable_zwave
        ? tr("Bediening ingeschakeld.", "Control enabled.")
        : tr("Schakel bediening in om apparaten te beheren.", "Enable control to manage devices."));
}

/* ===================================================================== */
/* List rendering                                                        */
/* ===================================================================== */
static void build_rows(void) {
    lv_obj_clean(list_box);

    if (g_dev_count == 0) {
        lv_obj_t * empty = lv_label_create(list_box);
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(empty, SF(18), 0);
        lv_label_set_text(empty,
            tr("Geen Z-Wave apparaten gekoppeld.\n"
               "Tik op \"Apparaat toevoegen\" en start koppelen op het apparaat.",
               "No Z-Wave devices paired.\n"
               "Tap \"Add device\" and trigger inclusion on the device."));
        return;
    }

    for (int i = 0; i < g_dev_count; i++) {
        zw_dev_t * d = &g_dev[i];
        lv_obj_t * row = lv_obj_create(list_box);
        lv_obj_set_size(row, SX(940), SY(84));
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_border_width(row, THEME_IS_LIGHT ? 1 : 0, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * nm = lv_label_create(row);
        lv_obj_set_style_text_font(nm, SF(22), 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(COL_TEXT_HI), 0);
        lv_label_set_text(nm, d->name[0] ? d->name : d->type);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, SX(4), 0);

        lv_obj_t * sub = lv_label_create(row);
        lv_obj_set_style_text_font(sub, SF(14), 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(COL_TEXT_DIM), 0);
        lv_label_set_text_fmt(sub, tr("%s   node %d", "%s   node %d"), d->type, d->node_id);
        lv_obj_align(sub, LV_ALIGN_TOP_LEFT, SX(4), SY(34));

        int x = -4;
        if (settings.enable_zwave) {
            if (d->node_id > 1) {
                lv_obj_t * del = lv_btn_create(row);
                lv_obj_set_size(del, SX(110), SY(48));
                lv_obj_align(del, LV_ALIGN_RIGHT_MID, SX(x), 0);
                lv_obj_set_style_bg_color(del, lv_color_hex(COL_WARN), 0);
                lv_obj_add_event_cb(del, on_row_delete, LV_EVENT_CLICKED, d);
                lv_obj_t * dl = lv_label_create(del); lv_label_set_text(dl, tr("Verwijder", "Delete"));
                lv_obj_set_style_text_color(dl, lv_color_hex(THEME_ON_ACCENT), 0);
                lv_obj_center(dl);
                x -= 120;
            }

            lv_obj_t * ren = lv_btn_create(row);
            lv_obj_set_size(ren, SX(110), SY(48));
            lv_obj_align(ren, LV_ALIGN_RIGHT_MID, SX(x), 0);
            lv_obj_set_style_bg_color(ren, lv_color_hex(THEME_BTN_ACCENT), 0);
            lv_obj_add_event_cb(ren, on_row_rename, LV_EVENT_CLICKED, d);
            lv_obj_t * rl = lv_label_create(ren); lv_label_set_text(rl, tr("Hernoem", "Rename"));
            lv_obj_center(rl);
            x -= 120;

            if (d->is_switch) {
                lv_obj_t * sw = lv_switch_create(row);
                lv_obj_align(sw, LV_ALIGN_RIGHT_MID, SX(x - 10), 0);
                if (d->state == 1) lv_obj_add_state(sw, LV_STATE_CHECKED);
                else lv_obj_clear_state(sw, LV_STATE_CHECKED);
                lv_obj_add_event_cb(sw, on_switch_toggled, LV_EVENT_VALUE_CHANGED, d);
            }
        }
    }
    g_rows_built = 1;
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (!scr_root || lv_scr_act() != scr_root) return;
    /* Poll for a pending GetDevices response. */
    if (zwave_response_ready) {
        int need_rebuild = parse_devices();
        zwave_response_ready = 0;
        if (need_rebuild) {
            build_rows();
            if (lbl_ctrl)
                lv_label_set_text_fmt(lbl_ctrl, "%d %s", g_dev_count,
                    g_dev_count == 1 ? tr("apparaat", "device") : tr("apparaten", "devices"));
        }
    }

    /* Result of add / remove (ma_admin). */
    {
        ma_admin_t a; ma_get_admin(&a);
        if (g_admin_seq0 >= 0 && a.busy && time(NULL) - g_admin_t0 > 150)
            ma_admin_timeout();                 /* driver never answered */
        if (g_admin_seq0 >= 0 && a.seq != g_admin_seq0 && !a.busy) {
            g_admin_seq0 = -1;
            int was_inc = g_inc_active, was_exc = g_exc_active;
            g_inc_active = g_exc_active = 0;
            g_poll_ticks = 0;
            if (was_inc && !strcmp(a.status, "added")) {
                snprintf(g_status, sizeof g_status, tr("Toegevoegd: %s. Controleren of het een stekker is...",
                                                       "Added: %s. Checking whether it is a plug..."), a.dev_type);
                snprintf(g_plug_uuid, sizeof g_plug_uuid, "%s", a.uuid);
                g_plug_deadline = time(NULL) + 45;
                g_plug_tick = 0;
            } else if (was_exc && !strcmp(a.status, "deleted") && a.uuid[0]) {
                snprintf(g_status, sizeof g_status, tr("Verwijderd: %s.", "Removed: %s."), a.dev_type);
            } else if (was_exc && !strcmp(a.status, "deleted")) {
                snprintf(g_status, sizeof g_status, "%s", tr("Er is geen apparaat verwijderd.", "No device was removed."));
            } else if (was_inc || was_exc) {
                snprintf(g_status, sizeof g_status, tr("Niet gelukt (%s).", "Did not work (%s)."), a.status);
            }
        }
    }
    /* A just-added device: once hdrv_zwave lists its electricity counter it
     * is a smart plug -> EnableDevice, so happ_smartplug creates its loggers. */
    if (g_plug_uuid[0] && g_plug_tick-- <= 0) {
        g_plug_tick = 3;
        int r = ma_zwave_is_plug(g_plug_uuid);
        if (r == 1) {
            ma_enable_plug(g_plug_uuid);
            snprintf(g_status, sizeof g_status, "%s",
                     tr("Stekker toegevoegd. Het verbruik verschijnt binnen een uur onder Statistieken > Stekkers.",
                        "Plug added. Its usage shows up under Statistics > Plugs within an hour."));
            g_plug_uuid[0] = 0;
        } else if (time(NULL) > g_plug_deadline) {
            fprintf(stderr, "[zwave] %s: no electricity counter, not a smart plug\n", g_plug_uuid);
            snprintf(g_status, sizeof g_status, "%s", tr("Apparaat toegevoegd.", "Device added."));
            g_plug_uuid[0] = 0;
        }
    }

    /* Periodically ask for the device list (every ~3 s, faster right after an
     * action sets g_poll_ticks = 0). */
    if (g_poll_ticks <= 0) {
        boxtalk_zwave_get_devices();
        g_poll_ticks = 3;
    } else {
        g_poll_ticks--;
    }

    /* Status + control-gated widgets. */
    if (lbl_status)
        lv_label_set_text(lbl_status, g_status[0] ? g_status :
            (settings.enable_zwave ? tr("Bediening ingeschakeld. Apparaten toevoegen, verwijderen of schakelen.",
                                        "Control enabled. Add, remove or toggle devices.")
                                   : tr("Schakel bediening in om apparaten te beheren.",
                                        "Enable control to manage devices.")));
    if (sw_enable) {
        if (settings.enable_zwave) lv_obj_add_state(sw_enable, LV_STATE_CHECKED);
        else                       lv_obj_clear_state(sw_enable, LV_STATE_CHECKED);
    }
    if (bar_actions) {
        if (settings.enable_zwave) lv_obj_clear_flag(bar_actions, LV_OBJ_FLAG_HIDDEN);
        else                       lv_obj_add_flag(bar_actions, LV_OBJ_FLAG_HIDDEN);
    }
    if (btn_inc_lbl) lv_label_set_text(btn_inc_lbl, g_inc_active ? tr("Stop toevoegen", "Stop add") : tr("+ Apparaat toevoegen", "+ Add device"));
    if (btn_exc_lbl) lv_label_set_text(btn_exc_lbl, g_exc_active ? tr("Stop verwijderen", "Stop remove") : tr("- Verwijderen", "- Remove"));
}

/* ===================================================================== */
/* Screen build                                                          */
/* ===================================================================== */
static void back_async(void * u) { (void)u; ui_pop(); }
static void on_back(lv_event_t * e) {
    (void)e;
    rename_close();
    delete_close();
    /* Leave no inclusion/exclusion window open behind us. */
    if (g_inc_active) { zw_inc(0); g_inc_active = 0; }
    if (g_exc_active) { zw_exc(0); g_exc_active = 0; }
    lv_async_call(back_async, NULL);
}

static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        if (refresh_timer) lv_timer_resume(refresh_timer);
        g_poll_ticks = 0;
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) lv_timer_pause(refresh_timer);
        g_rows_built = 0;
    }
}

static lv_obj_t * mk_action_btn(lv_obj_t * parent, const char * txt,
                                uint32_t col, lv_event_cb_t cb, int x,
                                lv_obj_t ** out_lbl) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, SX(300), SY(56));
    lv_obj_align(b, LV_ALIGN_LEFT_MID, SX(x), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l = lv_label_create(b);
    /* neutral button: normal text; ok/remove fills: text on accent */
    lv_obj_set_style_text_color(l, lv_color_hex(col == (uint32_t)THEME_BTN_ACCENT ? THEME_TEXT_HI : THEME_ON_ACCENT), 0);
    lv_obj_set_style_text_font(l, SF(18), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (out_lbl) *out_lbl = l;
    return b;
}

static int scr_light_mode = -1;   /* cached UI theme (settings.ui_theme) */

lv_obj_t * screen_zwave_create(void) {
    if (scr_root && scr_light_mode == settings.ui_theme) return scr_root;
    if (scr_root) { lv_obj_del(scr_root); scr_root = NULL; }
    /* Rebuild (light-mode/theme toggle): the old timer must go too, or one
     * more paused timer leaks per toggle. */
    if (refresh_timer) { lv_timer_del(refresh_timer); refresh_timer = NULL; }
    scr_light_mode = settings.ui_theme;
    g_rows_built = 0;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_LOADED,   NULL);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_UNLOADED, NULL);

    int x_w = (DISP_HOR < 1000) ? 68 : 74;
    int x_h = (DISP_VER < 600) ? 46 : 50;

    lv_obj_t * x = lv_btn_create(scr_root);
    lv_obj_set_size(x, x_w, x_h);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, SX(-16), SY(12));
    lv_obj_set_style_bg_color(x, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(x, 12, 0);
    lv_obj_set_ext_click_area(x, 25);
    lv_obj_add_event_cb(x, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(xl, SF(22), 0);
    lv_obj_set_style_text_color(xl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_center(xl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, "Z-Wave");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(24), SY(20));

    lbl_ctrl = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_ctrl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_ctrl, SF(14), 0);
    lv_label_set_text(lbl_ctrl, tr("opvragen...", "querying..."));
    lv_obj_align(lbl_ctrl, LV_ALIGN_TOP_LEFT, SX(150), SY(30));

    sw_enable = lv_switch_create(scr_root);
    lv_obj_set_size(sw_enable, SX(64), SY(32));
    lv_obj_align(sw_enable, LV_ALIGN_TOP_RIGHT, SX(-16) - x_w - SX(16), SY(18));
    if (settings.enable_zwave) lv_obj_add_state(sw_enable, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_enable, on_enable_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * en_lbl = lv_label_create(scr_root);
    lv_obj_set_style_text_color(en_lbl, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(en_lbl, SF(18), 0);
    lv_label_set_text(en_lbl, tr("Bediening aan", "Enable control"));
    lv_obj_align_to(en_lbl, sw_enable, LV_ALIGN_OUT_LEFT_MID, SX(-14), 0);

    bar_actions = lv_obj_create(scr_root);
    lv_obj_set_size(bar_actions, SX(980), SY(76));
    lv_obj_align(bar_actions, LV_ALIGN_TOP_LEFT, SX(22), SY(70));
    lv_obj_set_style_bg_opa(bar_actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar_actions, 0, 0);
    lv_obj_set_style_pad_all(bar_actions, 0, 0);
    lv_obj_clear_flag(bar_actions, LV_OBJ_FLAG_SCROLLABLE);
    mk_action_btn(bar_actions, tr("+ Apparaat toevoegen", "+ Add device"), COL_OK,   on_include, 0,   &btn_inc_lbl);
    mk_action_btn(bar_actions, tr("- Verwijderen", "- Remove"),     COL_WARN, on_exclude, 320, &btn_exc_lbl);
    mk_action_btn(bar_actions, tr("Netwerk herstellen", "Heal network"), THEME_BTN_ACCENT, on_heal,    640, NULL);
    if (!settings.enable_zwave) lv_obj_add_flag(bar_actions, LV_OBJ_FLAG_HIDDEN);

    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_status, SF(18), 0);
    lv_obj_set_width(lbl_status, SX(980));
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_status,
        settings.enable_zwave ? tr("Bediening ingeschakeld.", "Control enabled.")
                              : tr("Schakel bediening in om apparaten te beheren.", "Enable control to manage devices."));
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, SX(22), SY(152));

    list_box = lv_obj_create(scr_root);
    lv_obj_set_size(list_box, SX(980), SY(388));
    lv_obj_align(list_box, LV_ALIGN_TOP_LEFT, SX(22), SY(188));
    lv_obj_set_style_bg_opa(list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box, 0, 0);
    lv_obj_set_style_pad_all(list_box, 4, 0);
    lv_obj_set_style_pad_row(list_box, 10, 0);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    build_rows();

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}

/* ---- master↔slave bridge -------------------------------------------------
 * The Toon's controller talks Z-Wave over BoxTalk on the master. On the WASM
 * slave we don't have a controller at all; the master serialises its current
 * g_dev[] into the SSE frame and the slave applies it here so the admin
 * screen's list view (build_rows() → g_dev) shows the same nodes. */
int zwave_dev_count(void) { return g_dev_count; }
/* Switch count of the current list without polling (zwave_get_switch polls
 * when the list is empty — too costly for a per-tick check). */
int zwave_switch_count_cached(void) {
    int c = 0;
    for (int i = 0; i < g_dev_count; i++) if (g_dev[i].is_switch) c++;
    return c;
}
int zwave_dev_at(int i, zwave_dev_t * out) {
    if (!out || i < 0 || i >= g_dev_count) return 0;
    *out = g_dev[i];
    return 1;
}

int zwave_get_switch(int idx, zwave_dev_t * out) {
    if (!out || idx < 0) return 0;
    if (g_dev_count == 0) zwave_poll();
    int seen = 0;
    for (int i = 0; i < g_dev_count; i++) {
        if (!g_dev[i].is_switch) continue;
        if (seen++ == idx) {
            *out = g_dev[i];
            return 1;
        }
    }
    return 0;
}

int zwave_toggle_switch(int idx) {
    if (idx < 0) return 0;
    if (g_dev_count == 0) zwave_poll();
    int seen = 0;
    for (int i = 0; i < g_dev_count; i++) {
        if (!g_dev[i].is_switch) continue;
        if (seen++ == idx) {
            int want = (g_dev[i].state == 1) ? 0 : 1;
            g_dev[i].state = want;
            g_switch_settle_until = time(NULL) + 5;
            boxtalk_zwave_basic_set(g_dev[i].uuid, g_dev[i].node_id, want);
            return 1;
        }
    }
    return 0;
}

void zwave_set_devices_from_remote(int n, const zwave_dev_t * src) {
    if (n < 0) n = 0;
    if (n > MAX_ZW_DEV) n = MAX_ZW_DEV;
    for (int i = 0; i < n; i++) g_dev[i] = src[i];
    g_dev_count = n;
}
