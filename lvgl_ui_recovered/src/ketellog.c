/* Boiler serial capture front-end (BACKLOG #64), see ketellog.h and
 * scripts/ketellog.sh for the mechanism. */
#include "ketellog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define KL_SCRIPT  "/mnt/data/ketellog.sh"
#define KL_LIB     "/mnt/data/libttysniff.so"
#define KL_HOOK    "/tmp/etc-default-HCBv2"
#define KL_STOP    "/var/volatile/tmp/ketellog.stop"
#define KL_RESULT  "/var/volatile/tmp/ketellog.result"
#define KL_RUNNING "/var/volatile/tmp/ketellog.running"

static long hook_until(void) {
    FILE * f = fopen(KL_HOOK, "r");
    if (!f) return 0;
    char line[512];
    long until = 0;
    while (fgets(line, sizeof line, f)) {
        const char * p = strstr(line, "TTYSNIFF_UNTIL=");
        if (p) { until = atol(p + 15); break; }
    }
    fclose(f);
    return until;
}

void ketellog_get_status(ketellog_status_t * st) {
    memset(st, 0, sizeof *st);
    st->available = access(KL_SCRIPT, R_OK) == 0 && access(KL_LIB, R_OK) == 0;
    struct stat sb;
    if (stat(KETELLOG_LOG, &sb) == 0) st->log_bytes = (long)sb.st_size;
    st->starting = access(KL_RUNNING, F_OK) == 0;

    long until = hook_until();
    long now = (long)time(NULL);
    if (until > now && access(KL_STOP, F_OK) != 0) {
        st->active = 1;
        st->minutes_left = (until - now + 59) / 60;
    }

    FILE * f = fopen(KL_RESULT, "r");
    if (f) {
        char line[sizeof st->result];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0]) snprintf(st->result, sizeof st->result, "%s", line);
        }
        fclose(f);
    }
}

int ketellog_start(int minutes) {
    if (access(KL_SCRIPT, R_OK) != 0 || access(KL_LIB, R_OK) != 0) return -1;
    if (access(KL_RUNNING, F_OK) == 0) return 0;          /* already starting */
    if (minutes < 5) minutes = 5;
    if (minutes > 240) minutes = 240;
    char cmd[320];
    /* Background: the script waits up to ~50 s for happ_thermstat to come
     * back healthy. The marker file tells the UI it is still running. */
    snprintf(cmd, sizeof cmd,
             "( : > " KL_RUNNING "; sh " KL_SCRIPT " start %d > " KL_RESULT " 2>&1; "
             "rm -f " KL_RUNNING " ) </dev/null >/dev/null 2>&1 &", minutes);
    fprintf(stderr, "[ketellog] start %d min\n", minutes);
    return system(cmd) == 0 ? 0 : -1;
}

int ketellog_stop(void) {
    if (access(KL_SCRIPT, R_OK) != 0) return -1;
    fprintf(stderr, "[ketellog] stop\n");
    /* The stop file alone halts the tap at once (it checks every second);
     * the script (hook removal + a status line with a curl) runs behind. */
    FILE * f = fopen(KL_STOP, "w");
    if (f) fclose(f);
    int rc = system("( sh " KL_SCRIPT " stop > " KL_RESULT " 2>&1 ) </dev/null >/dev/null 2>&1 &");
    return rc == 0 ? 0 : -1;
}
