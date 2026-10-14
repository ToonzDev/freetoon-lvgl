/*
 * sonos.c — Native Sonos UPnP / SOAP controller with SSDP auto-discovery.
 * Direct communication with Sonos speakers over local LAN on port 1400.
 * Eliminates external dependencies (no Raspberry Pi or node-sonos-http-api needed).
 */
#include "sonos.h"
#include "settings.h"
#include "display.h"
#include "i18n.h"
#define LV_USE_SJPG 1
#include "lvgl/src/extra/libs/sjpg/tjpgd.h"
#include "lvgl/src/extra/libs/png/lodepng.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

sonos_state_t sonos_state = {0};

static pthread_mutex_t g_sonos_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_sonos_tid = 0;
static volatile int    g_force_poll = 0;

/* Room topology and IP cache */
typedef struct {
    char room[48];
    char ip[32];
    char uuid[48];
} sonos_room_entry_t;

static sonos_room_entry_t g_rooms[SONOS_MAX_ZONES];
static int                g_room_count = 0;
static char               g_active_ip[64] = "";
static char               g_bootstrap_ip[64] = "";
static int                g_poll_fail_count = 0;

/* Cached favorites for instant playback lookup */
typedef struct {
    char title[64];
    char uri[256];
    char meta[512];
} sonos_fav_entry_t;

static sonos_fav_entry_t g_favs[SONOS_MAX_ITEMS];
static int               g_fav_count = 0;

/* Artwork state and in-memory buffers */
static char            g_cur_art_url[256] = "";
static volatile int    g_art_version = 0;
static volatile int    g_art_downloading = 0;

#define ART_BIG_DIM   SUNI(130)
#define ART_TILE_DIM  SUNI(46)

static lv_color_t g_art_big_buf[ART_BIG_DIM * ART_BIG_DIM];
static lv_color_t g_art_tile_buf[ART_TILE_DIM * ART_TILE_DIM];
static int        g_art_has_image = 0;

static const lv_img_dsc_t g_art_big_dsc = {
    .header.always_zero = 0,
    .header.w = ART_BIG_DIM,
    .header.h = ART_BIG_DIM,
    .data_size = sizeof(g_art_big_buf),
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .data = (const uint8_t *)g_art_big_buf,
};

static const lv_img_dsc_t g_art_tile_dsc = {
    .header.always_zero = 0,
    .header.w = ART_TILE_DIM,
    .header.h = ART_TILE_DIM,
    .data_size = sizeof(g_art_tile_buf),
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .data = (const uint8_t *)g_art_tile_buf,
};

void sonos_get_snapshot(sonos_state_t * out) {
    if (!out) return;
    pthread_mutex_lock(&g_sonos_mtx);
    *out = sonos_state;
    pthread_mutex_unlock(&g_sonos_mtx);
}

const lv_img_dsc_t * sonos_get_art_big(int * out_ver) {
    pthread_mutex_lock(&g_sonos_mtx);
    if (out_ver) *out_ver = g_art_version;
    const lv_img_dsc_t * ret = g_art_has_image ? &g_art_big_dsc : NULL;
    pthread_mutex_unlock(&g_sonos_mtx);
    return ret;
}

const lv_img_dsc_t * sonos_get_art_tile(int * out_ver) {
    pthread_mutex_lock(&g_sonos_mtx);
    if (out_ver) *out_ver = g_art_version;
    const lv_img_dsc_t * ret = g_art_has_image ? &g_art_tile_dsc : NULL;
    pthread_mutex_unlock(&g_sonos_mtx);
    return ret;
}

/* =========================================================================
 * XML and Time Helpers
 * ========================================================================= */

static int parse_time_seconds(const char * str) {
    if (!str || !str[0]) return 0;
    int h = 0, m = 0, s = 0;
    if (sscanf(str, "%d:%d:%d", &h, &m, &s) == 3) {
        return h * 3600 + m * 60 + s;
    }
    if (sscanf(str, "%d:%d", &m, &s) == 2) {
        return m * 60 + s;
    }
    return atoi(str);
}

static void clean_url(char * url) {
    if (!url) return;
    char * p;
    while ((p = strstr(url, "&amp;")) != NULL) {
        memmove(p + 1, p + 5, strlen(p + 5) + 1);
    }
}

/* Extracts contents of <tag>...</tag> or &lt;tag...&gt;...&lt;/tag&gt; */
static int extract_xml_tag(const char * xml, const char * tag, char * out, size_t outsz) {
    if (!xml || !tag || !out || outsz == 0) return 0;
    out[0] = 0;

    char open1[64], open2[64];
    snprintf(open1, sizeof open1, "<%s", tag);
    snprintf(open2, sizeof open2, "&lt;%s", tag);
    size_t tlen = strlen(tag);

    const char * cur = xml;
    const char * p = NULL;
    int is_esc = 0;

    while (*cur) {
        const char * p1 = strstr(cur, open1);
        const char * p2 = strstr(cur, open2);
        if (!p1 && !p2) return 0;

        if (p1 && (!p2 || p1 < p2)) {
            char next = p1[1 + tlen];
            if (next == '>' || next == ' ' || next == '/' || next == '\t' || next == '\r' || next == '\n') {
                p = p1;
                is_esc = 0;
                break;
            }
            cur = p1 + 1;
        } else {
            const char * after = p2 + 4 + tlen;
            if (*after == ' ' || *after == '/' || *after == '\t' || *after == '\r' || *after == '\n' ||
                strncmp(after, "&gt;", 4) == 0) {
                p = p2;
                is_esc = 1;
                break;
            }
            cur = p2 + 1;
        }
    }
    if (!p) return 0;

    if (is_esc) {
        p = strstr(p, "&gt;");
        if (!p) return 0;
        p += 4;
    } else {
        p = strchr(p, '>');
        if (!p) return 0;
        p++;
    }

    char close1[64], close2[64];
    snprintf(close1, sizeof close1, "</%s>", tag);
    snprintf(close2, sizeof close2, "&lt;/%s&gt;", tag);

    const char * end = is_esc ? strstr(p, close2) : strstr(p, close1);
    if (!end) end = is_esc ? strstr(p, close1) : strstr(p, close2);
    if (!end) return 0;

    size_t o = 0;
    while (p < end && o + 1 < outsz) {
        if (*p == '&') {
            if (strncmp(p, "&amp;", 5) == 0) { out[o++] = '&'; p += 5; }
            else if (strncmp(p, "&lt;", 4) == 0) { out[o++] = '<'; p += 4; }
            else if (strncmp(p, "&gt;", 4) == 0) { out[o++] = '>'; p += 4; }
            else if (strncmp(p, "&quot;", 6) == 0) { out[o++] = '"'; p += 6; }
            else if (strncmp(p, "&#39;", 5) == 0 || strncmp(p, "&apos;", 6) == 0) {
                out[o++] = '\'';
                p += (p[1] == '#' ? 5 : 6);
            } else {
                out[o++] = *p++;
            }
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;

    /* Second unescape pass for doubly-nested DIDL/XML entities */
    if (strstr(out, "&amp;") || strstr(out, "&quot;") || strstr(out, "&#39;")) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", out);
        size_t to = 0;
        const char * tp = tmp;
        while (*tp && to + 1 < outsz) {
            if (*tp == '&') {
                if (strncmp(tp, "&amp;", 5) == 0) { out[to++] = '&'; tp += 5; }
                else if (strncmp(tp, "&lt;", 4) == 0) { out[to++] = '<'; tp += 4; }
                else if (strncmp(tp, "&gt;", 4) == 0) { out[to++] = '>'; tp += 4; }
                else if (strncmp(tp, "&quot;", 6) == 0) { out[to++] = '"'; tp += 6; }
                else if (strncmp(tp, "&#39;", 5) == 0 || strncmp(tp, "&apos;", 6) == 0) {
                    out[to++] = '\'';
                    tp += (tp[1] == '#' ? 5 : 6);
                } else {
                    out[to++] = *tp++;
                }
            } else {
                out[to++] = *tp++;
            }
        }
        out[to] = 0;
    }

    return (o > 0);
}

/* Extracts attr="value" or attr=&quot;value&quot; */
static int extract_xml_attr(const char * xml, const char * attr, char * out, size_t outsz) {
    if (!xml || !attr || !out || outsz == 0) return 0;
    out[0] = 0;

    char needle1[64], needle2[64];
    snprintf(needle1, sizeof needle1, "%s=\"", attr);
    snprintf(needle2, sizeof needle2, "%s=&quot;", attr);

    const char * p = strstr(xml, needle1);
    size_t skip = strlen(needle1);
    const char * close_quote = "\"";
    if (!p) {
        p = strstr(xml, needle2);
        skip = strlen(needle2);
        close_quote = "&quot;";
    }
    if (!p) return 0;
    p += skip;

    const char * end = strstr(p, close_quote);
    if (!end) return 0;

    size_t len = (size_t)(end - p);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return 1;
}

static void xml_escape(const char * src, char * dst, size_t dst_sz) {
    if (!src || !dst || dst_sz == 0) return;
    size_t o = 0;
    for (const char * p = src; *p && o + 6 < dst_sz; p++) {
        switch (*p) {
            case '&': memcpy(dst + o, "&amp;", 5); o += 5; break;
            case '<': memcpy(dst + o, "&lt;", 4);  o += 4; break;
            case '>': memcpy(dst + o, "&gt;", 4);  o += 4; break;
            case '"': memcpy(dst + o, "&quot;", 6); o += 6; break;
            default:  dst[o++] = *p; break;
        }
    }
    dst[o] = 0;
}

/* =========================================================================
 * Native UPnP SOAP Client
 * ========================================================================= */

static int sonos_soap(const char * ip, const char * control_path,
                      const char * service, const char * action,
                      const char * body_args,
                      char * out_buf, size_t out_sz, int timeout_s) {
    if (!ip || !ip[0] || !control_path || !service || !action) return -1;
    if (out_buf && out_sz > 0) out_buf[0] = 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv;
    tv.tv_sec = timeout_s > 0 ? timeout_s : 3;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(1400);
    sin.sin_addr.s_addr = inet_addr(ip);

    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(fd);
        return -1;
    }

    char body[4096];
    int blen = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:%s xmlns:u=\"%s\">"
        "%s"
        "</u:%s>"
        "</s:Body>"
        "</s:Envelope>",
        action, service, body_args ? body_args : "", action);

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:1400\r\n"
        "Content-Type: text/xml; charset=\"utf-8\"\r\n"
        "Content-Length: %d\r\n"
        "SOAPACTION: \"%s#%s\"\r\n"
        "Connection: close\r\n"
        "\r\n",
        control_path, ip, blen, service, action);

    if (send(fd, header, hlen, 0) != hlen || send(fd, body, blen, 0) != blen) {
        close(fd);
        return -1;
    }

    if (!out_buf || out_sz == 0) {
        close(fd);
        return 0;
    }

    size_t total = 0;
    while (total + 1 < out_sz) {
        ssize_t r = recv(fd, out_buf + total, out_sz - 1 - total, 0);
        if (r <= 0) break;
        total += r;
    }
    out_buf[total] = 0;
    close(fd);

    if (strstr(out_buf, "HTTP/1.1 200 OK") || strstr(out_buf, "HTTP/1.0 200 OK")) {
        return 0;
    }
    return -2;
}

/* =========================================================================
 * SSDP Auto-Discovery (M-SEARCH multicast)
 * ========================================================================= */

static int sonos_ssdp_discover(char * out_ip, size_t out_sz, int timeout_ms) {
    if (!out_ip || out_sz < 16) return 0;
    out_ip[0] = 0;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int ttl = 4;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(1900);
    dst.sin_addr.s_addr = inet_addr("239.255.255.250");

    const char * msg =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n"
        "\r\n";

    if (sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        close(fd);
        return 0;
    }

    char buf[2048];
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    int found = 0;

    while (1) {
        slen = sizeof(src);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &slen);
        if (n <= 0) break;
        buf[n] = 0;

        if (strstr(buf, "ZonePlayer") || strstr(buf, "Sonos") || strstr(buf, ":1400")) {
            char * loc = strstr(buf, "LOCATION:");
            if (!loc) loc = strstr(buf, "location:");
            if (loc) {
                char * hip = strstr(loc, "http://");
                if (hip) {
                    hip += 7;
                    char * colon = strchr(hip, ':');
                    char * slash = strchr(hip, '/');
                    char * end = colon ? colon : slash;
                    if (end && (size_t)(end - hip) < out_sz) {
                        size_t iplen = end - hip;
                        memcpy(out_ip, hip, iplen);
                        out_ip[iplen] = 0;
                        found = 1;
                        break;
                    }
                }
            }
            if (!found && src.sin_addr.s_addr != 0) {
                const char * ip_str = inet_ntoa(src.sin_addr);
                if (ip_str && strlen(ip_str) < out_sz) {
                    snprintf(out_ip, out_sz, "%s", ip_str);
                    found = 1;
                    break;
                }
            }
        }
    }

    close(fd);
    return found;
}

/* =========================================================================
 * Topology Discovery & Room Resolution
 * ========================================================================= */

static int update_topology(const char * bootstrap_ip) {
    if (!bootstrap_ip || !bootstrap_ip[0]) return 0;

    char * resp = malloc(65536);
    if (!resp) return 0;
    resp[0] = 0;

    int rc = sonos_soap(bootstrap_ip, "/ZoneGroupTopology/Control",
                        "urn:schemas-upnp-org:service:ZoneGroupTopology:1",
                        "GetZoneGroupState", "", resp, 65536, 3);
    if (rc != 0 || resp[0] == 0) {
        free(resp);
        return 0;
    }

    g_room_count = 0;

    const char * zg = resp;
    while ((zg = strstr(zg, "Coordinator=")) != NULL && g_room_count < SONOS_MAX_ZONES) {
        char coord_uuid[48] = "";
        extract_xml_attr(zg, "Coordinator", coord_uuid, sizeof coord_uuid);
        char * c = strchr(coord_uuid, ':'); if (c) *c = 0;

        const char * zg_end = strstr(zg, "</ZoneGroup>");
        if (!zg_end) zg_end = strstr(zg, "&lt;/ZoneGroup&gt;");
        if (!zg_end) break;

        const char * mem = zg;
        while ((mem = strstr(mem, "ZoneGroupMember")) != NULL && mem < zg_end) {
            char zname[48] = "";
            char loc[128] = "";
            char uuid[48] = "";

            extract_xml_attr(mem, "ZoneName", zname, sizeof zname);
            extract_xml_attr(mem, "Location", loc, sizeof loc);
            extract_xml_attr(mem, "UUID", uuid, sizeof uuid);
            char * u = strchr(uuid, ':'); if (u) *u = 0;

            if (coord_uuid[0] && strcmp(uuid, coord_uuid) == 0 && zname[0] && loc[0]) {
                char member_ip[32] = "";
                const char * hip = strstr(loc, "http://");
                if (hip) {
                    hip += 7;
                    const char * colon = strchr(hip, ':');
                    if (colon && (size_t)(colon - hip) < sizeof member_ip) {
                        memcpy(member_ip, hip, colon - hip);
                        member_ip[colon - hip] = 0;
                    }
                }
                if (member_ip[0]) {
                    int dup = 0;
                    for (int i = 0; i < g_room_count; i++) {
                        if (strcmp(g_rooms[i].room, zname) == 0) { dup = 1; break; }
                    }
                    if (!dup) {
                        snprintf(g_rooms[g_room_count].room, sizeof g_rooms[0].room, "%s", zname);
                        snprintf(g_rooms[g_room_count].ip, sizeof g_rooms[0].ip, "%s", member_ip);
                        snprintf(g_rooms[g_room_count].uuid, sizeof g_rooms[0].uuid, "%s", uuid);
                        g_room_count++;
                    }
                }
            }
            mem += 15;
        }
        zg = zg_end;
    }
    free(resp);

    /* Update active coordinator IP */
    g_active_ip[0] = 0;
    if (g_room_count > 0) {
        for (int i = 0; i < g_room_count; i++) {
            if (settings.sonos_zone[0] && strcasecmp(settings.sonos_zone, g_rooms[i].room) == 0) {
                snprintf(g_active_ip, sizeof g_active_ip, "%s", g_rooms[i].ip);
                break;
            }
        }
        if (!g_active_ip[0]) {
            /* Default to first room */
            snprintf(g_active_ip, sizeof g_active_ip, "%s", g_rooms[0].ip);
            if (!settings.sonos_zone[0]) {
                snprintf(settings.sonos_zone, sizeof settings.sonos_zone, "%s", g_rooms[0].room);
            }
        }
        if (g_active_ip[0]) {
            snprintf(settings.sonos_host, sizeof settings.sonos_host, "%s", g_active_ip);
        }
    }
    return g_room_count;
}

/* =========================================================================
 * Fast TCP Port 1400 Subnet Scan (Bypasses local UDP firewall restrictions)
 * ========================================================================= */

static int get_local_subnet_prefix(char * out_prefix, size_t out_sz) {
    if (!out_prefix || out_sz < 16) return -1;
    out_prefix[0] = 0;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        struct sockaddr_in probe;
        memset(&probe, 0, sizeof(probe));
        probe.sin_family = AF_INET;
        probe.sin_port = htons(53);
        probe.sin_addr.s_addr = inet_addr("1.1.1.1");
        if (connect(fd, (struct sockaddr *)&probe, sizeof(probe)) == 0) {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            if (getsockname(fd, (struct sockaddr *)&local, &len) == 0) {
                const char * ip = inet_ntoa(local.sin_addr);
                if (ip && strncmp(ip, "127.", 4) != 0 && strcmp(ip, "0.0.0.0") != 0) {
                    const char * last_dot = strrchr(ip, '.');
                    if (last_dot && (size_t)(last_dot - ip) < out_sz) {
                        size_t n = last_dot - ip;
                        memcpy(out_prefix, ip, n);
                        out_prefix[n] = 0;
                        close(fd);
                        return 0;
                    }
                }
            }
        }
        close(fd);
    }
    snprintf(out_prefix, out_sz, "192.168.2");
    return 0;
}

#define TCP_SCAN_BATCH 64

static int sonos_tcp_discover(char * out_ip, size_t out_sz) {
    if (!out_ip || out_sz < 16) return 0;
    out_ip[0] = 0;

    char prefix[32] = "";
    get_local_subnet_prefix(prefix, sizeof prefix);
    if (!prefix[0]) snprintf(prefix, sizeof prefix, "192.168.2");

    int fds[TCP_SCAN_BATCH];
    char ips[TCP_SCAN_BATCH][32];

    for (int start_host = 1; start_host <= 254; start_host += TCP_SCAN_BATCH) {
        int end_host = start_host + TCP_SCAN_BATCH - 1;
        if (end_host > 254) end_host = 254;
        int count = end_host - start_host + 1;

        int max_fd = -1;
        fd_set wfds;
        FD_ZERO(&wfds);

        for (int i = 0; i < count; i++) {
            fds[i] = -1;
            int host_num = start_host + i;
            snprintf(ips[i], sizeof ips[i], "%s.%d", prefix, host_num);

            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;

            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(1400);
            sin.sin_addr.s_addr = inet_addr(ips[i]);

            connect(fd, (struct sockaddr *)&sin, sizeof(sin));
            fds[i] = fd;
            FD_SET(fd, &wfds);
            if (fd > max_fd) max_fd = fd;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 300000; /* 300 ms */

        char candidates[8][32];
        int num_cand = 0;

        if (max_fd >= 0) {
            int sel = select(max_fd + 1, NULL, &wfds, NULL, &tv);
            if (sel > 0) {
                for (int i = 0; i < count && num_cand < 8; i++) {
                    if (fds[i] >= 0 && FD_ISSET(fds[i], &wfds)) {
                        int err = 0;
                        socklen_t elen = sizeof(err);
                        if (getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) {
                            snprintf(candidates[num_cand++], 32, "%s", ips[i]);
                        }
                    }
                }
            }
        }

        /* Close all sockets in this batch */
        for (int i = 0; i < count; i++) {
            if (fds[i] >= 0) close(fds[i]);
        }

        /* For each host that opened port 1400, test Sonos topology */
        for (int c = 0; c < num_cand; c++) {
            if (update_topology(candidates[c]) > 0) {
                snprintf(out_ip, out_sz, "%s", candidates[c]);
                return 1;
            }
        }
    }
    return 0;
}

int sonos_probe(char zones[][48], int max_zones) {
    if (g_bootstrap_ip[0]) {
        if (update_topology(g_bootstrap_ip) == 0) {
            g_bootstrap_ip[0] = 0;
        }
    }
    if (!g_bootstrap_ip[0]) {
        /* Primary: Fast TCP subnet probe on port 1400 */
        sonos_tcp_discover(g_bootstrap_ip, sizeof g_bootstrap_ip);

        /* Secondary: SSDP discovery fallback */
        if (!g_bootstrap_ip[0]) {
            sonos_ssdp_discover(g_bootstrap_ip, sizeof g_bootstrap_ip, 1200);
        }
    }
    if (g_bootstrap_ip[0] && g_room_count == 0) {
        update_topology(g_bootstrap_ip);
    }
    int count = (g_room_count < max_zones) ? g_room_count : max_zones;
    for (int i = 0; i < count; i++) {
        snprintf(zones[i], 48, "%s", g_rooms[i].room);
    }
    return count;
}

/* =========================================================================
 * Artwork Decoding & In-Memory Downscaling
 * ========================================================================= */

typedef struct {
    char url[256];
} art_req_t;

typedef struct {
    const uint8_t * data;
    size_t size;
    size_t pos;
    uint8_t * rgb_buf;
    int w, h;
} jdec_session_t;

static size_t jdec_infunc(JDEC * jd, uint8_t * buff, size_t ndata) {
    jdec_session_t * s = (jdec_session_t *)jd->device;
    size_t left = s->size > s->pos ? s->size - s->pos : 0;
    size_t to_read = ndata < left ? ndata : left;
    if (buff && to_read > 0) {
        memcpy(buff, s->data + s->pos, to_read);
    }
    s->pos += to_read;
    return to_read;
}

static int jdec_outfunc(JDEC * jd, void * bitmap, JRECT * rect) {
    jdec_session_t * s = (jdec_session_t *)jd->device;
    uint8_t * src = (uint8_t *)bitmap;
    int rw = rect->right - rect->left + 1;
    for (int y = rect->top; y <= rect->bottom; y++) {
        if (y < s->h) {
            int copy_w = rw;
            if (rect->left + copy_w > s->w) copy_w = s->w - rect->left;
            if (copy_w > 0 && rect->left < s->w) {
                uint8_t * dst = s->rgb_buf + (y * s->w + rect->left) * 3;
                memcpy(dst, src, copy_w * 3);
            }
        }
        src += rw * 3;
    }
    return 1;
}

static void resample_to_lv_color(const uint8_t * src, int src_w, int src_h, int channels,
                                 lv_color_t * dst, int dst_w, int dst_h) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    int src_stride = src_w * channels;

    int crop_x = 0, crop_y = 0, crop_w = src_w, crop_h = src_h;
    if (src_w > src_h) {
        crop_w = src_h;
        crop_x = (src_w - src_h) / 2;
    } else if (src_h > src_w) {
        crop_h = src_w;
        crop_y = (src_h - src_w) / 2;
    }

    for (int dy = 0; dy < dst_h; dy++) {
        int64_t sy16 = ((int64_t)dy * crop_h * 65536) / dst_h;
        int y0 = crop_y + (int)(sy16 >> 16);
        if (y0 < 0) y0 = 0;
        if (y0 >= src_h) y0 = src_h - 1;
        int y1 = (y0 + 1 < crop_y + crop_h && y0 + 1 < src_h) ? y0 + 1 : y0;
        int y_frac = (sy16 >> 8) & 0xff;

        for (int dx = 0; dx < dst_w; dx++) {
            int64_t sx16 = ((int64_t)dx * crop_w * 65536) / dst_w;
            int x0 = crop_x + (int)(sx16 >> 16);
            if (x0 < 0) x0 = 0;
            if (x0 >= src_w) x0 = src_w - 1;
            int x1 = (x0 + 1 < crop_x + crop_w && x0 + 1 < src_w) ? x0 + 1 : x0;
            int x_frac = (sx16 >> 8) & 0xff;

            int w00 = (256 - x_frac) * (256 - y_frac);
            int w10 = x_frac * (256 - y_frac);
            int w01 = (256 - x_frac) * y_frac;
            int w11 = x_frac * y_frac;

            const uint8_t * p00 = src + y0 * src_stride + x0 * channels;
            const uint8_t * p10 = src + y0 * src_stride + x1 * channels;
            const uint8_t * p01 = src + y1 * src_stride + x0 * channels;
            const uint8_t * p11 = src + y1 * src_stride + x1 * channels;

            uint8_t r = (p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11 + 32768) >> 16;
            uint8_t g = (p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11 + 32768) >> 16;
            uint8_t b = (p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11 + 32768) >> 16;

            dst[dy * dst_w + dx] = lv_color_make(r, g, b);
        }
    }
}

static void * art_download_worker(void * arg) {
    art_req_t * req = (art_req_t *)arg;
    char tmp[64] = "/tmp/sonos_art_dl.tmp";
    clean_url(req->url);

    char safe_url[512];
    size_t si = 0;
    for (size_t i = 0; req->url[i] && si + 4 < sizeof safe_url; i++) {
        if (req->url[i] == '\'') {
            safe_url[si++] = '%'; safe_url[si++] = '2'; safe_url[si++] = '7';
        } else {
            safe_url[si++] = req->url[i];
        }
    }
    safe_url[si] = 0;

    unlink(tmp);
    char cmd[640];
    snprintf(cmd, sizeof cmd, "curl -s -L --connect-timeout 3 --max-time 6 -o %s '%s' 2>/dev/null", tmp, safe_url);
    int rc = system(cmd);
    bool ok = false;
    lv_color_t * tmp_big = malloc(sizeof(g_art_big_buf));
    lv_color_t * tmp_tile = malloc(sizeof(g_art_tile_buf));

    if (rc == 0 && tmp_big && tmp_tile) {
        FILE * f = fopen(tmp, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long fsz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (fsz > 4 && fsz < 5 * 1024 * 1024) {
                uint8_t * fbuf = malloc(fsz);
                if (fbuf) {
                    if (fread(fbuf, 1, fsz, f) == (size_t)fsz) {
                        if (fbuf[0] == 0xFF && fbuf[1] == 0xD8) {
                            /* JPEG */
                            void * work = malloc(32768);
                            if (work) {
                                jdec_session_t s = { .data = fbuf, .size = (size_t)fsz, .pos = 0, .rgb_buf = NULL, .w = 0, .h = 0 };
                                JDEC jd;
                                if (jd_prepare(&jd, jdec_infunc, work, 32768, &s) == JDR_OK) {
                                    s.w = jd.width;
                                    s.h = jd.height;
                                    s.rgb_buf = malloc(s.w * s.h * 3);
                                    if (s.rgb_buf) {
                                        if (jd_decomp(&jd, jdec_outfunc, 0) == JDR_OK) {
                                            resample_to_lv_color(s.rgb_buf, s.w, s.h, 3, tmp_big, ART_BIG_DIM, ART_BIG_DIM);
                                            resample_to_lv_color(s.rgb_buf, s.w, s.h, 3, tmp_tile, ART_TILE_DIM, ART_TILE_DIM);
                                            ok = true;
                                        }
                                        free(s.rgb_buf);
                                    }
                                }
                                free(work);
                            }
                        } else if (fbuf[0] == 0x89 && fbuf[1] == 'P' && fbuf[2] == 'N' && fbuf[3] == 'G') {
                            /* PNG */
                            uint8_t * img = NULL;
                            unsigned pw = 0, ph = 0;
                            if (lodepng_decode24(&img, &pw, &ph, fbuf, fsz) == 0 && img) {
                                resample_to_lv_color(img, (int)pw, (int)ph, 3, tmp_big, ART_BIG_DIM, ART_BIG_DIM);
                                resample_to_lv_color(img, (int)pw, (int)ph, 3, tmp_tile, ART_TILE_DIM, ART_TILE_DIM);
                                free(img);
                                ok = true;
                            }
                        }
                    }
                    free(fbuf);
                }
            }
            fclose(f);
        }
        unlink(tmp);
    }

    pthread_mutex_lock(&g_sonos_mtx);
    if (ok) {
        memcpy(g_art_big_buf, tmp_big, sizeof(g_art_big_buf));
        memcpy(g_art_tile_buf, tmp_tile, sizeof(g_art_tile_buf));
        g_art_has_image = 1;
        g_art_version++;
    } else {
        g_art_has_image = 0;
        g_art_version++;
    }
    g_art_downloading = 0;
    pthread_mutex_unlock(&g_sonos_mtx);

    free(tmp_big);
    free(tmp_tile);
    free(req);
    return NULL;
}

static void check_trigger_art_download(const char * new_url) {
    if (!new_url || !new_url[0]) {
        pthread_mutex_lock(&g_sonos_mtx);
        if (g_art_has_image) {
            g_art_has_image = 0;
            g_cur_art_url[0] = 0;
            g_art_version++;
        }
        pthread_mutex_unlock(&g_sonos_mtx);
        return;
    }

    pthread_mutex_lock(&g_sonos_mtx);
    if (strcmp(g_cur_art_url, new_url) == 0 || g_art_downloading) {
        pthread_mutex_unlock(&g_sonos_mtx);
        return;
    }
    snprintf(g_cur_art_url, sizeof g_cur_art_url, "%s", new_url);
    g_art_downloading = 1;
    pthread_mutex_unlock(&g_sonos_mtx);

    art_req_t * req = malloc(sizeof *req);
    if (!req) {
        pthread_mutex_lock(&g_sonos_mtx);
        g_art_downloading = 0;
        pthread_mutex_unlock(&g_sonos_mtx);
        return;
    }
    snprintf(req->url, sizeof req->url, "%s", new_url);

    pthread_t tid;
    if (pthread_create(&tid, NULL, art_download_worker, req) == 0) {
        pthread_detach(tid);
    } else {
        free(req);
        pthread_mutex_lock(&g_sonos_mtx);
        g_art_downloading = 0;
        pthread_mutex_unlock(&g_sonos_mtx);
    }
}

/* =========================================================================
 * Background State Polling
 * ========================================================================= */

static void poll_state(void) {
    if (!settings.sonos_enabled) {
        pthread_mutex_lock(&g_sonos_mtx);
        sonos_state.connected = 0;
        pthread_mutex_unlock(&g_sonos_mtx);
        return;
    }

    /* Bootstrap discovery if active IP unknown */
    if (!g_active_ip[0]) {
        if (!g_bootstrap_ip[0]) {
            sonos_tcp_discover(g_bootstrap_ip, sizeof g_bootstrap_ip);
            if (!g_bootstrap_ip[0]) {
                sonos_ssdp_discover(g_bootstrap_ip, sizeof g_bootstrap_ip, 1200);
            }
        }
        if (g_bootstrap_ip[0]) {
            update_topology(g_bootstrap_ip);
        }
        if (!g_active_ip[0]) {
            pthread_mutex_lock(&g_sonos_mtx);
            sonos_state.connected = 0;
            pthread_mutex_unlock(&g_sonos_mtx);
            return;
        }
    }

    char * buf = malloc(32768);
    if (!buf) return;

    sonos_state_t st = {0};
    st.connected = 1;
    snprintf(st.zone, sizeof st.zone, "%s", settings.sonos_zone[0] ? settings.sonos_zone : "Sonos");

    /* 1. GetTransportInfo */
    buf[0] = 0;
    int rc = sonos_soap(g_active_ip, "/MediaRenderer/AVTransport/Control",
                        "urn:schemas-upnp-org:service:AVTransport:1",
                        "GetTransportInfo", "<InstanceID>0</InstanceID>", buf, 32768, 3);
    if (rc == 0) {
        g_poll_fail_count = 0;
        char state[32] = "";
        extract_xml_tag(buf, "CurrentTransportState", state, sizeof state);
        st.playing = (strcmp(state, "PLAYING") == 0 || strcmp(state, "TRANSITIONING") == 0);
    } else {
        /* Absorb single network hiccups with a 3-consecutive-failure threshold */
        g_poll_fail_count++;
        if (g_poll_fail_count >= 3) {
            g_active_ip[0] = 0;
            g_bootstrap_ip[0] = 0;
            pthread_mutex_lock(&g_sonos_mtx);
            sonos_state.connected = 0;
            pthread_mutex_unlock(&g_sonos_mtx);
        }
        free(buf);
        return;
    }

    /* 2. GetPositionInfo */
    buf[0] = 0;
    rc = sonos_soap(g_active_ip, "/MediaRenderer/AVTransport/Control",
                    "urn:schemas-upnp-org:service:AVTransport:1",
                    "GetPositionInfo", "<InstanceID>0</InstanceID>", buf, 32768, 3);
    if (rc == 0) {
        char dur_str[32] = "", rel_str[32] = "", track_uri[256] = "";
        extract_xml_tag(buf, "TrackDuration", dur_str, sizeof dur_str);
        extract_xml_tag(buf, "RelTime", rel_str, sizeof rel_str);
        extract_xml_tag(buf, "TrackURI", track_uri, sizeof track_uri);

        st.duration = parse_time_seconds(dur_str);
        st.elapsed  = parse_time_seconds(rel_str);

        if (strstr(track_uri, "x-sonosapi-stream:") || strstr(track_uri, "x-rincon-mp3radio:") ||
            strstr(track_uri, "hls-radio:") || strstr(track_uri, "aac://") || strstr(track_uri, "http://")) {
            st.is_radio = 1;
        }

        /* Extract DIDL metadata */
        char title[96] = "", artist[64] = "", album[64] = "", stream[96] = "", art[256] = "";
        extract_xml_tag(buf, "dc:title", title, sizeof title);
        extract_xml_tag(buf, "dc:creator", artist, sizeof artist);
        if (!artist[0]) extract_xml_tag(buf, "upnp:artist", artist, sizeof artist);
        extract_xml_tag(buf, "upnp:album", album, sizeof album);
        extract_xml_tag(buf, "r:streamContent", stream, sizeof stream);
        extract_xml_tag(buf, "upnp:albumArtURI", art, sizeof art);

        if (st.is_radio && stream[0]) {
            /* Radio stream with currently playing song info */
            if (title[0]) snprintf(st.album, sizeof st.album, "%s", title);
            snprintf(st.title, sizeof st.title, "%s", stream);
            snprintf(st.artist, sizeof st.artist, "%s", artist[0] ? artist : title);
        } else {
            snprintf(st.title, sizeof st.title, "%s", title);
            snprintf(st.artist, sizeof st.artist, "%s", artist);
            snprintf(st.album, sizeof st.album, "%s", album);
        }

        if (art[0]) {
            if (art[0] == '/') {
                snprintf(st.art_url, sizeof st.art_url, "http://%s:1400%s", g_active_ip, art);
            } else {
                snprintf(st.art_url, sizeof st.art_url, "%s", art);
            }
            clean_url(st.art_url);
        }
    }

    /* 3. GetVolume */
    buf[0] = 0;
    rc = sonos_soap(g_active_ip, "/MediaRenderer/RenderingControl/Control",
                    "urn:schemas-upnp-org:service:RenderingControl:1",
                    "GetVolume", "<InstanceID>0</InstanceID><Channel>Master</Channel>", buf, 4096, 2);
    if (rc == 0) {
        char vol_str[16] = "";
        extract_xml_tag(buf, "CurrentVolume", vol_str, sizeof vol_str);
        if (vol_str[0]) st.volume = atoi(vol_str);
    }

    free(buf);

    pthread_mutex_lock(&g_sonos_mtx);
    sonos_state = st;
    pthread_mutex_unlock(&g_sonos_mtx);

    check_trigger_art_download(st.art_url);
}

static void * sonos_worker(void * arg) {
    (void)arg;
    while (1) {
        poll_state();
        for (int i = 0; i < 20; i++) {
            if (g_force_poll) {
                g_force_poll = 0;
                break;
            }
            usleep(100000); /* 100 ms slices = 2 s total */
        }
    }
    return NULL;
}

int sonos_start(void) {
    if (g_sonos_tid) return 0;
    if (pthread_create(&g_sonos_tid, NULL, sonos_worker, NULL) != 0) {
        g_sonos_tid = 0;
        return -1;
    }
    pthread_detach(g_sonos_tid);
    return 0;
}

/* =========================================================================
 * Async Control Commands
 * ========================================================================= */

typedef struct {
    char service[64];
    char control_path[64];
    char action[32];
    char body[512];
} soap_cmd_t;

static void * soap_cmd_worker(void * arg) {
    soap_cmd_t * c = (soap_cmd_t *)arg;
    if (g_active_ip[0]) {
        sonos_soap(g_active_ip, c->control_path, c->service, c->action, c->body, NULL, 0, 2);
    }
    free(c);
    usleep(100000);
    g_force_poll = 1;
    return NULL;
}

static void send_upnp_cmd(const char * ctrl, const char * svc, const char * act, const char * body) {
    if (!settings.sonos_enabled || !g_active_ip[0]) return;
    soap_cmd_t * c = malloc(sizeof *c);
    if (!c) return;
    snprintf(c->control_path, sizeof c->control_path, "%s", ctrl);
    snprintf(c->service, sizeof c->service, "%s", svc);
    snprintf(c->action, sizeof c->action, "%s", act);
    snprintf(c->body, sizeof c->body, "%s", body ? body : "");

    pthread_t t;
    if (pthread_create(&t, NULL, soap_cmd_worker, c) == 0) {
        pthread_detach(t);
    } else {
        free(c);
    }
}

void sonos_play(void) {
    pthread_mutex_lock(&g_sonos_mtx);
    sonos_state.playing = 1;
    pthread_mutex_unlock(&g_sonos_mtx);
    send_upnp_cmd("/MediaRenderer/AVTransport/Control",
                  "urn:schemas-upnp-org:service:AVTransport:1",
                  "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>");
}

void sonos_pause(void) {
    pthread_mutex_lock(&g_sonos_mtx);
    sonos_state.playing = 0;
    pthread_mutex_unlock(&g_sonos_mtx);
    send_upnp_cmd("/MediaRenderer/AVTransport/Control",
                  "urn:schemas-upnp-org:service:AVTransport:1",
                  "Pause", "<InstanceID>0</InstanceID>");
}

void sonos_play_pause(void) {
    int p;
    pthread_mutex_lock(&g_sonos_mtx);
    p = sonos_state.playing;
    sonos_state.playing = !p;
    pthread_mutex_unlock(&g_sonos_mtx);
    if (p) sonos_pause();
    else sonos_play();
}

void sonos_next(void) {
    send_upnp_cmd("/MediaRenderer/AVTransport/Control",
                  "urn:schemas-upnp-org:service:AVTransport:1",
                  "Next", "<InstanceID>0</InstanceID>");
}

void sonos_prev(void) {
    send_upnp_cmd("/MediaRenderer/AVTransport/Control",
                  "urn:schemas-upnp-org:service:AVTransport:1",
                  "Previous", "<InstanceID>0</InstanceID>");
}

void sonos_seek(int seconds) {
    if (seconds < 0) seconds = 0;
    pthread_mutex_lock(&g_sonos_mtx);
    sonos_state.elapsed = seconds;
    pthread_mutex_unlock(&g_sonos_mtx);

    char target[32];
    snprintf(target, sizeof target, "%02d:%02d:%02d", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
    char body[128];
    snprintf(body, sizeof body, "<InstanceID>0</InstanceID><Unit>REL_TIME</Unit><Target>%s</Target>", target);

    send_upnp_cmd("/MediaRenderer/AVTransport/Control",
                  "urn:schemas-upnp-org:service:AVTransport:1",
                  "Seek", body);
}

void sonos_volume_up(void) {
    pthread_mutex_lock(&g_sonos_mtx);
    if (sonos_state.volume <= 98) sonos_state.volume += 2;
    pthread_mutex_unlock(&g_sonos_mtx);
    send_upnp_cmd("/MediaRenderer/RenderingControl/Control",
                  "urn:schemas-upnp-org:service:RenderingControl:1",
                  "SetRelativeVolume",
                  "<InstanceID>0</InstanceID><Channel>Master</Channel><Adjustment>+2</Adjustment>");
}

void sonos_volume_down(void) {
    pthread_mutex_lock(&g_sonos_mtx);
    if (sonos_state.volume >= 2) sonos_state.volume -= 2;
    pthread_mutex_unlock(&g_sonos_mtx);
    send_upnp_cmd("/MediaRenderer/RenderingControl/Control",
                  "urn:schemas-upnp-org:service:RenderingControl:1",
                  "SetRelativeVolume",
                  "<InstanceID>0</InstanceID><Channel>Master</Channel><Adjustment>-2</Adjustment>");
}

void sonos_set_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    pthread_mutex_lock(&g_sonos_mtx);
    sonos_state.volume = vol;
    pthread_mutex_unlock(&g_sonos_mtx);

    char body[96];
    snprintf(body, sizeof body,
             "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>%d</DesiredVolume>", vol);
    send_upnp_cmd("/MediaRenderer/RenderingControl/Control",
                  "urn:schemas-upnp-org:service:RenderingControl:1",
                  "SetVolume", body);
}

void sonos_set_zone(const char * zone) {
    if (!zone || !zone[0]) return;
    snprintf(settings.sonos_zone, sizeof settings.sonos_zone, "%s", zone);
    settings_save();

    /* Switch active IP matching this room */
    for (int i = 0; i < g_room_count; i++) {
        if (strcasecmp(g_rooms[i].room, zone) == 0) {
            snprintf(g_active_ip, sizeof g_active_ip, "%s", g_rooms[i].ip);
            break;
        }
    }
    g_force_poll = 1;
}

/* =========================================================================
 * Sonos Favorites & Playlists (ContentDirectory)
 * ========================================================================= */

int sonos_get_favorites(char favs[][64], int max_favs) {
    if (!g_active_ip[0] && g_bootstrap_ip[0]) {
        update_topology(g_bootstrap_ip);
    }
    if (!g_active_ip[0] || max_favs <= 0) return 0;

    char * buf = malloc(65536);
    if (!buf) return 0;
    buf[0] = 0;

    char args[256];
    snprintf(args, sizeof args,
        "<ObjectID>FV:2</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag>"
        "<Filter>*</Filter><StartingIndex>0</StartingIndex><RequestedCount>%d</RequestedCount><SortCriteria></SortCriteria>",
        max_favs);

    int rc = sonos_soap(g_active_ip, "/MediaServer/ContentDirectory/Control",
                        "urn:schemas-upnp-org:service:ContentDirectory:1",
                        "Browse", args, buf, 65536, 3);
    if (rc != 0 || buf[0] == 0) {
        free(buf);
        return 0;
    }

    g_fav_count = 0;
    const char * cur = buf;
    while (cur) {
        const char * p1 = strstr(cur, "<item");
        const char * p2 = strstr(cur, "&lt;item");
        if (!p1 && !p2) break;
        cur = (p1 && p2) ? (p1 < p2 ? p1 : p2) : (p1 ? p1 : p2);
        if (g_fav_count >= max_favs || g_fav_count >= SONOS_MAX_ITEMS) break;

        const char * end_item = strstr(cur, "</item>");
        if (!end_item) end_item = strstr(cur, "&lt;/item&gt;");
        if (!end_item) break;

        char title[64] = "";
        char uri[256] = "";
        char resMD[512] = "";

        extract_xml_tag(cur, "dc:title", title, sizeof title);
        extract_xml_tag(cur, "res", uri, sizeof uri);
        extract_xml_tag(cur, "r:resMD", resMD, sizeof resMD);

        if (title[0] && uri[0]) {
            snprintf(g_favs[g_fav_count].title, sizeof g_favs[0].title, "%s", title);
            snprintf(g_favs[g_fav_count].uri, sizeof g_favs[0].uri, "%s", uri);
            snprintf(g_favs[g_fav_count].meta, sizeof g_favs[0].meta, "%s", resMD);
            snprintf(favs[g_fav_count], 64, "%s", title);
            g_fav_count++;
        }
        cur = end_item + 7;
    }

    free(buf);
    return g_fav_count;
}

static int get_sonos_playlists(sonos_item_t items[], int max_items) {
    if (!g_active_ip[0] || max_items <= 0) return 0;

    char * buf = malloc(65536);
    if (!buf) return 0;
    buf[0] = 0;

    char args[256];
    snprintf(args, sizeof args,
        "<ObjectID>SQ:</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag>"
        "<Filter>*</Filter><StartingIndex>0</StartingIndex><RequestedCount>%d</RequestedCount><SortCriteria></SortCriteria>",
        max_items);

    int rc = sonos_soap(g_active_ip, "/MediaServer/ContentDirectory/Control",
                        "urn:schemas-upnp-org:service:ContentDirectory:1",
                        "Browse", args, buf, 65536, 3);
    if (rc != 0 || buf[0] == 0) {
        free(buf);
        return 0;
    }

    int count = 0;
    const char * cur = buf;
    while (cur) {
        const char * p1 = strstr(cur, "<item");
        const char * p2 = strstr(cur, "&lt;item");
        if (!p1 && !p2) break;
        cur = (p1 && p2) ? (p1 < p2 ? p1 : p2) : (p1 ? p1 : p2);
        if (count >= max_items) break;
        const char * end_item = strstr(cur, "</item>");
        if (!end_item) end_item = strstr(cur, "&lt;/item&gt;");
        if (!end_item) break;

        char title[64] = "", uri[128] = "";
        extract_xml_tag(cur, "dc:title", title, sizeof title);
        extract_xml_tag(cur, "res", uri, sizeof uri);

        if (title[0]) {
            snprintf(items[count].name, sizeof items[0].name, "%s", title);
            snprintf(items[count].uri, sizeof items[0].uri, "%s", uri);
            count++;
        }
        cur = end_item + 7;
    }
    free(buf);
    return count;
}

void sonos_play_favorite(const char * name) {
    if (!name || !name[0] || !g_active_ip[0]) return;

    /* Search in cached favorites */
    const sonos_fav_entry_t * match = NULL;
    for (int i = 0; i < g_fav_count; i++) {
        if (strcmp(g_favs[i].title, name) == 0) {
            match = &g_favs[i];
            break;
        }
    }

    if (!match) return;

    char esc_uri[384], esc_meta[768];
    xml_escape(match->uri, esc_uri, sizeof esc_uri);
    xml_escape(match->meta, esc_meta, sizeof esc_meta);

    char body[1280];
    snprintf(body, sizeof body,
             "<InstanceID>0</InstanceID><CurrentURI>%s</CurrentURI><CurrentURIMetaData>%s</CurrentURIMetaData>",
             esc_uri, esc_meta);

    send_upnp_cmd("/MediaRenderer/AVTransport/Control",
                  "urn:schemas-upnp-org:service:AVTransport:1",
                  "SetAVTransportURI", body);

    usleep(100000);
    sonos_play();
}

void sonos_play_playlist(const char * name) {
    if (!name || !name[0] || !g_active_ip[0]) return;
    /* Browse SQ: for playlist URI and set */
    char enc_name[128];
    xml_escape(name, enc_name, sizeof enc_name);
    char body[384];
    snprintf(body, sizeof body,
             "<InstanceID>0</InstanceID><CurrentURI>file:///jffs/settings/savedqueues.rsq#%s</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>",
             enc_name);
    send_upnp_cmd("/MediaRenderer/AVTransport/Control",
                  "urn:schemas-upnp-org:service:AVTransport:1",
                  "SetAVTransportURI", body);
    usleep(100000);
    sonos_play();
}

void sonos_play_spotify(const char * uri) {
    if (!uri || !uri[0] || !g_active_ip[0]) return;
    char esc[192];
    xml_escape(uri, esc, sizeof esc);
    char body[384];
    snprintf(body, sizeof body,
             "<InstanceID>0</InstanceID><CurrentURI>%s</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>",
             esc);
    send_upnp_cmd("/MediaRenderer/AVTransport/Control",
                  "urn:schemas-upnp-org:service:AVTransport:1",
                  "SetAVTransportURI", body);
    usleep(100000);
    sonos_play();
}

/* =========================================================================
 * Spotify Web API Integration (Optional user playlist listing)
 * ========================================================================= */

static int extract_json_str(const char * src, const char * key, char * out, size_t outsz) {
    if (!src || !key || !out || outsz == 0) return 0;
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char * p = strstr(src, needle);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outsz) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

static int extract_json_int(const char * src, const char * key, int * out) {
    if (!src || !key || !out) return 0;
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char * p = strstr(src, needle);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '"') p++;
    *out = atoi(p);
    return 1;
}

void sonos_get_spotify_config(sonos_spotify_cfg_t * cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    FILE * f = fopen("/mnt/data/tsc/sonos.userSettings.json", "r");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return;
    buf[n] = 0;

    extract_json_str(buf, "spotifyClientId", cfg->client_id, sizeof cfg->client_id);
    extract_json_str(buf, "spotifyClientSecret", cfg->client_secret, sizeof cfg->client_secret);
    extract_json_int(buf, "selectedPlaylistUser", &cfg->selected_user);

    if (cfg->client_id[0] && cfg->client_secret[0]) {
        cfg->configured = 1;
    }

    const char * un = strstr(buf, "\"spotifyUserNames\"");
    if (un) {
        const char * p = strchr(un, '[');
        if (p) {
            p++;
            while (*p && *p != ']' && cfg->user_count < 8) {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
                if (*p == '"') {
                    p++;
                    const char * q = strchr(p, '"');
                    if (!q) break;
                    size_t len = (size_t)(q - p);
                    if (len >= sizeof cfg->user_names[0]) len = sizeof cfg->user_names[0] - 1;
                    memcpy(cfg->user_names[cfg->user_count], p, len);
                    cfg->user_names[cfg->user_count][len] = 0;
                    cfg->user_count++;
                    p = q + 1;
                } else {
                    p++;
                }
            }
        }
    }

    const char * ui = strstr(buf, "\"spotifyUserIDs\"");
    if (ui) {
        const char * p = strchr(ui, '[');
        if (p) {
            p++;
            int idx = 0;
            while (*p && *p != ']' && idx < 8) {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
                if (*p == '"') {
                    p++;
                    const char * q = strchr(p, '"');
                    if (!q) break;
                    size_t len = (size_t)(q - p);
                    if (len >= sizeof cfg->user_ids[0]) len = sizeof cfg->user_ids[0] - 1;
                    memcpy(cfg->user_ids[idx], p, len);
                    cfg->user_ids[idx][len] = 0;
                    idx++;
                    p = q + 1;
                } else {
                    p++;
                }
            }
        }
    }
}

static char   s_spot_token[300] = {0};
static time_t s_spot_token_expiry = 0;

static int get_spotify_token(const sonos_spotify_cfg_t * cfg, char * out, size_t outsz) {
    time_t now = time(NULL);
    if (s_spot_token[0] && now < s_spot_token_expiry - 60) {
        snprintf(out, outsz, "%s", s_spot_token);
        return 0;
    }
    if (!cfg || !cfg->configured) return -1;

    char cmd[512];
    snprintf(cmd, sizeof cmd,
        "/usr/bin/curl -s -4 -k --max-time 6 --connect-timeout 4 "
        "-X POST 'https://accounts.spotify.com/api/token' "
        "-d 'grant_type=client_credentials' -u '%s:%s'",
        cfg->client_id, cfg->client_secret);

    FILE * fp = popen(cmd, "r");
    if (!fp) return -1;
    char resp[1024];
    size_t got = fread(resp, 1, sizeof(resp) - 1, fp);
    pclose(fp);
    resp[got] = 0;

    char tok[300] = "";
    if (extract_json_str(resp, "access_token", tok, sizeof tok) && tok[0]) {
        int exp = 3600;
        extract_json_int(resp, "expires_in", &exp);
        snprintf(s_spot_token, sizeof s_spot_token, "%s", tok);
        s_spot_token_expiry = now + exp;
        snprintf(out, outsz, "%s", tok);
        return 0;
    }
    return -1;
}

static int parse_spotify_playlists(const char * json, sonos_item_t items[], int max_items) {
    if (!json || !items || max_items <= 0) return 0;
    int count = 0;
    const char * cur = json;
    while ((cur = strstr(cur, "\"uri\":\"spotify:playlist:")) != NULL && count < max_items) {
        cur += 7;
        const char * q = strchr(cur, '"');
        if (!q) break;
        size_t ulen = (size_t)(q - cur);
        char uri[128];
        if (ulen >= sizeof uri) ulen = sizeof uri - 1;
        memcpy(uri, cur, ulen);
        uri[ulen] = 0;

        const char * search_start = (cur - 1500 > json) ? cur - 1500 : json;
        const char * nm = cur;
        while (nm > search_start) {
            if (strncmp(nm, "\"name\":\"", 8) == 0) break;
            nm--;
        }
        char name[64] = "";
        if (strncmp(nm, "\"name\":\"", 8) == 0) {
            nm += 8;
            const char * nq = strchr(nm, '"');
            if (nq && nq < cur) {
                size_t nlen = (size_t)(nq - nm);
                if (nlen >= sizeof name) nlen = sizeof name - 1;
                memcpy(name, nm, nlen);
                name[nlen] = 0;
            }
        }
        if (name[0] && uri[0]) {
            snprintf(items[count].name, sizeof items[count].name, "%s", name);
            snprintf(items[count].uri, sizeof items[count].uri, "%s", uri);
            count++;
        }
        cur = q + 1;
    }
    return count;
}

int sonos_get_source_items(sonos_source_t src, int spotify_user_idx, sonos_item_t items[], int max_items) {
    if (!items || max_items <= 0) return 0;
    if (src == SONOS_SRC_FAVORITES) {
        char favs[SONOS_MAX_ITEMS][64];
        int n = sonos_get_favorites(favs, max_items);
        for (int i = 0; i < n; i++) {
            snprintf(items[i].name, sizeof items[i].name, "%s", favs[i]);
            items[i].uri[0] = 0;
        }
        return n;
    } else if (src == SONOS_SRC_PLAYLISTS) {
        return get_sonos_playlists(items, max_items);
    } else if (src == SONOS_SRC_SPOTIFY) {
        sonos_spotify_cfg_t cfg;
        sonos_get_spotify_config(&cfg);
        if (!cfg.configured || spotify_user_idx < 0 || spotify_user_idx >= cfg.user_count)
            return 0;
        char token[300] = "";
        if (get_spotify_token(&cfg, token, sizeof token) != 0 || !token[0])
            return 0;

        char cmd[512];
        snprintf(cmd, sizeof cmd,
            "/usr/bin/curl -s -4 -k --max-time 6 --connect-timeout 4 "
            "-H 'Authorization: Bearer %s' "
            "'https://api.spotify.com/v1/users/%s/playlists'",
            token, cfg.user_ids[spotify_user_idx]);

        FILE * fp = popen(cmd, "r");
        if (!fp) return 0;
        char * resp = malloc(32768);
        if (!resp) { pclose(fp); return 0; }
        size_t got = fread(resp, 1, 32767, fp);
        pclose(fp);
        resp[got] = 0;
        int count = parse_spotify_playlists(resp, items, max_items);
        free(resp);
        return count;
    }
    return 0;
}
