/*
 * Inbox model — parses happ_usermsg UpdateDataSet payloads into an array
 * of inbox_msg_t. Read-state stored locally in /mnt/data/toonui_inbox.txt
 * (one UUID per line) since the BoxTalk protocol has no "read" flag.
 */
#include "inbox.h"
#include "boxtalk.h"
#include "healthcheck.h"
#include "settings.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

inbox_msg_t     inbox_msgs[INBOX_MAX];
volatile int    inbox_count = 0;
volatile int    inbox_unread = 0;

#define READ_FILE "/mnt/data/toonui_inbox.txt"

/* Read-uuid persistence — small list of UUIDs marked as read.
   Loaded into memory at boot; appended-to when the user marks one read. */
#define READ_LIST_MAX 64
static char read_list[READ_LIST_MAX][INBOX_UUID_LEN];
static int  read_list_n = 0;

static int is_in_read_list(const char * uuid) {
    for (int i = 0; i < read_list_n; i++)
        if (strcmp(read_list[i], uuid) == 0) return 1;
    return 0;
}
static void add_to_read_list(const char * uuid) {
    if (is_in_read_list(uuid)) return;
    if (read_list_n >= READ_LIST_MAX) return;
    snprintf(read_list[read_list_n], INBOX_UUID_LEN, "%s", uuid);
    read_list_n++;
}

void inbox_persist(void) {
    FILE * f = fopen(READ_FILE, "w");
    if (!f) return;
    for (int i = 0; i < read_list_n; i++) fprintf(f, "%s\n", read_list[i]);
    fclose(f);
}

/* Extract `<tag>VALUE</tag>` text. Returns 0 if not found. */
static int xml_text(const char * begin, const char * end,
                    const char * tag, char * out, size_t outsz) {
    char open[40], close[40];
    snprintf(open,  sizeof(open),  "<%s>",  tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char * p = strstr(begin, open);
    if (!p || p >= end) { if (outsz) out[0] = 0; return 0; }
    p += strlen(open);
    const char * e = strstr(p, close);
    if (!e || e > end) { if (outsz) out[0] = 0; return 0; }
    size_t len = (size_t)(e - p);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return 1;
}

/* ---- held messages ---------------------------------------------------------
 * The stock network service hcb_netcon posts "<display> is niet verbonden met
 * internet / de router, bekijk hier de oplossingen" as soon as a few of its
 * own pings fail, and deletes it again when they work. Short hiccups thus put
 * a message in the Inbox that is gone (or wrong) by the time you look. So
 * those wait in g_all[] and only reach inbox_msgs[] once they have been there
 * INBOX_NETCON_HOLD_S and FreeToon's own internet check fails as well. */
static inbox_msg_t     g_all[INBOX_MAX];
static long            g_all_seen[INBOX_MAX];   /* when we first saw that uuid */
static int             g_all_n = 0;
static int             g_all_valid = 0;
static int             g_held_logged = -1;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Case-insensitive substring (ASCII). */
static int has_ci(const char * hay, const char * needle) {
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* hcb_netcon's "no connection to the internet / router" notice, in any of the
 * languages the firmware ships (nl, en, de, fr, es). */
static int is_netcon_msg(const inbox_msg_t * m) {
    static const char * const pat[] = {
        "niet verbonden met internet", "niet verbonden met de router",
        "not connected to the internet", "not connected to the router",
        "nicht mit dem internet verbunden", "nicht mit dem router verbunden",
        "pas connect", "no est\xc3\xa1 conectado",
    };
    if (has_ci(m->type, "netcon") || has_ci(m->sub_type, "netcon")) return 1;
    for (size_t i = 0; i < sizeof pat / sizeof pat[0]; i++)
        if (has_ci(m->text, pat[i])) return 1;
    return 0;
}

/* Build inbox_msgs[] from g_all[]. Caller holds g_mtx. */
static void apply_locked(void) {
    long now = (long)time(NULL);
    int down = healthcheck_internet_down();
    int n = 0, unread = 0, held = 0;
    for (int i = 0; i < g_all_n && n < INBOX_MAX; i++) {
        if (is_netcon_msg(&g_all[i]) &&
            !(down && now - g_all_seen[i] >= INBOX_NETCON_HOLD_S)) {
            held++;
            continue;
        }
        inbox_msgs[n] = g_all[i];
        inbox_msgs[n].read = is_in_read_list(inbox_msgs[n].uuid);
        if (!inbox_msgs[n].read) unread++;
        n++;
    }
    inbox_count = n;
    inbox_unread = unread;
    if (held != g_held_logged) {
        if (held || g_held_logged > 0)
            fprintf(stderr, "[inbox] %d network message(s) of the stock software held back "
                    "(shown after %d min if FreeToon also sees no internet; internet %s)\n",
                    held, INBOX_NETCON_HOLD_S / 60, down ? "down" : "ok");
        g_held_logged = held;
    }
}

void inbox_reapply(void) {
    if (settings.client_mode) return;    /* the master's list comes via client_link */
    pthread_mutex_lock(&g_mtx);
    if (g_all_valid) apply_locked();
    pthread_mutex_unlock(&g_mtx);
}

void inbox_parse_dataset(const char * xml) {
    /* Locate <notifications>...</notifications> block. */
    const char * ns = strstr(xml, "<notifications>");
    if (!ns) return;
    ns += strlen("<notifications>");
    const char * ne = strstr(ns, "</notifications>");
    if (!ne) return;

    static inbox_msg_t parsed[INBOX_MAX];
    int count = 0;
    const char * p = ns;
    while (p < ne && count < INBOX_MAX) {
        const char * start = strstr(p, "<notification>");
        if (!start || start >= ne) break;
        start += strlen("<notification>");
        const char * stop = strstr(start, "</notification>");
        if (!stop || stop > ne) break;

        inbox_msg_t * m = &parsed[count];
        memset(m, 0, sizeof *m);
        xml_text(start, stop, "uuid",       m->uuid,     sizeof(m->uuid));
        xml_text(start, stop, "type",       m->type,     sizeof(m->type));
        xml_text(start, stop, "subType",    m->sub_type, sizeof(m->sub_type));
        xml_text(start, stop, "text",       m->text,     sizeof(m->text));
        if (!m->text[0])   /* sent as <plainText> by FreeToon before #40 */
            xml_text(start, stop, "plainText", m->text, sizeof(m->text));

        /* Nothing to show: FreeToon's own messages from before #40 reached
         * happ_usermsg without a body ("telegram / imported" with no text).
         * Hide those. Only FreeToon's own types: stock ones (error, update,
         * tsc, ...) may legitimately carry no text and are worded by type. */
        int ours = !strcmp(m->type, "telegram") || !strcmp(m->type, "calendar") ||
                   !strcmp(m->type, "integration") || !strcmp(m->type, "package") ||
                   !strcmp(m->type, "system") ||
                   (!strcmp(m->type, "update") && !strcmp(m->sub_type, "freetoon"));
        if (!m->text[0] && ours) {
            memset(m, 0, sizeof(*m));
            p = stop + strlen("</notification>");
            continue;
        }

        /* CR2: Suppress continuous system notification about Service Center connection */
        if (strstr(m->text, "Service Center") || strstr(m->text, "Service center") ||
            strstr(m->text, "service center") || strstr(m->text, "ServiceCenter") ||
            strstr(m->type, "ServiceCenter") || strstr(m->sub_type, "ServiceCenter")) {
            memset(m, 0, sizeof(*m));
            p = stop + strlen("</notification>");
            continue;
        }

        /* Suppress the TSC "resource file update" notice ("Er is een TSC
         * resource file update beschikbaar. Update via TSC menu aub"): it is
         * acted on in the stock UI's TSC menu, which FreeToon doesn't have (#39). */
        if (strstr(m->text, "resource file") || strstr(m->text, "Resource file") ||
            strstr(m->text, "resourcefile") || strstr(m->text, "TSC menu")) {
            memset(m, 0, sizeof(*m));
            p = stop + strlen("</notification>");
            continue;
        }

        /* Suppress TSC root certificate notification (FreeToon uses curl directly and needs no restart) */
        if ((strstr(m->text, "vertrouwde") && strstr(m->text, "certificaten")) ||
            strstr(m->text, "webserver certificaten") ||
            strstr(m->text, "webcertificaten") ||
            (strcmp(m->type, "tsc") == 0 && strstr(m->text, "certificaten"))) {
            memset(m, 0, sizeof(*m));
            p = stop + strlen("</notification>");
            continue;
        }

        char date_buf[24] = {0};
        if (xml_text(start, stop, "creationDate", date_buf, sizeof(date_buf)))
            m->creation_date = atol(date_buf);
        count++;
        p = stop + strlen("</notification>");
    }

    pthread_mutex_lock(&g_mtx);
    long now = (long)time(NULL);
    static long seen[INBOX_MAX];
    for (int i = 0; i < count; i++) {
        seen[i] = now;
        for (int j = 0; j < g_all_n; j++)
            if (parsed[i].uuid[0] && !strcmp(parsed[i].uuid, g_all[j].uuid)) { seen[i] = g_all_seen[j]; break; }
    }
    memcpy(g_all, parsed, sizeof(inbox_msg_t) * (size_t)count);
    memcpy(g_all_seen, seen, sizeof(long) * (size_t)count);
    g_all_n = count;
    g_all_valid = 1;
    apply_locked();
    pthread_mutex_unlock(&g_mtx);
    fprintf(stderr, "[inbox] parsed %d messages, %d unread\n",
            inbox_count, inbox_unread);
}

void inbox_mark_read(const char * uuid) {
    if (!uuid) return;
    add_to_read_list(uuid);
    int unread = 0;
    for (int i = 0; i < inbox_count; i++) {
        if (strcmp(inbox_msgs[i].uuid, uuid) == 0) inbox_msgs[i].read = 1;
        if (!inbox_msgs[i].read) unread++;
    }
    inbox_unread = unread;
    inbox_persist();
}

/* Delete from happ_usermsg via BoxTalk + drop from local array. The
   DeleteNotification verb takes type+subType (not uuid in qt-gui's usage)
   but accepts uuid-targeting in newer firmwares. Send both for safety. */
extern int boxtalk_send_raw_xml(const char * xml);   /* declared inline below */
extern const char * boxtalk_our_uuid(void);
extern const char * boxtalk_usermsg_uuid(void);
void inbox_delete(const char * uuid) {
    if (!uuid) return;
    /* find message to get its type/subType */
    char type[INBOX_TYPE_LEN] = "";
    char st[INBOX_TYPE_LEN] = "";
    int idx = -1;
    for (int i = 0; i < inbox_count; i++) {
        if (strcmp(inbox_msgs[i].uuid, uuid) == 0) {
            idx = i;
            snprintf(type, sizeof(type), "%s", inbox_msgs[i].type);
            snprintf(st,   sizeof(st),   "%s", inbox_msgs[i].sub_type);
            break;
        }
    }
    if (idx < 0) return;

    char xml[640];
    snprintf(xml, sizeof(xml),
        "<action class=\"invoke\" uuid=\"%s\" "
        "destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:Notification\">"
        "<u:DeleteNotification xmlns:u=\"urn:hcb-hae-com:service:Notification:1\">"
        "<type>%s</type><subType>%s</subType><uuid>%s</uuid>"
        "</u:DeleteNotification></action>",
        boxtalk_our_uuid(), boxtalk_usermsg_uuid(), type, st, uuid);
    boxtalk_send_raw_xml(xml);

    /* Local removal — happ_usermsg will send a follow-up UpdateDataSet
       but we drop immediately so the UI feels responsive. */
    pthread_mutex_lock(&g_mtx);
    for (int j = 0; j < g_all_n; j++) {
        if (strcmp(g_all[j].uuid, uuid) == 0) {
            memmove(&g_all[j], &g_all[j + 1], (size_t)(g_all_n - j - 1) * sizeof(inbox_msg_t));
            memmove(&g_all_seen[j], &g_all_seen[j + 1], (size_t)(g_all_n - j - 1) * sizeof(long));
            g_all_n--;
            break;
        }
    }
    pthread_mutex_unlock(&g_mtx);
    memmove(&inbox_msgs[idx], &inbox_msgs[idx + 1],
            (inbox_count - idx - 1) * sizeof(inbox_msg_t));
    inbox_count--;
    int unread = 0;
    for (int i = 0; i < inbox_count; i++) if (!inbox_msgs[i].read) unread++;
    inbox_unread = unread;
}
