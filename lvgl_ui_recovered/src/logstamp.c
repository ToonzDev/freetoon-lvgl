/*
 * logstamp.c - a date/time in front of every line toonui writes to its log.
 *
 * toonui logs with plain fprintf(stderr, "[xx] ...") in hundreds of places,
 * and ui_launcher.sh sends stdout + stderr to toonui.log. Instead of changing
 * every call, logstamp_start() points fd 1 and 2 at a pipe; a thread reads it
 * and writes each line to the original log with "YYYY-MM-DD HH:MM:SS " in
 * front (local time, like ui_launcher.log and the app logs). Children
 * started by toonui (sh, curl, helpers) inherit the pipe and are stamped too.
 *
 *   - Lines that already start with a date ("2026-..." or "[2026-...") are
 *     passed through unchanged.
 *   - The write end is non-blocking: should the thread ever stall, a log
 *     line is dropped instead of toonui hanging on fprintf.
 *   - On a crash (SIGSEGV / SIGBUS / SIGILL / SIGFPE / SIGABRT) the handler
 *     copies what is still in the pipe straight to the log before the
 *     default action runs, so the last lines before a crash are kept.
 *   - FT_NO_LOGSTAMP=1 in the environment switches it off.
 */
#include "logstamp.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int g_out = -1;      /* the original log (fd 2 before) */
static int g_rd = -1;       /* pipe read end */
static volatile int g_busy; /* the reader is writing out what it read */

static int starts_with_date(const char * p, size_t n) {
    if (n && *p == '[') { p++; n--; }
    return n >= 10 && p[0] == '2' && p[1] == '0' && p[4] == '-' && p[7] == '-';
}

static void write_all(int fd, const char * b, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w < 0) { if (errno == EINTR) continue; return; }
        b += w; n -= (size_t)w;
    }
}

/* Stamp and write the bytes in b: a date at the start of every line (not on
 * the continuation of a line longer than the buffer, nor when it has one). */
static void stamp_out(const char * b, size_t n, char * line, size_t cap, size_t * ll, int * at_start) {
    for (size_t i = 0; i < n; i++) {
        if (*ll < cap - 1) line[(*ll)++] = b[i];
        if (b[i] != '\n' && *ll < cap - 1) continue;
        if (*at_start && !starts_with_date(line, *ll)) {
            char ts[24];
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            size_t tl = strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S ", &tm);
            write_all(g_out, ts, tl);
        }
        write_all(g_out, line, *ll);
        *at_start = line[*ll - 1] == '\n';
        *ll = 0;
    }
}

static void * reader(void * arg) {
    (void)arg;
    static char buf[4096], line[2048];
    size_t ll = 0;
    int at_start = 1;
    for (;;) {
        ssize_t r = read(g_rd, buf, sizeof buf);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        g_busy = 1;                     /* at_exit_wait: lines in hand */
        stamp_out(buf, (size_t)r, line, sizeof line, &ll, &at_start);
        g_busy = 0;
    }
    if (ll) write_all(g_out, line, ll);
    return NULL;
}

/* Crash: copy what is still queued, unstamped (only async-signal-safe calls). */
static void on_fatal(int sig) {
    if (g_rd >= 0 && g_out >= 0) {
        int fl = fcntl(g_rd, F_GETFL);
        if (fl >= 0) fcntl(g_rd, F_SETFL, fl | O_NONBLOCK);
        char b[4096];
        ssize_t r;
        while ((r = read(g_rd, b, sizeof b)) > 0) write_all(g_out, b, (size_t)r);
        static const char m[] = "[logstamp] fatal signal, log flushed\n";
        write_all(g_out, m, sizeof m - 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Normal exit (the boot picker returns quickly): give the reader up to
 * 0.5 s to write what is queued or in hand. Reading here ourselves would race
 * with it (a line it holds would be lost). Later output goes straight to the
 * log, unstamped. */
static void at_exit_drain(void) {
    if (g_rd < 0 || g_out < 0) return;
    fflush(stdout);
    fflush(stderr);
    for (int i = 0; i < 100; i++) {
        int q = 0;
        if (ioctl(g_rd, FIONREAD, &q) != 0) q = 0;
        if (q <= 0 && !g_busy) break;
        usleep(5000);
    }
    dup2(g_out, 1);
    dup2(g_out, 2);
}

void logstamp_start(void) {
    const char * off = getenv("FT_NO_LOGSTAMP");
    if (off && *off && *off != '0') return;
    if (g_out >= 0) return;
    int out = dup(2);
    if (out < 0) return;
    int p[2];
    if (pipe(p) != 0) { close(out); return; }
    fcntl(out, F_SETFD, FD_CLOEXEC);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    int fl = fcntl(p[1], F_GETFL);
    if (fl >= 0) fcntl(p[1], F_SETFL, fl | O_NONBLOCK);
    g_out = out;
    g_rd = p[0];
    pthread_t t;
    if (pthread_create(&t, NULL, reader, NULL) != 0) {
        close(p[0]); close(p[1]); close(out);
        g_out = g_rd = -1;
        return;
    }
    pthread_detach(t);
    fflush(stdout);
    fflush(stderr);
    dup2(p[1], 1);
    dup2(p[1], 2);
    close(p[1]);
    atexit(at_exit_drain);
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_fatal;
        sa.sa_flags = SA_RESETHAND;
        sigaction(sigs[i], &sa, NULL);
    }
}
