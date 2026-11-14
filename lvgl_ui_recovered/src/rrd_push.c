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

int rrd_logger_exists(const char * name) {
    static const char * const paths[] = {
        "/mnt/data/qmf/config/config_hcb_rrd.xml", "/qmf/config/config_hcb_rrd.xml" };
    char want[96];
    snprintf(want, sizeof want, "<internalAddress>%s</internalAddress>", name);
    for (size_t p = 0; p < sizeof paths / sizeof paths[0]; p++) {
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
