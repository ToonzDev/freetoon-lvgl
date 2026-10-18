/*
 * Inbox model — parses happ_usermsg UpdateDataSet payloads into an array
 * of inbox_msg_t. Read-state stored locally in /mnt/data/toonui_inbox.txt
 * (one UUID per line) since the BoxTalk protocol has no "read" flag.
 */
#include "inbox.h"
#include "boxtalk.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

void inbox_load(void) {
    FILE * f = fopen(READ_FILE, "r");
    if (!f) return;
    char line[80];
    while (fgets(line, sizeof(line), f)) {
        /* strip trailing newline */
        char * nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (line[0]) add_to_read_list(line);
    }
    fclose(f);
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

void inbox_parse_dataset(const char * xml) {
    /* Locate <notifications>...</notifications> block. */
    const char * ns = strstr(xml, "<notifications>");
    if (!ns) return;
    ns += strlen("<notifications>");
    const char * ne = strstr(ns, "</notifications>");
    if (!ne) return;

    inbox_count = 0;
    int unread = 0;
    const char * p = ns;
    while (p < ne && inbox_count < INBOX_MAX) {
        const char * start = strstr(p, "<notification>");
        if (!start || start >= ne) break;
        start += strlen("<notification>");
        const char * stop = strstr(start, "</notification>");
        if (!stop || stop > ne) break;

        inbox_msg_t * m = &inbox_msgs[inbox_count];
        xml_text(start, stop, "uuid",       m->uuid,     sizeof(m->uuid));
        xml_text(start, stop, "type",       m->type,     sizeof(m->type));
        xml_text(start, stop, "subType",    m->sub_type, sizeof(m->sub_type));
        xml_text(start, stop, "text",       m->text,     sizeof(m->text));

        /* CR2: Suppress continuous system notification about Service Center connection */
        if (strstr(m->text, "Service Center") || strstr(m->text, "Service center") ||
            strstr(m->text, "service center") || strstr(m->text, "ServiceCenter") ||
            strstr(m->type, "ServiceCenter") || strstr(m->sub_type, "ServiceCenter")) {
            memset(m, 0, sizeof(*m));
            p = stop + strlen("</notification>");
            continue;
        }

        char date_buf[24] = {0};
        if (xml_text(start, stop, "creationDate", date_buf, sizeof(date_buf)))
            m->creation_date = atol(date_buf);
        m->read = is_in_read_list(m->uuid);
        if (!m->read) unread++;
        inbox_count++;
        p = stop + strlen("</notification>");
    }
    inbox_unread = unread;
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
    memmove(&inbox_msgs[idx], &inbox_msgs[idx + 1],
            (inbox_count - idx - 1) * sizeof(inbox_msg_t));
    inbox_count--;
    int unread = 0;
    for (int i = 0; i < inbox_count; i++) if (!inbox_msgs[i].read) unread++;
    inbox_unread = unread;
}
