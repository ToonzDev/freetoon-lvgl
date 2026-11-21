/* Boiler serial capture for support (BACKLOG #64). Thin C front for
 * /mnt/data/ketellog.sh: start/stop never block the caller, status is read
 * from the hook / log files directly (no fork), so it is cheap enough for a
 * 1 s LVGL refresh and for the :10081 logs page. */
#ifndef KETELLOG_H
#define KETELLOG_H

#include <stddef.h>

#define KETELLOG_LOG "/var/volatile/tmp/ketellog.txt"

typedef struct {
    int  available;     /* tap + script installed (Toon 2 releases)        */
    int  active;        /* hook present, before its deadline, not stopped  */
    int  starting;      /* start requested, script still running           */
    long minutes_left;  /* when active                                     */
    long log_bytes;     /* 0 = no log                                      */
    char result[128];   /* last line of the last start ("OK: ..."/"ERROR") */
} ketellog_status_t;

void ketellog_get_status(ketellog_status_t * st);
int  ketellog_start(int minutes);   /* 0 = launched in the background */
int  ketellog_stop(void);           /* 0 = ok */

#endif
