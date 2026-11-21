/*
 * ttysniff.c - passive capture of the boiler-module serial line (BACKLOG #64).
 *
 * LD_PRELOAD library loaded into happ_thermstat ONLY (via the stock
 * /tmp/etc-default-HCBv2 hook that scripts/ketellog.sh writes). It logs what
 * happ_thermstat writes to and reads from the boiler UART, plus the line
 * settings it applies (tcsetattr / ioctl). It never changes a byte, a setting
 * or the timing on the line: every hook calls the real function unchanged and
 * only appends to a log file in RAM.
 *
 * Production version of boxtalk_re/ttysniff.c (that one is the RE tool: one
 * log line per byte, no limits). Differences:
 *   - bytes are grouped per direction into one line per burst (a 7-byte Quby
 *     frame per line in steady state), flushed on direction change, a gap of
 *     >= GAP_MS, or a full line;
 *   - tcsetattr / ioctl(TCSETS*) / modem-line ioctls on the UART are logged
 *     decoded (baud, data bits, parity, stop bits, flow control);
 *   - header per process start with the active UI (/mnt/data/ui_choice);
 *   - stops by itself at TTYSNIFF_UNTIL (epoch s), when TTYSNIFF_STOP exists,
 *     or when the log reaches TTYSNIFF_MAX bytes - after that it is inert;
 *   - any /dev/ttymxc* or /dev/ttyS* is matched (boiler: ttymxc0 on a Toon 2,
 *     ttymxc1 on a Toon 1, #62), so the log says which line happ really uses;
 *   - removes LD_PRELOAD from the environment at load, so programs that
 *     happ_thermstat (or the HCBv2 wrapper shell) starts don't load it.
 *
 * Environment (set by ketellog.sh in the HCBv2 hook):
 *   TTYSNIFF_LOG    log file      (default /var/volatile/tmp/ketellog.txt)
 *   TTYSNIFF_UNTIL  epoch seconds (required; missing/0/past = inert)
 *   TTYSNIFF_STOP   stop file     (default /var/volatile/tmp/ketellog.stop)
 *   TTYSNIFF_MAX    max log bytes (default 20 MB)
 *
 * Log lines:
 *   HH:MM:SS.mmm W <hex...>   happ_thermstat -> boiler module
 *   HH:MM:SS.mmm R <hex...>   boiler module -> happ_thermstat
 *   HH:MM:SS.mmm T <text>     line settings / modem lines
 *   HH:MM:SS.mmm * <text>     open / close / start / stop markers
 *   (ketellog.sh and freetoon_diag.sh add "S" status lines to the same file)
 *
 * Build (Toon 2):
 *   arm-linux-gnueabihf-gcc -shared -fPIC -O2 -Wall -o libttysniff.so ttysniff.c -ldl
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#define GAP_MS      15          /* a new burst after this much silence */
#define LINE_MAX_B  32          /* bytes per log line at most */
#define MAX_TTY     4
#define IOCTL_LOG_CAP 300       /* misc ioctl lines per process */

typedef int     (*open_fn)(const char *, int, ...);
typedef int     (*close_fn)(int);
typedef ssize_t (*read_fn)(int, void *, size_t);
typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef int     (*dup_fn)(int);
typedef int     (*dup2_fn)(int, int);
typedef int     (*tcsetattr_fn)(int, int, const struct termios *);
typedef int     (*ioctl_fn)(int, unsigned long, ...);

static open_fn      real_open, real_open64;
static close_fn     real_close;
static read_fn      real_read;
static write_fn     real_write;
static dup_fn       real_dup;
static dup2_fn      real_dup2;
static tcsetattr_fn real_tcsetattr;
static ioctl_fn     real_ioctl;

static int    active;               /* 0 = inert */
static int    log_fd = -1;
static long   until_s;
static long   max_bytes = 20L * 1024 * 1024;
static long   log_bytes;
static char   log_path[128] = "/var/volatile/tmp/ketellog.txt";
static char   stop_path[128] = "/var/volatile/tmp/ketellog.stop";
static long   last_check_s;
static int    header_done;
static int    ioctl_lines;

static int    tty_fds[MAX_TTY] = { -1, -1, -1, -1 };

/* Pending burst. */
static char   pend_dir;
static unsigned char pend[LINE_MAX_B];
static int    pend_n;
static struct timeval pend_t0, pend_last;

static volatile int lock_word;
static __thread int in_hook;

static void lock(void)   { while (__sync_lock_test_and_set(&lock_word, 1)) ; }
static void unlock(void) { __sync_lock_release(&lock_word); }

#define RESOLVE(var, type, name) do { if (!var) var = (type)dlsym(RTLD_NEXT, name); } while (0)

static void resolve_all(void) {
    RESOLVE(real_open,      open_fn,      "open");
    RESOLVE(real_open64,    open_fn,      "open64");
    RESOLVE(real_close,     close_fn,     "close");
    RESOLVE(real_read,      read_fn,      "read");
    RESOLVE(real_write,     write_fn,     "write");
    RESOLVE(real_dup,       dup_fn,       "dup");
    RESOLVE(real_dup2,      dup2_fn,      "dup2");
    RESOLVE(real_tcsetattr, tcsetattr_fn, "tcsetattr");
    RESOLVE(real_ioctl,     ioctl_fn,     "ioctl");
}

static int is_tty(int fd) {
    if (fd < 0) return 0;
    for (int i = 0; i < MAX_TTY; i++) if (tty_fds[i] == fd) return 1;
    return 0;
}
static void tty_add(int fd) {
    for (int i = 0; i < MAX_TTY; i++) if (tty_fds[i] == fd) return;
    for (int i = 0; i < MAX_TTY; i++) if (tty_fds[i] < 0) { tty_fds[i] = fd; return; }
}
static void tty_del(int fd) {
    for (int i = 0; i < MAX_TTY; i++) if (tty_fds[i] == fd) tty_fds[i] = -1;
}

static int ts_str(const struct timeval * tv, char * out, size_t n) {
    struct tm tm;
    time_t s = tv->tv_sec;
    localtime_r(&s, &tm);
    return snprintf(out, n, "%02d:%02d:%02d.%03d",
                    tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv->tv_usec / 1000));
}

/* Raw append, counting bytes. Caller holds the lock. */
static void log_raw(const char * s, int n) {
    if (log_fd < 0 || n <= 0) return;
    ssize_t w = real_write(log_fd, s, (size_t)n);
    if (w > 0) log_bytes += w;
}

static void stop_capture(const char * why);

/* Deadline / stop file / size, at most once per second. Caller holds the lock. */
static void check_limits(void) {
    if (!active) return;
    long now = (long)time(NULL);
    if (log_bytes >= max_bytes) { stop_capture("size limit reached"); return; }
    if (now == last_check_s) return;
    last_check_s = now;
    if (now >= until_s) { stop_capture("time limit reached"); return; }
    struct stat st;
    if (stat(stop_path, &st) == 0) { stop_capture("stopped by ketellog.sh"); return; }
    /* The diag loop appends status lines too: follow the real size now and then. */
    if (fstat(log_fd, &st) == 0) log_bytes = (long)st.st_size;
}

static void flush_pending(void) {
    if (!pend_n || log_fd < 0) { pend_n = 0; return; }
    static const char hx[] = "0123456789abcdef";
    char line[32 + 3 * LINE_MAX_B + 2];
    int o = ts_str(&pend_t0, line, 32);
    line[o++] = ' '; line[o++] = pend_dir;
    for (int i = 0; i < pend_n; i++) {
        line[o++] = ' ';
        line[o++] = hx[pend[i] >> 4];
        line[o++] = hx[pend[i] & 0xf];
    }
    line[o++] = '\n';
    log_raw(line, o);
    pend_n = 0;
}

/* Marker / settings line with the current time. Caller holds the lock. */
static void log_line(char kind, const char * fmt, ...) {
    if (log_fd < 0) return;
    flush_pending();
    struct timeval tv;
    gettimeofday(&tv, NULL);
    char line[320];
    int o = ts_str(&tv, line, 32);
    o += snprintf(line + o, sizeof(line) - o, " %c ", kind);
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(line + o, sizeof(line) - o - 1, fmt, ap);
    va_end(ap);
    if (k < 0) k = 0;
    o += k;
    if (o > (int)sizeof(line) - 2) o = (int)sizeof(line) - 2;
    line[o++] = '\n';
    log_raw(line, o);
}

static void stop_capture(const char * why) {
    if (!active) return;
    log_line('*', "capture stopped: %s (pid %d)", why, (int)getpid());
    active = 0;
    if (log_fd >= 0) { real_close(log_fd); log_fd = -1; }
}

static void add_bytes(char dir, const unsigned char * p, ssize_t n) {
    if (in_hook || n <= 0) return;
    in_hook = 1;
    lock();
    if (active) {
        struct timeval now;
        gettimeofday(&now, NULL);
        for (ssize_t i = 0; i < n; i++) {
            long gap_ms = (now.tv_sec - pend_last.tv_sec) * 1000L
                        + (now.tv_usec - pend_last.tv_usec) / 1000L;
            if (pend_n && (pend_dir != dir || pend_n >= LINE_MAX_B || gap_ms >= GAP_MS))
                flush_pending();
            if (!pend_n) { pend_dir = dir; pend_t0 = now; }
            pend[pend_n++] = p[i];
            pend_last = now;
        }
        check_limits();
    }
    unlock();
    in_hook = 0;
}

static const char * baud_name(speed_t s) {
    switch (s) {
    case B0: return "0"; case B300: return "300"; case B600: return "600";
    case B1200: return "1200"; case B2400: return "2400"; case B4800: return "4800";
    case B9600: return "9600"; case B19200: return "19200"; case B38400: return "38400";
    case B57600: return "57600"; case B115200: return "115200"; case B230400: return "230400";
    default: return "?";
    }
}

/* Decode the termios that is about to be applied. Only c_iflag/c_oflag/
 * c_cflag/c_lflag are read: their layout is the same in glibc's struct
 * termios and the kernel's (ioctl TCSETS), and the speed lives in c_cflag. */
static void log_termios(const char * via, int fd, const struct termios * t) {
    if (!t) return;
    tcflag_t c = t->c_cflag;
    int bits = (c & CSIZE) == CS5 ? 5 : (c & CSIZE) == CS6 ? 6 : (c & CSIZE) == CS7 ? 7 : 8;
    char par = (c & PARENB) ? ((c & PARODD) ? 'O' : 'E') : 'N';
    log_line('T', "%s fd=%d %s %d%c%d%s%s%s%s iflag=%#lx oflag=%#lx cflag=%#lx lflag=%#lx",
             via, fd, baud_name((speed_t)(c & (CBAUD | CBAUDEX))), bits, par,
             (c & CSTOPB) ? 2 : 1,
             (c & CRTSCTS) ? " rtscts" : "",
             (c & CLOCAL) ? " clocal" : " -clocal",
             (c & CREAD) ? "" : " -cread",
             (t->c_iflag & (IXON | IXOFF)) ? " xonxoff" : "",
             (unsigned long)t->c_iflag, (unsigned long)t->c_oflag,
             (unsigned long)c, (unsigned long)t->c_lflag);
}

static int tty_path(const char * path) {
    return path && (!strncmp(path, "/dev/ttymxc", 11) || !strncmp(path, "/dev/ttyS", 9));
}

static void read_ui(char * out, size_t n) {
    snprintf(out, n, "?");
    int fd = real_open("/mnt/data/ui_choice", O_RDONLY);
    if (fd < 0) return;
    char b[32];
    ssize_t r = real_read(fd, b, sizeof(b) - 1);
    real_close(fd);
    if (r <= 0) return;
    b[r] = 0;
    b[strcspn(b, " \r\n")] = 0;
    snprintf(out, n, "%s", b);
}

static void note_open(const char * path, int fd) {
    if (fd < 0 || !tty_path(path) || in_hook) return;
    in_hook = 1;
    lock();
    tty_add(fd);
    if (active) {
        if (!header_done) {
            header_done = 1;
            char ui[32];
            read_ui(ui, sizeof(ui));
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            char d[32];
            strftime(d, sizeof(d), "%Y-%m-%d %H:%M:%S", &tm);
            log_line('*', "=== happ_thermstat pid %d started capture %s, ui=%s, until %ld ===",
                     (int)getpid(), d, ui, until_s);
        }
        log_line('*', "open(%s)=%d", path, fd);
    }
    unlock();
    in_hook = 0;
}

__attribute__((constructor))
static void init(void) {
    resolve_all();
    /* Never let children of happ_thermstat (or the HCBv2 shell) load us. */
    unsetenv("LD_PRELOAD");

    const char * u = getenv("TTYSNIFF_UNTIL");
    until_s = u ? atol(u) : 0;
    if (until_s <= (long)time(NULL)) return;           /* inert */
    const char * v;
    if ((v = getenv("TTYSNIFF_LOG"))  && *v) snprintf(log_path,  sizeof(log_path),  "%s", v);
    if ((v = getenv("TTYSNIFF_STOP")) && *v) snprintf(stop_path, sizeof(stop_path), "%s", v);
    if ((v = getenv("TTYSNIFF_MAX"))  && atol(v) > 0) max_bytes = atol(v);

    struct stat st;
    if (stat(stop_path, &st) == 0) return;             /* stopped already */
    log_fd = real_open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0) return;
    if (fstat(log_fd, &st) == 0) log_bytes = (long)st.st_size;
    if (log_bytes >= max_bytes) { real_close(log_fd); log_fd = -1; return; }
    active = 1;
    /* The header is written at the first open of a serial line, so the
     * HCBv2 wrapper shell (which also sees LD_PRELOAD) leaves no trace. */
}

int open(const char * path, int flags, ...) {
    RESOLVE(real_open, open_fn, "open");
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    int fd = real_open(path, flags, mode);
    note_open(path, fd);
    return fd;
}
int open64(const char * path, int flags, ...) {
    RESOLVE(real_open64, open_fn, "open64");
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    int fd = real_open64(path, flags, mode);
    note_open(path, fd);
    return fd;
}

int close(int fd) {
    RESOLVE(real_close, close_fn, "close");
    if (is_tty(fd) && !in_hook) {
        in_hook = 1;
        lock();
        if (active) log_line('*', "close(%d)", fd);
        tty_del(fd);
        unlock();
        in_hook = 0;
    }
    return real_close(fd);
}

int dup(int oldfd) {
    RESOLVE(real_dup, dup_fn, "dup");
    int nfd = real_dup(oldfd);
    if (nfd >= 0 && is_tty(oldfd)) { lock(); tty_add(nfd); unlock(); }
    return nfd;
}
int dup2(int oldfd, int newfd) {
    RESOLVE(real_dup2, dup2_fn, "dup2");
    int nfd = real_dup2(oldfd, newfd);
    if (nfd >= 0 && is_tty(oldfd)) { lock(); tty_add(nfd); unlock(); }
    return nfd;
}

ssize_t read(int fd, void * buf, size_t count) {
    RESOLVE(real_read, read_fn, "read");
    ssize_t n = real_read(fd, buf, count);
    if (n > 0 && is_tty(fd)) add_bytes('R', buf, n);    /* module -> happ */
    return n;
}

ssize_t write(int fd, const void * buf, size_t count) {
    RESOLVE(real_write, write_fn, "write");
    ssize_t n = real_write(fd, buf, count);
    if (n > 0 && is_tty(fd)) add_bytes('W', buf, n);    /* happ -> module */
    return n;
}

int tcsetattr(int fd, int act, const struct termios * t) {
    RESOLVE(real_tcsetattr, tcsetattr_fn, "tcsetattr");
    if (is_tty(fd) && !in_hook) {
        in_hook = 1;
        lock();
        if (active) log_termios(act == TCSANOW ? "tcsetattr(NOW)" :
                                act == TCSADRAIN ? "tcsetattr(DRAIN)" : "tcsetattr(FLUSH)", fd, t);
        unlock();
        in_hook = 0;
    }
    return real_tcsetattr(fd, act, t);
}

int ioctl(int fd, unsigned long req, ...) {
    RESOLVE(real_ioctl, ioctl_fn, "ioctl");
    va_list ap;
    va_start(ap, req);
    void * arg = va_arg(ap, void *);
    va_end(ap);
    if (is_tty(fd) && !in_hook) {
        in_hook = 1;
        lock();
        if (active) {
            if (req == TCSETS || req == TCSETSW || req == TCSETSF) {
                log_termios(req == TCSETS ? "ioctl(TCSETS)" : req == TCSETSW ? "ioctl(TCSETSW)"
                                                                            : "ioctl(TCSETSF)",
                            fd, (const struct termios *)arg);
            } else if (req == TIOCMSET || req == TIOCMBIS || req == TIOCMBIC) {
                int m = arg ? *(const int *)arg : 0;
                log_line('T', "%s fd=%d %#x%s%s",
                         req == TIOCMSET ? "TIOCMSET" : req == TIOCMBIS ? "TIOCMBIS" : "TIOCMBIC",
                         fd, m, (m & TIOCM_RTS) ? " RTS" : "", (m & TIOCM_DTR) ? " DTR" : "");
            } else if (req != TCGETS && req != FIONREAD && req != TIOCMGET
                       && ioctl_lines < IOCTL_LOG_CAP) {
                ioctl_lines++;
                log_line('T', "ioctl fd=%d req=%#lx", fd, req);
            }
        }
        unlock();
        in_hook = 0;
    }
    return real_ioctl(fd, req, arg);
}
