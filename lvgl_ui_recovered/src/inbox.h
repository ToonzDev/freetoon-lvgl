#ifndef TOON_INBOX_H
#define TOON_INBOX_H

/* In-memory mirror of the happ_usermsg/notifications dataset.
   Updated whenever the BoxTalk thread receives an UpdateDataSet for
   notifications. Read-state is local (Eneco's protocol has no
   read flag) and persisted to /mnt/data/toonui_inbox.txt. */

#define INBOX_MAX           32
#define INBOX_UUID_LEN      40
#define INBOX_TYPE_LEN      32
#define INBOX_TEXT_LEN      256

typedef struct {
    char  uuid[INBOX_UUID_LEN];
    char  type[INBOX_TYPE_LEN];
    char  sub_type[INBOX_TYPE_LEN];
    char  text[INBOX_TEXT_LEN];
    long  creation_date;        /* unix timestamp */
    int   read;                 /* 0 unread, 1 read */
} inbox_msg_t;

extern inbox_msg_t       inbox_msgs[INBOX_MAX];
extern volatile int      inbox_count;
extern volatile int      inbox_unread;

/* Replace the current set from a UpdateDataSet XML payload. */
void inbox_parse_dataset(const char * xml);

void inbox_mark_read(const char * uuid);
void inbox_delete(const char * uuid);   /* removes locally + sends DeleteNotification */

void inbox_persist(void);

#endif
