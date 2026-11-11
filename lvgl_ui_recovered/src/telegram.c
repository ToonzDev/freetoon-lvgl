/*
 * telegram.c — built-in Telegram bot, a drop-in replacement for the QML
 * ToonBot app (github.com/glsf91/toonbot). See telegram.h for the overview.
 *
 * Transport: /usr/bin/curl, like the rest of toonui, but the request is
 * handed to curl as a config file on stdin (`curl -K -`) instead of on the
 * command line. The bot token is part of the URL path, so passing it as an
 * argument would expose it in `ps` to every process on the Toon.
 *
 * Threads:
 *   tg_poll_thread  — getMe, then getUpdates long-poll (timeout 50 s) and
 *                     command handling. Idles while disabled / no token.
 *   tg_alert_thread — every 10 s looks at toon_state for boiler error, low
 *                     water pressure and thermostat-offline, debounced, and
 *                     pushes an alert (and an "all clear") to alert chats.
 *
 * Shared state (chats, log, status) lives behind g_mu. The UI only touches it
 * through the telegram_* accessors.
 */
#define _GNU_SOURCE
#include "telegram.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>

#ifdef WASM_BUILD
/* The browser UI is a remote view of the Toon; the bot runs on the Toon. */
int  telegram_start(void) { return 0; }
void telegram_set_token(const char * t) { (void)t; }
void telegram_set_enabled(int on) { (void)on; }
void telegram_status(char * out, int n) { if (out && n > 0) snprintf(out, n, "-"); }
unsigned telegram_version(void) { return 0; }
int  telegram_get_chats(tg_chat_t * o, int m) { (void)o; (void)m; return 0; }
int  telegram_get_log(tg_log_t * o, int m) { (void)o; (void)m; return 0; }
void telegram_chat_set_state(long long id, int s) { (void)id; (void)s; }
void telegram_chat_set_alerts(long long id, int on) { (void)id; (void)on; }
void telegram_chat_remove(long long id) { (void)id; }
int  telegram_send_test(void) { return 0; }
#else

#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "i18n.h"
#include "boxtalk.h"
#include "canonical_state.h"
#include "energy.h"
#include "schedule.h"
#include "notify.h"
#include "http.h"

/* ------------------------------------------------------------------ state */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;

static tg_chat_t g_chats[TG_MAX_CHATS];
static time_t    g_chat_nag[TG_MAX_CHATS];   /* last "waiting for approval" reply */
static int       g_nchats;
static tg_log_t  g_log[TG_LOG_MAX];          /* newest first */
static int       g_nlog;
static unsigned  g_ver;
static unsigned  g_gen;                       /* bumped on token / enable change */
static pid_t     g_poll_pid;                  /* in-flight long-poll curl, or 0 */
static int       g_started;

enum { ST_DISABLED, ST_NO_TOKEN, ST_CONNECTING, ST_OK, ST_BAD_TOKEN, ST_CONFLICT, ST_NET,
       ST_TOKEN_FORMAT };
/* Outcome of the ToonBot import at startup, shown under "no token". */
static char g_import_note[96];
static int    g_status = ST_DISABLED;
static char   g_bot_user[64];
static time_t g_last_ok;

/* ------------------------------------------------------------------ paths */

static const char * data_dir(void) {
    const char * d = getenv("TOONUI_DATA_DIR");
    if (d && *d) return d;
    if (access("/mnt/data", W_OK) == 0) return "/mnt/data";
    return ".";
}

static void chats_path(char * out, size_t n) {
    snprintf(out, n, "%s/toonui_telegram_chats.txt", data_dir());
}

static const char * api_base(void) {
    /* Override for tests (a local fake Bot API); never set on a real Toon. */
    const char * b = getenv("TOONUI_TELEGRAM_API");
    return (b && *b) ? b : "https://api.telegram.org";
}

/* --------------------------------------------------------- tiny JSON reader
 * Just enough to walk Bot API responses: skip any value, look a key up in an
 * object, read strings (with \u escapes → UTF-8) and integers. All functions
 * take an explicit end pointer and never read past it. */

static const char * js_ws(const char * p, const char * e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char * js_skip_str(const char * p, const char * e) {
    if (p >= e || *p != '"') return NULL;
    for (p++; p < e; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p + 1;
    }
    return NULL;
}

static const char * js_skip(const char * p, const char * e) {
    p = js_ws(p, e);
    if (p >= e) return NULL;
    if (*p == '"') return js_skip_str(p, e);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < e) {
            if (*p == '"') { p = js_skip_str(p, e); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
            p++;
        }
        return NULL;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}

/* `obj` points at '{'. Returns the start of the value for `key`, or NULL. */
static const char * js_get(const char * obj, const char * e, const char * key) {
    if (!obj) return NULL;
    const char * p = js_ws(obj, e);
    if (p >= e || *p != '{') return NULL;
    p++;
    size_t klen = strlen(key);
    for (;;) {
        p = js_ws(p, e);
        if (p >= e || *p == '}') return NULL;
        if (*p != '"') return NULL;
        const char * ks = p + 1;
        const char * ke = js_skip_str(p, e);
        if (!ke) return NULL;
        p = js_ws(ke, e);
        if (p >= e || *p != ':') return NULL;
        p = js_ws(p + 1, e);
        if ((size_t)(ke - 1 - ks) == klen && memcmp(ks, key, klen) == 0) return p;
        p = js_skip(p, e);
        if (!p) return NULL;
        p = js_ws(p, e);
        if (p < e && *p == ',') p++;
    }
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int utf8_put(char * o, size_t room, unsigned cp) {
    if (cp < 0x80)    { if (room < 1) return 0; o[0] = (char)cp; return 1; }
    if (cp < 0x800)   { if (room < 2) return 0; o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { if (room < 3) return 0; o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    if (room < 4) return 0;
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Read a JSON string value into `out` (UTF-8, NUL-terminated, truncated on a
 * character boundary). Returns 1 if `v` was a string. */
static int js_str(const char * v, const char * e, char * out, size_t n) {
    if (n == 0) return 0;
    out[0] = 0;
    if (!v || v >= e || *v != '"') return 0;
    size_t o = 0;
    for (const char * p = v + 1; p < e && *p != '"'; p++) {
        unsigned cp = (unsigned char)*p;
        if (*p == '\\' && p + 1 < e) {
            p++;
            switch (*p) {
                case 'n': cp = '\n'; break;
                case 't': cp = '\t'; break;
                case 'r': cp = '\r'; break;
                case 'b': cp = '\b'; break;
                case 'f': cp = '\f'; break;
                case 'u':
                    if (p + 4 < e && hexv(p[1]) >= 0 && hexv(p[2]) >= 0 && hexv(p[3]) >= 0 && hexv(p[4]) >= 0) {
                        cp = (unsigned)(hexv(p[1]) << 12 | hexv(p[2]) << 8 | hexv(p[3]) << 4 | hexv(p[4]));
                        p += 4;
                        if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 < e && p[1] == '\\' && p[2] == 'u') {
                            int a = hexv(p[3]), b = hexv(p[4]), c = hexv(p[5]), d = hexv(p[6]);
                            if (a >= 0 && b >= 0 && c >= 0 && d >= 0) {
                                unsigned lo = (unsigned)(a << 12 | b << 8 | c << 4 | d);
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                    p += 6;
                                }
                            }
                        }
                    } else cp = '?';
                    break;
                default: cp = (unsigned char)*p; break;   /* \" \\ \/ */
            }
            int w = utf8_put(out + o, n - 1 - o, cp);
            if (w == 0) break;
            o += (size_t)w;
            continue;
        }
        /* Raw byte (already UTF-8). Stop before splitting a multi-byte char. */
        if (cp >= 0xC0) {
            size_t len = cp >= 0xF0 ? 4 : cp >= 0xE0 ? 3 : 2;
            if (o + len > n - 1) break;
            for (size_t i = 0; i < len && p < e; i++) out[o++] = *p++;
            p--;
            continue;
        }
        if (o + 1 > n - 1) break;
        out[o++] = (char)cp;
    }
    out[o] = 0;
    return 1;
}

static int js_ll(const char * v, const char * e, long long * out) {
    if (!v || v >= e) return 0;
    char tmp[32];
    size_t i = 0;
    while (v < e && i < sizeof tmp - 1 && (*v == '-' || isdigit((unsigned char)*v))) tmp[i++] = *v++;
    tmp[i] = 0;
    if (i == 0 || (i == 1 && tmp[0] == '-')) return 0;
    *out = strtoll(tmp, NULL, 10);
    return 1;
}

static int js_true(const char * v, const char * e) {
    return v && e - v >= 4 && memcmp(v, "true", 4) == 0;
}

/* ------------------------------------------------------------ HTTP (curl) */

/* Append `s` to the curl config as a double-quoted string. */
static size_t cfg_quote(char * dst, size_t room, const char * s) {
    size_t o = 0;
    if (room < 3) return 0;
    dst[o++] = '"';
    for (; *s; s++) {
        char c = *s;
        const char * esc = NULL;
        if (c == '"') esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        if (esc) {
            if (o + 2 >= room - 1) break;
            dst[o++] = esc[0]; dst[o++] = esc[1];
        } else {
            if (o + 1 >= room - 1) break;
            dst[o++] = c;
        }
    }
    dst[o++] = '"';
    dst[o] = 0;
    return o;
}

static int token_ok(const char * t) {
    if (!t || !*t || strlen(t) > 63 || !strchr(t, ':')) return 0;
    for (; *t; t++) {
        char c = *t;
        if (!(isalnum((unsigned char)c) || c == ':' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* Normalise a pasted / imported token: keep only the characters a BotFather
 * token can contain (drops spaces, newlines, quotes, zero-width chars from a
 * copy-paste) and a leading "bot" prefix copied from an API URL. Logs why a
 * token is still unusable, without printing the token itself. */
static void token_clean(char * dst, size_t n, const char * src) {
    size_t j = 0, dropped = 0;
    if (!src) src = "";
    if (strncmp(src, "bot", 3) == 0 && isdigit((unsigned char)src[3])) src += 3;
    for (const char * p = src; *p; p++) {
        char c = *p;
        if (isalnum((unsigned char)c) || c == ':' || c == '_' || c == '-') {
            if (j + 1 < n) dst[j++] = c;
        } else dropped++;
    }
    dst[j] = 0;
    if (dropped)
        fprintf(stderr, "[telegram] token: removed %zu stray character(s)\n", dropped);
    if (j && !token_ok(dst))
        fprintf(stderr, "[telegram] token unusable: len=%zu colon=%d\n", j, strchr(dst, ':') != NULL);
}

/* POST `method` with form fields `kv` ("name=value", NULL-terminated; values
 * are URL-encoded by curl). Response body → out. Returns bytes read, or -1.
 * `track` registers the curl pid so a token / enable change can cut a
 * long-poll short. */
static int tg_call(const char * method, const char * const * kv, int max_time,
                   char * out, size_t outsz, int track) {
    char token[64];
    pthread_mutex_lock(&g_mu);
    snprintf(token, sizeof token, "%s", settings.telegram_token);
    pthread_mutex_unlock(&g_mu);
    if (!token_ok(token) || outsz < 2) return -1;

    size_t need = 512;
    for (int i = 0; kv && kv[i]; i++) need += 2 * strlen(kv[i]) + 32;
    char * cfg = malloc(need);
    if (!cfg) return -1;
    char url[256];
    snprintf(url, sizeof url, "%s/bot%s/%s", api_base(), token, method);
    size_t o = 0;
    o += (size_t)snprintf(cfg + o, need - o, "url = ");
    o += cfg_quote(cfg + o, need - o, url);
    o += (size_t)snprintf(cfg + o, need - o,
                          "\nmax-time = %d\nconnect-timeout = 15\nuser-agent = \"toonui\"\n", max_time);
    for (int i = 0; kv && kv[i]; i++) {
        o += (size_t)snprintf(cfg + o, need - o, "data-urlencode = ");
        o += cfg_quote(cfg + o, need - o, kv[i]);
        o += (size_t)snprintf(cfg + o, need - o, "\n");
    }

    int in_p[2], out_p[2];
    if (pipe(in_p) != 0) { free(cfg); return -1; }
    if (pipe(out_p) != 0) { close(in_p[0]); close(in_p[1]); free(cfg); return -1; }

    /* -4: the Toons' IPv6 is usually half-configured; match http.c. */
    static char * const argv[] = { "curl", "-s", "-4", "-K", "-", NULL };
    pid_t pid = fork();
    if (pid < 0) {
        close(in_p[0]); close(in_p[1]); close(out_p[0]); close(out_p[1]);
        free(cfg);
        return -1;
    }
    if (pid == 0) {
        /* Child: async-signal-safe calls only. */
        dup2(in_p[0], 0);
        dup2(out_p[1], 1);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) dup2(dn, 2);
        for (int fd = 3; fd < 256; fd++) close(fd);
        execv("/usr/bin/curl", argv);
        _exit(127);
    }
    close(in_p[0]);
    close(out_p[1]);
    if (track) {
        pthread_mutex_lock(&g_mu);
        g_poll_pid = pid;
        pthread_mutex_unlock(&g_mu);
    }

    /* The config is a few hundred bytes — far below the pipe buffer, so this
     * write can't block against curl's output. */
    size_t w = 0;
    while (w < o) {
        ssize_t k = write(in_p[1], cfg + w, o - w);
        if (k < 0) { if (errno == EINTR) continue; break; }
        w += (size_t)k;
    }
    close(in_p[1]);
    free(cfg);

    size_t got = 0;
    char drain[512];
    for (;;) {
        ssize_t k;
        if (got < outsz - 1) k = read(out_p[0], out + got, outsz - 1 - got);
        else                 k = read(out_p[0], drain, sizeof drain);   /* keep curl from blocking */
        if (k < 0) { if (errno == EINTR) continue; break; }
        if (k == 0) break;
        if (got < outsz - 1) got += (size_t)k;
    }
    out[got] = 0;
    close(out_p[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (track) {
        pthread_mutex_lock(&g_mu);
        if (g_poll_pid == pid) g_poll_pid = 0;
        pthread_mutex_unlock(&g_mu);
    }
    return got > 0 ? (int)got : -1;
}

/* Result of a Bot API call: 1 ok, 0 API error (code in *err), -1 transport. */
static int tg_ok(const char * resp, int len, int * err) {
    if (err) *err = 0;
    if (len <= 0) return -1;
    const char * e = resp + len;
    const char * v = js_get(resp, e, "ok");
    if (!v) return -1;
    if (js_true(v, e)) return 1;
    long long code = 0;
    js_ll(js_get(resp, e, "error_code"), e, &code);
    if (err) *err = (int)code;
    return 0;
}

static void html_escape(const char * in, char * out, size_t n) {
    size_t o = 0;
    for (; *in && o + 1 < n; in++) {
        const char * r = NULL;
        if (*in == '<') r = "&lt;";
        else if (*in == '>') r = "&gt;";
        else if (*in == '&') r = "&amp;";
        if (r) {
            size_t l = strlen(r);
            if (o + l >= n) break;
            memcpy(out + o, r, l);
            o += l;
        } else out[o++] = *in;
    }
    out[o] = 0;
}

static int tg_send(long long chat, const char * html) {
    char cid[40], text[4200];
    snprintf(cid, sizeof cid, "chat_id=%lld", chat);
    snprintf(text, sizeof text, "text=%s", html);
    const char * kv[] = { cid, text, "parse_mode=HTML", "disable_web_page_preview=true", NULL };
    char resp[2048];
    int n = tg_call("sendMessage", kv, 20, resp, sizeof resp, 0);
    int err = 0;
    int ok = tg_ok(resp, n, &err);
    if (ok != 1) fprintf(stderr, "[telegram] sendMessage to %lld failed (%d/%d)\n", chat, ok, err);
    return ok == 1 ? 0 : -1;
}

/* ------------------------------------------------------- chats + log store */

static void chats_save_locked(void) {
    char path[256], tmp[272];
    chats_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE * f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < g_nchats; i++)
        fprintf(f, "%lld\t%d\t%d\t%s\n", g_chats[i].id, g_chats[i].state,
                g_chats[i].alerts, g_chats[i].name);
    fclose(f);
    chmod(tmp, 0600);
    rename(tmp, path);
}

static int chats_load(void) {
    char path[256];
    chats_path(path, sizeof path);
    FILE * f = fopen(path, "r");
    if (!f) return -1;
    char line[160];
    pthread_mutex_lock(&g_mu);
    g_nchats = 0;
    while (fgets(line, sizeof line, f) && g_nchats < TG_MAX_CHATS) {
        line[strcspn(line, "\r\n")] = 0;
        tg_chat_t c;
        memset(&c, 0, sizeof c);
        char * s1 = strchr(line, '\t'); if (!s1) continue; *s1++ = 0;
        char * s2 = strchr(s1, '\t');   if (!s2) continue; *s2++ = 0;
        char * s3 = strchr(s2, '\t');   if (!s3) continue; *s3++ = 0;
        c.id = strtoll(line, NULL, 10);
        c.state = atoi(s1);
        c.alerts = atoi(s2);
        snprintf(c.name, sizeof c.name, "%s", s3);
        if (c.id == 0 || c.state < 0 || c.state > 2) continue;
        g_chats[g_nchats++] = c;
    }
    g_ver++;
    pthread_mutex_unlock(&g_mu);
    fclose(f);
    return 0;
}

static int chat_find_locked(long long id) {
    for (int i = 0; i < g_nchats; i++) if (g_chats[i].id == id) return i;
    return -1;
}

static void clean_name(char * s) {
    for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static void log_add(const char * who, const char * text, const char * result) {
    pthread_mutex_lock(&g_mu);
    if (g_nlog == TG_LOG_MAX) g_nlog--;
    memmove(&g_log[1], &g_log[0], (size_t)g_nlog * sizeof g_log[0]);
    tg_log_t * l = &g_log[0];
    l->t = time(NULL);
    snprintf(l->who, sizeof l->who, "%s", who);
    snprintf(l->text, sizeof l->text, "%s", text);
    snprintf(l->result, sizeof l->result, "%s", result);
    g_nlog++;
    g_ver++;
    pthread_mutex_unlock(&g_mu);
}

static void pending_notify(void) {
    int pending = 0;
    char who[48] = "";
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nchats; i++)
        if (g_chats[i].state == TG_CHAT_PENDING) { pending++; snprintf(who, sizeof who, "%s", g_chats[i].name); }
    pthread_mutex_unlock(&g_mu);
    if (pending) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 tr("Telegram: %s vraagt toegang. Keur goed via Instellingen > Telegram.",
                    "Telegram: %s asks for access. Approve in Settings > Telegram."), who);
        notify_show("telegram", "pending", msg);
    } else {
        notify_clear("telegram", "pending");
    }
}

/* ------------------------------------------------------ ToonBot migration */

/* Take over a ToonBot config so switching from the QML app needs no setup.
 *  - token: whenever no valid token is set yet (so a fixed-up import is
 *    retried on the next start);
 *  - chat list + on/off: first start only (no chats file yet). The chat list
 *    becomes the approved list; the alarm chat gets alerts. */
static void import_toonbot(void) {
    char path[256];
    chats_path(path, sizeof path);
    int first = access(path, F_OK) != 0;
    if (!first && token_ok(settings.telegram_token)) return;

    char tb[256];
    snprintf(tb, sizeof tb, "%s/tsc/toonbot.userSettings.json", data_dir());
    FILE * f = fopen(tb, "r");
    int imported = 0;
    if (!f) {
        fprintf(stderr, "[telegram] no ToonBot config at %s\n", tb);
        snprintf(g_import_note, sizeof g_import_note, "%s",
                 tr("geen ToonBot-config gevonden", "no ToonBot config found"));
    }
    if (f) {
        static char buf[8192];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        const char * e = buf + n;
        const char * root = buf;
        if (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
            root += 3;                                   /* UTF-8 BOM */
        root = js_ws(root, e);
        char raw_tok[128] = "", token[64] = "", refresh[8] = "", alarm[32] = "";
        int is_json = (root < e && *root == '{');
        js_str(js_get(root, e, "Token"), e, raw_tok, sizeof raw_tok);
        token_clean(token, sizeof token, raw_tok);
        const char * verdict = !is_json ? "not JSON" : token_ok(token) ? "ok" : raw_tok[0] ? "INVALID" : "missing";
        fprintf(stderr, "[telegram] ToonBot config %s (%zu bytes): token %s (len %zu)\n",
                tb, n, verdict, strlen(raw_tok));
        snprintf(g_import_note, sizeof g_import_note, "%s",
                 !is_json ? tr("ToonBot-bestand is geen geldige JSON", "ToonBot file is not valid JSON")
                 : token_ok(token) ? ""
                 : raw_tok[0] ? tr("token in ToonBot-bestand is ongeldig", "token in ToonBot file is invalid")
                 : tr("ToonBot-bestand bevat geen token", "ToonBot file has no token"));
        js_str(js_get(root, e, "RefreshOn"), e, refresh, sizeof refresh);
        js_str(js_get(root, e, "AlarmChatId"), e, alarm, sizeof alarm);
        long long alarm_id = alarm[0] ? strtoll(alarm, NULL, 10) : 0;

        pthread_mutex_lock(&g_mu);
        if (!token_ok(settings.telegram_token) && token_ok(token)) {
            snprintf(settings.telegram_token, sizeof settings.telegram_token, "%s", token);
            if (first) settings.telegram_enabled = (strcmp(refresh, "Yes") == 0);
            imported = 1;
        }
        const char * users = first ? js_get(root, e, "Users") : NULL;
        if (users && *users == '[') {
            const char * p = users + 1;
            for (;;) {
                p = js_ws(p, e);
                if (p >= e || *p != '"') break;
                char entry[96];
                js_str(p, e, entry, sizeof entry);
                p = js_skip(p, e);
                if (!p) break;
                p = js_ws(p, e);
                if (p < e && *p == ',') p++;
                char * at = strrchr(entry, '@');      /* "name@chatid" */
                if (!at) continue;
                *at = 0;
                long long id = strtoll(at + 1, NULL, 10);
                if (id == 0 || chat_find_locked(id) >= 0 || g_nchats >= TG_MAX_CHATS) continue;
                tg_chat_t * c = &g_chats[g_nchats++];
                memset(c, 0, sizeof *c);
                c->id = id;
                c->state = TG_CHAT_ALLOWED;
                c->alerts = alarm_id ? (id == alarm_id) : 1;
                snprintf(c->name, sizeof c->name, "%s", entry[0] ? entry : "ToonBot");
                clean_name(c->name);
                imported = 1;
            }
        }
        if (first && alarm_id && chat_find_locked(alarm_id) < 0 && g_nchats < TG_MAX_CHATS) {
            tg_chat_t * c = &g_chats[g_nchats++];
            memset(c, 0, sizeof *c);
            c->id = alarm_id;
            c->state = TG_CHAT_ALLOWED;
            c->alerts = 1;
            snprintf(c->name, sizeof c->name, "%s", tr("Alarmchat", "Alarm chat"));
            imported = 1;
        }
        g_ver++;
        pthread_mutex_unlock(&g_mu);
    }
    if (first) {
        pthread_mutex_lock(&g_mu);
        chats_save_locked();      /* marks the chat migration as done, even when empty */
        pthread_mutex_unlock(&g_mu);
    }
    if (imported) {
        settings_save();
        fprintf(stderr, "[telegram] imported ToonBot settings (%d chats)\n", g_nchats);
        notify_show("telegram", "imported",
                    tr("Telegram: ToonBot-instellingen overgenomen.",
                       "Telegram: ToonBot settings imported."));
    }
}

/* --------------------------------------------------------------- replies */

static const char * preset_name(int s) {
    switch (s) {
        case 0: return "Comfort";
        case 1: return tr("Thuis", "Home");
        case 2: return tr("Slapen", "Sleep");
        case 3: return tr("Weg", "Away");
        case -1: return tr("Handmatig", "Manual");
        default: return tr("onbekend", "unknown");
    }
}

static const char * program_name(int p) {
    switch (p) {
        case PROG_MANUAL:       return tr("Uit", "Off");
        case PROG_BASE:         return tr("Aan", "On");
        case PROG_TEMPOVERRIDE: return tr("Tijdelijk", "Temporary");
        case PROG_LOCKEDBASE:   return tr("Vast", "Locked");
        default:                return tr("onbekend", "unknown");
    }
}

/* Next switch point of the weekly schedule after now ("" when unknown). */
static void next_switch(char * out, size_t n) {
    static time_t last_load;
    out[0] = 0;
    if (schedule_count == 0) {
        time_t now = time(NULL);
        if (now - last_load < 600) return;
        last_load = now;
        if (schedule_load() != 0) return;
    }
    int cnt = schedule_count;
    if (cnt <= 0) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int now_min = ((tm.tm_wday + 6) % 7) * 1440 + tm.tm_hour * 60 + tm.tm_min;   /* 0 = Monday */
    int best = -1, best_d = 7 * 1440 + 1;
    for (int i = 0; i < cnt && i < SCHEDULE_MAX; i++) {
        const schedule_entry_t * s = &schedule_entries[i];
        int m = s->start_day * 1440 + s->start_hour * 60 + s->start_min;
        int d = (m - now_min + 7 * 1440) % (7 * 1440);
        if (d == 0) d = 7 * 1440;
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best < 0) return;
    const schedule_entry_t * s = &schedule_entries[best];
    int days = (tm.tm_hour * 60 + tm.tm_min + best_d) / 1440;
    const char * when = days == 0 ? "" : days == 1 ? tr("morgen ", "tomorrow ")
                       : i18n_day_long((s->start_day + 1) % 7);
    char day[24] = "";
    if (days >= 2) snprintf(day, sizeof day, "%s ", when);
    snprintf(out, n, tr("  Volgende: %som %02d:%02d naar <b>%s</b>\n", "  Next: %sat %02d:%02d to <b>%s</b>\n"),
             days >= 2 ? day : when, s->start_hour, s->start_min, preset_name(s->target_state));
}

static void build_info(char * out, size_t n) {
    canonical_thermostat_t t;
    canonical_state_get_thermostat(&t);
    toon_state_t ts;
    boxtalk_get_snapshot(&ts);
    size_t o = 0;
    o += (size_t)snprintf(out + o, n - o, "%s", tr("<b>Status Toon:</b>\n", "<b>Toon status:</b>\n"));
    if (t.room_temp > 0)
        o += (size_t)snprintf(out + o, n - o, tr("  Temperatuur: <b>%.1f</b> graden\n", "  Temperature: <b>%.1f</b> °C\n"),
                              display_indoor_temp(t.room_temp));
    o += (size_t)snprintf(out + o, n - o, tr("  Thermostaat: <b>%.1f</b> graden\n", "  Thermostat: <b>%.1f</b> °C\n"), t.setpoint);
    o += (size_t)snprintf(out + o, n - o, tr("  Programma: <b>%s</b>\n", "  Program: <b>%s</b>\n"), preset_name(t.active_state));
    o += (size_t)snprintf(out + o, n - o, tr("  Auto programma: <b>%s</b>\n", "  Auto program: <b>%s</b>\n"), program_name(t.program_state));
    if (ts.boiler_type == 0 || t.burner_on || t.dhw_on) {
        const char * b = t.burner_on ? tr("aan (verwarming)", "on (heating)")
                       : t.dhw_on    ? tr("aan (warm water)", "on (hot water)")
                       : tr("uit", "off");
        o += (size_t)snprintf(out + o, n - o, tr("  Ketel: <b>%s</b>\n", "  Boiler: <b>%s</b>\n"), b);
    }
    if (ts.water_pressure > 0.05f)
        o += (size_t)snprintf(out + o, n - o, tr("  Waterdruk: <b>%.1f</b> bar\n", "  Water pressure: <b>%.1f</b> bar\n"),
                              ts.water_pressure);
    if (t.program_state == PROG_BASE || t.program_state == PROG_TEMPOVERRIDE) {
        char nx[128];
        next_switch(nx, sizeof nx);
        if (nx[0] && o < n) o += (size_t)snprintf(out + o, n - o, "%s", nx);
    }
    (void)o;
}

/* Today's use from an hcb_rrd cumulative counter logger, in kWh (<0 = n/a).
 * Same source ToonBot used; values are Wh. */
static double rrd_today(const char * logger) {
    if (settings.client_mode) return -1;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    time_t mid = mktime(&tm);
    char url[256];
    snprintf(url, sizeof url,
             "http://127.0.0.1/hcb_rrd?action=getRrdData&loggerName=%s&rra=5yrhours"
             "&readableTime=0&nullForNaN=1&from=%ld", logger, (long)(mid - 3600));
    static char buf[8192];      /* only the poll thread calls this */
    if (http_fetch_to(url, buf, sizeof buf, 6) != 0) return -1;
    const char * e = buf + strlen(buf);
    const char * p = js_ws(buf, e);
    if (p >= e || *p != '{') return -1;
    p++;
    long long base_ts = -1, first_ts = -1, last_ts = -1;
    double base_v = 0, first_v = 0, last_v = 0;
    for (;;) {
        p = js_ws(p, e);
        if (p >= e || *p != '"') break;
        long long ts = strtoll(p + 1, NULL, 10);
        p = js_skip_str(p, e);
        if (!p) break;
        p = js_ws(p, e);
        if (p >= e || *p != ':') break;
        p = js_ws(p + 1, e);
        const char * ve = js_skip(p, e);
        if (!ve) break;
        if (*p != 'n') {
            double v = strtod(p, NULL);
            if (ts <= (long long)mid && ts > base_ts) { base_ts = ts; base_v = v; }
            if (first_ts < 0 || ts < first_ts) { first_ts = ts; first_v = v; }
            if (ts > last_ts) { last_ts = ts; last_v = v; }
        }
        p = js_ws(ve, e);
        if (p < e && *p == ',') p++;
    }
    if (last_ts < 0) return -1;
    double start = base_ts >= 0 ? base_v : first_v;
    double d = (last_v - start) / 1000.0;
    return d >= 0 ? d : -1;
}

static void build_energy(char * out, size_t n) {
    size_t o = 0;
    o += (size_t)snprintf(out + o, n - o, "%s", tr("<b>Status energie verbruik:</b>\n", "<b>Energy usage:</b>\n"));
    if (energy_connected()) {
        float w = energy_power_w();
        o += (size_t)snprintf(out + o, n - o, tr("  Stroom huidig verbruik: <b>%.0f</b> Watt\n", "  Power use now: <b>%.0f</b> W\n"),
                              w > 0 ? w : 0.0f);
        o += (size_t)snprintf(out + o, n - o, tr("  Stroom huidig teruglevering: <b>%.0f</b> Watt\n", "  Power export now: <b>%.0f</b> W\n"),
                              w < 0 ? -w : 0.0f);
    } else {
        o += (size_t)snprintf(out + o, n - o, tr("  Stroom huidig: <b>n.v.t.</b> (%s)\n", "  Power now: <b>n/a</b> (%s)\n"),
                              energy_offline_label());
    }
    if (energy_have_solar())
        o += (size_t)snprintf(out + o, n - o, tr("  Zonnepanelen: <b>%.0f</b> Watt\n", "  Solar: <b>%.0f</b> W\n"), energy_solar_w());
    double nt = rrd_today("elec_quantity_nt");
    double lt = rrd_today("elec_quantity_lt");
    if (nt >= 0 || lt >= 0)
        o += (size_t)snprintf(out + o, n - o, tr("  Stroom verbruik vandaag: <b>%.3f</b> kWh\n", "  Power used today: <b>%.3f</b> kWh\n"),
                              (nt > 0 ? nt : 0) + (lt > 0 ? lt : 0));
    else
        o += (size_t)snprintf(out + o, n - o, "%s", tr("  Stroom verbruik vandaag: <b>n.v.t.</b>\n", "  Power used today: <b>n/a</b>\n"));
    float g = energy_gas_today_m3();
    if (g >= 0)
        o += (size_t)snprintf(out + o, n - o, tr("  Gas verbruik vandaag: <b>%.2f</b> m3\n", "  Gas used today: <b>%.2f</b> m3\n"), g);
    else
        o += (size_t)snprintf(out + o, n - o, "%s", tr("  Gas verbruik vandaag: <b>n.v.t.</b>\n", "  Gas used today: <b>n/a</b>\n"));
    (void)o;
}

static void build_help(char * out, size_t n, const char * unknown, long long chat) {
    char esc[96] = "";
    size_t o = 0;
    if (unknown && *unknown) {
        html_escape(unknown, esc, sizeof esc);
        o += (size_t)snprintf(out + o, n - o, tr("<b>Onbekend commando ontvangen: <i>%s</i></b>\n",
                                                 "<b>Unknown command: <i>%s</i></b>\n"), esc);
    }
    snprintf(out + o, n - o, "%s%lld\n%s", tr("ChatId: ", "Chat id: "), chat, tr(
        "De volgende commando's zijn mogelijk:\n"
        "  /info    Vraag Toon gegevens op\n"
        "  /energie Vraag energie verbruik op\n"
        "  /prog_c  Activeer programma Comfort\n"
        "  /prog_t  Activeer programma Thuis\n"
        "  /prog_w  Activeer programma Weg\n"
        "  /prog_s  Activeer programma Slapen\n"
        "  /auto_a  Zet auto programma aan\n"
        "  /auto_u  Zet auto programma uit\n"
        "  /therm_xx (6-30) Pas de thermostaat aan. Voeg een 5 toe voor een halve graad.\n\n"
        "  Voorbeeld:\n"
        "    /prog_c    voor programma Comfort\n"
        "    /therm_19  voor thermostaat op 19 graden\n"
        "    /therm_195 voor thermostaat op 19.5 graden\n",
        "Available commands:\n"
        "  /info    Toon status\n"
        "  /energie Energy usage\n"
        "  /prog_c  Program Comfort\n"
        "  /prog_t  Program Home\n"
        "  /prog_w  Program Away\n"
        "  /prog_s  Program Sleep\n"
        "  /auto_a  Auto program on\n"
        "  /auto_u  Auto program off\n"
        "  /therm_xx (6-30) Set the thermostat. Add a 5 for half a degree.\n\n"
        "  Examples:\n"
        "    /prog_c    for program Comfort\n"
        "    /therm_19  for 19 degrees\n"
        "    /therm_195 for 19.5 degrees\n"));
}

/* ToonBot's /therm parsing: "19" → 19, "195" → 19.5, "19.5" → 19.5; 6..30,
 * rounded to the nearest half degree. Returns <0 when invalid. */
static float parse_therm(const char * a) {
    if (!a || !*a) return -1;
    char tmp[16];
    size_t j = 0;
    for (; *a && j < sizeof tmp - 1; a++) tmp[j++] = (*a == ',') ? '.' : *a;
    tmp[j] = 0;
    char * end = NULL;
    double v = strtod(tmp, &end);
    if (end == tmp || *end) return -1;
    if (v >= 60) v /= 10.0;
    if (v < 6.0 || v > 30.0) return -1;
    return (float)(floor(v * 2.0 + 0.5) / 2.0);
}

/* ------------------------------------------------------- command handling */

static void handle_command(long long chat, const char * who, const char * raw) {
    char text[128];
    snprintf(text, sizeof text, "%s", raw);
    /* Trim and lower-case. */
    char * s = text;
    while (*s == ' ') s++;
    size_t L = strlen(s);
    while (L && (s[L - 1] == ' ' || s[L - 1] == '\n')) s[--L] = 0;
    for (char * p = s; *p; p++) *p = (char)tolower((unsigned char)*p);

    /* "/prog_c@my_bot", "/prog c", "/prog_c" → cmd "/prog", arg "c". */
    char cmd[32] = "", arg[32] = "";
    size_t i = 0;
    while (s[i] && s[i] != ' ' && s[i] != '_' && s[i] != '@' && i < sizeof cmd - 1) { cmd[i] = s[i]; i++; }
    cmd[i] = 0;
    const char * rest = s + i;
    /* "/info@my_bot": bot usernames contain '_', so skip to a space only. */
    if (*rest == '@') { while (*rest && *rest != ' ') rest++; }
    if (*rest == '_' || *rest == ' ') rest++;
    size_t j = 0;
    for (; *rest && j < sizeof arg - 1; rest++) {
        if (*rest == '@') break;
        if (*rest != ' ') arg[j++] = *rest;
    }
    arg[j] = 0;

    static char reply[4096];
    char ack[200], esc[140];
    html_escape(raw, esc, sizeof esc);
    snprintf(ack, sizeof ack, tr("Toon heeft het commando: <b><i>%s</i></b> ontvangen.\n\n",
                                 "Toon received the command: <b><i>%s</i></b>\n\n"), esc);
    const char * result = "Ok";

    if (!strcmp(cmd, "/start") || !strcmp(cmd, "/help")) {
        build_help(reply, sizeof reply, NULL, chat);
    } else if ((!strcmp(cmd, "/info") || !strcmp(cmd, "/status")) && !arg[0]) {
        size_t o = (size_t)snprintf(reply, sizeof reply, "%s", ack);
        build_info(reply + o, sizeof reply - o);
    } else if ((!strcmp(cmd, "/energie") || !strcmp(cmd, "/energy")) && !arg[0]) {
        size_t o = (size_t)snprintf(reply, sizeof reply, "%s", ack);
        build_energy(reply + o, sizeof reply - o);
    } else if (!strcmp(cmd, "/prog") && strlen(arg) == 1 && strchr("ctsw", arg[0])) {
        int st = arg[0] == 'c' ? 0 : arg[0] == 't' ? 1 : arg[0] == 's' ? 2 : 3;
        int rc = canonical_state_set_preset(st);
        if (rc != 0) result = tr("Fout", "Error");
        sleep(2);    /* let happ_thermstat's notify land before reporting */
        size_t o = (size_t)snprintf(reply, sizeof reply, "%s", ack);
        build_info(reply + o, sizeof reply - o);
    } else if (!strcmp(cmd, "/auto") && (!strcmp(arg, "a") || !strcmp(arg, "u"))) {
        int rc = arg[0] == 'a' ? boxtalk_resume_schedule() : boxtalk_set_manual();
        if (rc != 0) result = tr("Fout", "Error");
        canonical_state_notify(CSTATE_EVT_THERMOSTAT);
        sleep(2);
        size_t o = (size_t)snprintf(reply, sizeof reply, "%s", ack);
        build_info(reply + o, sizeof reply - o);
    } else if (!strcmp(cmd, "/therm") && parse_therm(arg) > 0) {
        int rc = canonical_state_set_setpoint(parse_therm(arg));
        if (rc != 0) result = tr("Fout", "Error");
        sleep(2);
        size_t o = (size_t)snprintf(reply, sizeof reply, "%s", ack);
        build_info(reply + o, sizeof reply - o);
    } else {
        build_help(reply, sizeof reply, raw, chat);
        result = tr("Onbekend", "Unknown");
    }
    tg_send(chat, reply);
    log_add(who, raw, result);
}

/* One update object from getUpdates. */
static void handle_update(const char * u, const char * e, int first_batch) {
    const char * msg = js_get(u, e, "message");
    if (!msg) return;
    const char * me = js_skip(msg, e);
    if (!me) return;
    char text[256];
    if (!js_str(js_get(msg, me, "text"), me, text, sizeof text)) return;
    const char * chat = js_get(msg, me, "chat");
    if (!chat) return;
    const char * ce = js_skip(chat, me);
    long long cid = 0, date = 0;
    if (!js_ll(js_get(chat, ce, "id"), ce, &cid) || cid == 0) return;
    js_ll(js_get(msg, me, "date"), me, &date);

    char type[16] = "", name[48] = "", first[48] = "", last[48] = "";
    js_str(js_get(chat, ce, "type"), ce, type, sizeof type);
    if (!strcmp(type, "private")) {
        js_str(js_get(chat, ce, "first_name"), ce, first, sizeof first);
        js_str(js_get(chat, ce, "last_name"), ce, last, sizeof last);
        snprintf(name, sizeof name, "%s%s%s", first, last[0] ? " " : "", last);
    } else {
        js_str(js_get(chat, ce, "title"), ce, name, sizeof name);
    }
    if (!name[0]) snprintf(name, sizeof name, "%lld", cid);
    clean_name(name);

    /* Don't act on stale commands (queued while the Toon was off / offline):
     * ToonBot's rule was 10 polling intervals ≈ 10 min. On the very first poll
     * after a (re)start be stricter. If the clock isn't set yet (pre-NTP),
     * skip the backlog entirely rather than replay it. */
    time_t now = time(NULL);
    int clock_ok = now > 1700000000;
    long age = (long)(now - (time_t)date);
    if (!clock_ok ? first_batch : (age > (first_batch ? 120 : 600))) {
        fprintf(stderr, "[telegram] skip stale message from %s (age %lds)\n", name, age);
        return;
    }

    pthread_mutex_lock(&g_mu);
    int k = chat_find_locked(cid);
    int state = k >= 0 ? g_chats[k].state : -1;
    int nag = 0;
    if (k < 0 && g_nchats < TG_MAX_CHATS) {
        tg_chat_t * c = &g_chats[g_nchats];
        memset(c, 0, sizeof *c);
        c->id = cid;
        c->state = TG_CHAT_PENDING;
        c->alerts = 1;
        snprintf(c->name, sizeof c->name, "%s", name);
        g_chat_nag[g_nchats] = now;
        g_nchats++;
        chats_save_locked();
        g_ver++;
        state = TG_CHAT_PENDING;
        nag = 2;
    } else if (k >= 0 && state == TG_CHAT_PENDING && now - g_chat_nag[k] > 600) {
        g_chat_nag[k] = now;
        nag = 1;
    } else if (k >= 0 && strcmp(g_chats[k].name, name) != 0 && state == TG_CHAT_ALLOWED) {
        snprintf(g_chats[k].name, sizeof g_chats[k].name, "%s", name);   /* renamed group/user */
        chats_save_locked();
        g_ver++;
    }
    pthread_mutex_unlock(&g_mu);

    if (state == TG_CHAT_ALLOWED) {
        handle_command(cid, name, text);
    } else if (state == TG_CHAT_PENDING || state < 0) {
        if (nag) {
            char msg2[400];
            snprintf(msg2, sizeof msg2,
                     tr("Deze chat heeft nog geen toegang tot de Toon.\n"
                        "Keur hem goed op de Toon: <b>Instellingen &gt; Telegram</b>.\n(ChatId: %lld)",
                        "This chat has no access to the Toon yet.\n"
                        "Approve it on the Toon: <b>Settings &gt; Telegram</b>.\n(Chat id: %lld)"), cid);
            tg_send(cid, msg2);
        }
        if (nag == 2) pending_notify();
        log_add(name, text, tr("Wacht op goedkeuring", "Awaiting approval"));
    } else {
        log_add(name, text, tr("Geweigerd", "Denied"));
    }
}

/* ---------------------------------------------------------- poll thread */

static void set_status(int st) {
    pthread_mutex_lock(&g_mu);
    if (g_status != st) { g_status = st; g_ver++; }
    if (st == ST_OK) g_last_ok = time(NULL);
    pthread_mutex_unlock(&g_mu);
}

/* Sleep up to `sec`, returning early when settings change. */
static void tg_wait(int sec, unsigned gen) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += sec;
    pthread_mutex_lock(&g_mu);
    while (g_gen == gen) {
        if (pthread_cond_timedwait(&g_cv, &g_mu, &ts) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&g_mu);
}

static void set_commands(void) {
    char json[1024];
    snprintf(json, sizeof json,
        "commands=[{\"command\":\"info\",\"description\":\"%s\"},"
        "{\"command\":\"energie\",\"description\":\"%s\"},"
        "{\"command\":\"prog_c\",\"description\":\"%s Comfort\"},"
        "{\"command\":\"prog_t\",\"description\":\"%s %s\"},"
        "{\"command\":\"prog_s\",\"description\":\"%s %s\"},"
        "{\"command\":\"prog_w\",\"description\":\"%s %s\"},"
        "{\"command\":\"auto_a\",\"description\":\"%s\"},"
        "{\"command\":\"auto_u\",\"description\":\"%s\"},"
        "{\"command\":\"help\",\"description\":\"%s\"}]",
        tr("Toon gegevens", "Toon status"),
        tr("Energie verbruik", "Energy usage"),
        tr("Programma", "Program"),
        tr("Programma", "Program"), tr("Thuis", "Home"),
        tr("Programma", "Program"), tr("Slapen", "Sleep"),
        tr("Programma", "Program"), tr("Weg", "Away"),
        tr("Auto programma aan", "Auto program on"),
        tr("Auto programma uit", "Auto program off"),
        tr("Alle commando's, ook /therm_xx", "All commands, incl. /therm_xx"));
    const char * kv[] = { json, NULL };
    char resp[512];
    tg_call("setMyCommands", kv, 20, resp, sizeof resp, 0);
}

static void * tg_poll_thread(void * arg) {
    (void)arg;
    static char resp[64 * 1024];
    long long offset = 0;
    int have_me = 0, first_batch = 1, backoff = 5;
    unsigned gen = 0;

    for (;;) {
        pthread_mutex_lock(&g_mu);
        int enabled = settings.telegram_enabled;
        int has_tok = token_ok(settings.telegram_token);
        int tok_set = settings.telegram_token[0] != 0;
        if (g_gen != gen) { gen = g_gen; have_me = 0; first_batch = 1; offset = 0; }
        pthread_mutex_unlock(&g_mu);

        if (!enabled || !has_tok) {
            set_status(!enabled ? ST_DISABLED
                       : tok_set ? ST_TOKEN_FORMAT : ST_NO_TOKEN);
            tg_wait(30, gen);
            continue;
        }

        if (!have_me) {
            set_status(ST_CONNECTING);
            int n = tg_call("getMe", NULL, 20, resp, sizeof resp, 1);
            int err = 0, ok = tg_ok(resp, n, &err);
            if (ok == 1) {
                const char * e = resp + n;
                const char * r = js_get(resp, e, "result");
                char user[64] = "";
                js_str(js_get(r, e, "username"), e, user, sizeof user);
                pthread_mutex_lock(&g_mu);
                snprintf(g_bot_user, sizeof g_bot_user, "%s", user);
                pthread_mutex_unlock(&g_mu);
                fprintf(stderr, "[telegram] connected as @%s\n", user);
                have_me = 1;
                backoff = 5;
                set_commands();
                set_status(ST_OK);
            } else {
                set_status(ok == 0 && (err == 401 || err == 404) ? ST_BAD_TOKEN : ST_NET);
                tg_wait(ok == 0 ? 300 : backoff, gen);
                if (backoff < 300) backoff *= 2;
                continue;
            }
        }

        char off[40];
        snprintf(off, sizeof off, "offset=%lld", offset);
        const char * kv[] = { off, "timeout=50", "limit=20", "allowed_updates=[\"message\"]", NULL };
        int n = tg_call("getUpdates", kv, 65, resp, sizeof resp, 1);

        pthread_mutex_lock(&g_mu);
        int changed = (g_gen != gen);
        pthread_mutex_unlock(&g_mu);
        if (changed) continue;

        int err = 0, ok = tg_ok(resp, n, &err);
        if (ok != 1) {
            if (ok == 0 && err == 409) set_status(ST_CONFLICT);
            else if (ok == 0 && (err == 401 || err == 404)) { set_status(ST_BAD_TOKEN); have_me = 0; }
            else set_status(ST_NET);
            fprintf(stderr, "[telegram] getUpdates failed (%d/%d), retry in %ds\n", ok, err, backoff);
            tg_wait(backoff, gen);
            if (backoff < 300) backoff *= 2;
            continue;
        }
        backoff = 5;
        set_status(ST_OK);

        const char * e = resp + n;
        const char * r = js_get(resp, e, "result");
        if (r && *r == '[') {
            const char * p = r + 1;
            for (;;) {
                p = js_ws(p, e);
                if (p >= e || *p != '{') break;
                const char * ue = js_skip(p, e);
                if (!ue) break;
                long long uid = 0;
                if (js_ll(js_get(p, ue, "update_id"), ue, &uid) && uid >= offset)
                    offset = uid + 1;   /* confirm first: a crash mid-command must not replay it */
                handle_update(p, ue, first_batch);
                p = js_ws(ue, e);
                if (p < e && *p == ',') p++;
            }
        }
        first_batch = 0;
    }
    return NULL;
}

/* --------------------------------------------------------- alert thread */

static int send_alert(const char * html) {
    tg_chat_t list[TG_MAX_CHATS];
    int n = telegram_get_chats(list, TG_MAX_CHATS);
    int sent = 0;
    for (int i = 0; i < n; i++)
        if (list[i].state == TG_CHAT_ALLOWED && list[i].alerts && tg_send(list[i].id, html) == 0) sent++;
    return sent;
}

typedef struct { int active; time_t since; } tg_cond_t;

/* Debounced edge detector: returns +1 when `on` has held for `delay` s,
 * -1 when an active condition clears (`off`), 0 otherwise. */
static int cond_step(tg_cond_t * c, int on, int off, int delay, time_t now) {
    if (!c->active) {
        if (!on) { c->since = 0; return 0; }
        if (!c->since) c->since = now;
        if (now - c->since >= delay) { c->active = 1; return 1; }
        return 0;
    }
    if (off) { c->active = 0; c->since = 0; return -1; }
    return 0;
}

static void * tg_alert_thread(void * arg) {
    (void)arg;
    tg_cond_t boiler = {0}, press = {0}, offline = {0};
    time_t started = time(NULL);
    for (;;) {
        sleep(10);
        time_t now = time(NULL);
        if (now - started < 120) continue;              /* let the links come up after boot */
        pthread_mutex_lock(&g_mu);
        int live = settings.telegram_enabled && token_ok(settings.telegram_token);
        pthread_mutex_unlock(&g_mu);
        if (!live) {
            memset(&boiler, 0, sizeof boiler); memset(&press, 0, sizeof press); memset(&offline, 0, sizeof offline);
            continue;
        }
        toon_state_t ts;
        boxtalk_get_snapshot(&ts);
        int mask = settings.telegram_alerts;
        char msg[256];

        int ot_err = ts.connected && ts.boiler_type == 0 && ts.ot_comm_error;
        int r = cond_step(&boiler, ot_err, !ot_err, 120, now);
        if (r && (mask & TG_ALERT_BOILER)) {
            send_alert(r > 0 ? tr("⚠️ <b>Ketelstoring</b>: de Toon heeft geen OpenTherm-verbinding met de ketel.",
                                  "⚠️ <b>Boiler fault</b>: the Toon has lost the OpenTherm link to the boiler.")
                             : tr("✅ Verbinding met de ketel is hersteld.", "✅ Boiler link restored."));
        }

        float p = ts.water_pressure;
        int have_p = p > 0.05f;
        r = cond_step(&press, have_p && p < 0.8f, have_p && p >= 1.0f, 300, now);
        if (r && (mask & TG_ALERT_PRESSURE)) {
            if (r > 0) snprintf(msg, sizeof msg, tr("⚠️ <b>Lage waterdruk</b>: %.1f bar. Vul de cv-installatie bij.",
                                                    "⚠️ <b>Low water pressure</b>: %.1f bar. Top up the heating system."), p);
            else       snprintf(msg, sizeof msg, tr("✅ Waterdruk weer in orde: %.1f bar.", "✅ Water pressure OK again: %.1f bar."), p);
            send_alert(msg);
        }

        r = cond_step(&offline, !ts.connected, ts.connected, 300, now);
        if (r && (mask & TG_ALERT_OFFLINE)) {
            send_alert(r > 0 ? tr("⚠️ <b>Thermostaat niet bereikbaar</b>: de Toon krijgt geen gegevens van de thermostaat-software.",
                                  "⚠️ <b>Thermostat unreachable</b>: the Toon gets no data from the thermostat software.")
                             : tr("✅ Thermostaat weer bereikbaar.", "✅ Thermostat reachable again."));
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ public API */

static void * tg_welcome_thread(void * a);

int telegram_start(void) {
    if (g_started) return 0;
    g_started = 1;
    /* Repair a token saved with stray characters (earlier import / cfg edit). */
    if (settings.telegram_token[0]) {
        char t[64];
        token_clean(t, sizeof t, settings.telegram_token);
        if (strcmp(t, settings.telegram_token) != 0) {
            snprintf(settings.telegram_token, sizeof settings.telegram_token, "%s", t);
            settings_save();
        }
    }
    chats_load();
    import_toonbot();
    pthread_t t;
    if (pthread_create(&t, NULL, tg_poll_thread, NULL) != 0) return -1;
    pthread_detach(t);
    if (pthread_create(&t, NULL, tg_alert_thread, NULL) == 0) pthread_detach(t);
    return 0;
}

static void kick_locked(void) {
    g_gen++;
    g_ver++;
    g_bot_user[0] = 0;
    if (g_poll_pid > 0) kill(g_poll_pid, SIGTERM);
    pthread_cond_broadcast(&g_cv);
}

void telegram_set_token(const char * token) {
    char t[64];
    token_clean(t, sizeof t, token);
    pthread_mutex_lock(&g_mu);
    snprintf(settings.telegram_token, sizeof settings.telegram_token, "%s", t);
    kick_locked();
    pthread_mutex_unlock(&g_mu);
    settings_save();
}

void telegram_set_enabled(int on) {
    pthread_mutex_lock(&g_mu);
    settings.telegram_enabled = on ? 1 : 0;
    kick_locked();
    pthread_mutex_unlock(&g_mu);
    settings_save();
}

void telegram_status(char * out, int n) {
    pthread_mutex_lock(&g_mu);
    int st = g_status;
    char user[64];
    snprintf(user, sizeof user, "%s", g_bot_user);
    pthread_mutex_unlock(&g_mu);
    switch (st) {
        case ST_DISABLED:  snprintf(out, n, "%s", tr("Uitgeschakeld", "Disabled")); break;
        case ST_NO_TOKEN:  snprintf(out, n, "%s%s%s", tr("Geen bot-token ingesteld", "No bot token set"),
                                    g_import_note[0] ? " - " : "", g_import_note); break;
        case ST_TOKEN_FORMAT: snprintf(out, n, "%s", tr("Token heeft een ongeldig formaat (verwacht 123456789:AAH...)",
                                                        "Token has an invalid format (expected 123456789:AAH...)")); break;
        case ST_CONNECTING:snprintf(out, n, "%s", tr("Verbinden met Telegram...", "Connecting to Telegram...")); break;
        case ST_OK:        snprintf(out, n, tr("Verbonden als @%s", "Connected as @%s"), user); break;
        case ST_BAD_TOKEN: snprintf(out, n, "%s", tr("Token geweigerd door Telegram - controleer de token",
                                                     "Token rejected by Telegram - check the token")); break;
        case ST_CONFLICT:  snprintf(out, n, "%s", tr("Conflict: een andere app (ToonBot?) gebruikt deze bot al",
                                                     "Conflict: another app (ToonBot?) is already using this bot")); break;
        default:           snprintf(out, n, "%s", tr("Geen verbinding met Telegram, opnieuw proberen...",
                                                     "Can't reach Telegram, retrying...")); break;
    }
}

unsigned telegram_version(void) {
    pthread_mutex_lock(&g_mu);
    unsigned v = g_ver;
    pthread_mutex_unlock(&g_mu);
    return v;
}

int telegram_get_chats(tg_chat_t * out, int max) {
    pthread_mutex_lock(&g_mu);
    int n = g_nchats < max ? g_nchats : max;
    memcpy(out, g_chats, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&g_mu);
    return n;
}

int telegram_get_log(tg_log_t * out, int max) {
    pthread_mutex_lock(&g_mu);
    int n = g_nlog < max ? g_nlog : max;
    memcpy(out, g_log, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&g_mu);
    return n;
}

void telegram_chat_set_state(long long id, int state) {
    long long welcome = 0;
    pthread_mutex_lock(&g_mu);
    int k = chat_find_locked(id);
    if (k >= 0 && state >= 0 && state <= 2) {
        if (g_chats[k].state != TG_CHAT_ALLOWED && state == TG_CHAT_ALLOWED) welcome = id;
        g_chats[k].state = state;
        chats_save_locked();
        g_ver++;
    }
    pthread_mutex_unlock(&g_mu);
    pending_notify();
    if (welcome) {
        /* Tell the user right away; done off the UI thread (it's a network call). */
        long long * arg = malloc(sizeof *arg);
        if (arg) {
            *arg = welcome;
            pthread_t t;
            if (pthread_create(&t, NULL, tg_welcome_thread, arg) == 0) pthread_detach(t);
            else free(arg);
        }
    }
}

static void * tg_welcome_thread(void * a) {
    long long id = *(long long *)a;
    free(a);
    char help[2048];
    size_t o = (size_t)snprintf(help, sizeof help, "%s",
        tr("✅ Deze chat is goedgekeurd op de Toon.\n\n", "✅ This chat has been approved on the Toon.\n\n"));
    build_help(help + o, sizeof help - o, NULL, id);
    tg_send(id, help);
    return NULL;
}

void telegram_chat_set_alerts(long long id, int on) {
    pthread_mutex_lock(&g_mu);
    int k = chat_find_locked(id);
    if (k >= 0) { g_chats[k].alerts = on ? 1 : 0; chats_save_locked(); g_ver++; }
    pthread_mutex_unlock(&g_mu);
}

void telegram_chat_remove(long long id) {
    pthread_mutex_lock(&g_mu);
    int k = chat_find_locked(id);
    if (k >= 0) {
        memmove(&g_chats[k], &g_chats[k + 1], (size_t)(g_nchats - k - 1) * sizeof g_chats[0]);
        memmove(&g_chat_nag[k], &g_chat_nag[k + 1], (size_t)(g_nchats - k - 1) * sizeof g_chat_nag[0]);
        g_nchats--;
        chats_save_locked();
        g_ver++;
    }
    pthread_mutex_unlock(&g_mu);
    pending_notify();
}

static void * tg_test_thread(void * a) {
    (void)a;
    int n = send_alert(tr("🔔 Testmelding van je Toon. Meldingen werken!",
                          "🔔 Test alert from your Toon. Alerts work!"));
    char res[24];
    snprintf(res, sizeof res, tr("%d chat(s)", "%d chat(s)"), n);
    log_add("(test)", tr("Testmelding", "Test alert"), res);
    return NULL;
}

/* Returns how many chats will get it; the sending happens off the UI thread. */
int telegram_send_test(void) {
    int n = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nchats; i++)
        if (g_chats[i].state == TG_CHAT_ALLOWED && g_chats[i].alerts) n++;
    pthread_mutex_unlock(&g_mu);
    if (n == 0) return 0;
    pthread_t t;
    if (pthread_create(&t, NULL, tg_test_thread, NULL) == 0) pthread_detach(t);
    return n;
}

#endif /* !WASM_BUILD */
