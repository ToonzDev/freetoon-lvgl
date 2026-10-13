/*
 * news — built-in RSS newsreader.
 *
 * A background thread fetches settings.news_rss_url with curl every 15 min,
 * parses <item> blocks for <title> + <link>, and stores up to NEWS_MAX_ITEMS
 * headlines for the home-screen ticker. CDATA-aware, entity-light. The URL is
 * validated to a safe character set before it touches the curl command line.
 */
#define _GNU_SOURCE
#include "news.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

#if !defined(_GNU_SOURCE) || defined(__GLIBC__)
/* Ensure memmem prototype is present even if features.h was pre-included */
extern void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen);
#endif

typedef struct {
    char title[NEWS_TITLE_MAX];
    char link[NEWS_LINK_MAX];
    char body[NEWS_BODY_MAX];   /* RSS <description>, HTML-stripped */
    int  feed;                  /* index into g_feeds[] — which source it came from */
} news_item_t;

/* Convert HTML to plain text in place, turning block-closing and <br> tags into
 * paragraph/line breaks and collapsing extra whitespace while preserving '\n'. */
static void strip_html(char * s) {
    char * w = s;
    for (char * r = s; *r; ) {
        if (*r == '<') {
            /* Detect <br>, </p>, </h2>, </h3>, </h4>, </li> for line/paragraph breaks */
            char t0 = (char)(r[1] | 0x20);
            char t1 = r[1] ? (char)(r[2] | 0x20) : 0;
            char t2 = (r[1] && r[2]) ? (char)(r[3] | 0x20) : 0;
            int is_br = (t0 == 'b' && t1 == 'r' && (t2 == '>' || t2 == ' ' || t2 == '/'));
            int is_p_end = (r[1] == '/' &&
                            ((t1 == 'p' && t2 == '>') ||
                             (t1 == 'h' && (t2 >= '1' && t2 <= '6')) ||
                             (t1 == 'l' && t2 == 'i')));
            while (*r && *r != '>') r++;
            if (*r == '>') r++;
            if (is_p_end) { *w++ = '\n'; *w++ = '\n'; }
            else if (is_br) { *w++ = '\n'; }
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
    /* Collapse horizontal spaces and cap consecutive newlines to 2 */
    char * o = s;
    int sp = 0, nl = 0;
    for (char * r = s; *r; r++) {
        if (*r == '\n') {
            while (o > s && o[-1] == ' ') o--;
            if (nl < 2) *o++ = '\n';
            nl++;
            sp = 0;
        } else if (*r == ' ') {
            if (!sp && nl == 0 && o > s) { *o++ = ' '; sp = 1; }
        } else {
            *o++ = *r;
            sp = 0;
            nl = 0;
        }
    }
    *o = 0;
    while (o > s && (o[-1] == ' ' || o[-1] == '\n')) *--o = 0;
}

static news_item_t     g_items[NEWS_MAX_ITEMS];
static int             g_count = 0;
static char            g_feeds[NEWS_MAX_FEEDS][64];  /* feed display names */
static int             g_feed_n = 0;
static int             g_seq = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Staging buffers used by fetch_thread so g_items is swapped atomically
 * once all enabled feeds have been fetched and parsed. */
static news_item_t     s_items[NEWS_MAX_ITEMS];
static char            s_feeds[NEWS_MAX_FEEDS][64];
static int             s_feed_n = 0;

static int url_ok(const char * raw) {
    if (!raw) return 0;
    while (*raw == ' ' || *raw == '\t' || *raw == '\r' || *raw == '\n') raw++;
    if (!*raw) return 0;
    if (strncmp(raw, "http://", 7) != 0 && strncmp(raw, "https://", 8) != 0) return 0;
    const char * end = raw + strlen(raw);
    while (end > raw && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    for (const char * p = raw; p < end; p++) {
        if (*p == '\'' || *p == '`' || *p == '"' || (unsigned char)*p < 0x20 || *p == ' ')
            return 0;
    }
    return 1;
}

/* UTF-8 C3-xx (Latin-1 supplement) trail byte → base ASCII letter, or 0.
   So "Oekraïne" → "Oekraine", "café" → "cafe" instead of "??" boxes. */
static char latin1_to_ascii(unsigned char n) {
    if (n >= 0xA0 && n <= 0xA5) return 'a';
    if (n == 0xA6) return 0;
    if (n == 0xA7) return 'c';
    if (n >= 0xA8 && n <= 0xAB) return 'e';
    if (n >= 0xAC && n <= 0xAF) return 'i';
    if (n == 0xB0) return 'd';
    if (n == 0xB1) return 'n';
    if (n >= 0xB2 && n <= 0xB6) return 'o';
    if (n == 0xB8) return 'o';
    if (n >= 0xB9 && n <= 0xBC) return 'u';
    if (n == 0xBD || n == 0xBF) return 'y';
    if (n >= 0x80 && n <= 0x85) return 'A';
    if (n == 0x87) return 'C';
    if (n >= 0x88 && n <= 0x8B) return 'E';
    if (n >= 0x8C && n <= 0x8F) return 'I';
    if (n == 0x91) return 'N';
    if (n >= 0x92 && n <= 0x96) return 'O';
    if (n >= 0x99 && n <= 0x9C) return 'U';
    return 0;
}

/* Minimal entity + whitespace cleanup, in place. If keep_nl is non-zero,
 * '\n' characters are preserved (for article bodies). */
static void clean_text_ex(char * s, int keep_nl) {
    static const struct { const char * e; char c; } ent[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'},
        {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", ' '},
        {"&#8216;", '\''}, {"&#8217;", '\''}, {"&#8220;", '"'}, {"&#8221;", '"'},
        {"&#8211;", '-'}, {"&#8212;", '-'}, {NULL, 0}
    };
    for (int i = 0; ent[i].e; i++) {
        char * p;
        while ((p = strstr(s, ent[i].e))) {
            *p = ent[i].c;
            memmove(p + 1, p + strlen(ent[i].e), strlen(p + strlen(ent[i].e)) + 1);
        }
    }
    char * w = s;
    int prev_sp = 0;
    for (char * r = s; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (keep_nl && c == '\n') {
            *w++ = '\n';
            prev_sp = 0;
        } else if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!prev_sp) { *w++ = ' '; prev_sp = 1; }
        } else if (c >= 0xC0) {                       /* UTF-8 lead byte */
            char a = (c == 0xC3 && r[1]) ? latin1_to_ascii((unsigned char)r[1]) : 0;
            if (!a && c == 0xE2 && (unsigned char)r[1] == 0x80) {
                unsigned char b2 = (unsigned char)r[2];
                if (b2 == 0x98 || b2 == 0x99) a = '\'';
                else if (b2 == 0x9C || b2 == 0x9D) a = '"';
                else if (b2 == 0x93 || b2 == 0x94) a = '-';
            }
            if (a) { *w++ = a; prev_sp = 0; }
            else if (!prev_sp) { *w++ = ' '; prev_sp = 1; }   /* drop unknown multibyte */
            while (((unsigned char)r[1]) >= 0x80 && ((unsigned char)r[1]) <= 0xBF) r++;
        } else if (c >= 0x80) {
            /* stray continuation byte — skip */
        } else {
            *w++ = (char)c; prev_sp = 0;
        }
    }
    *w = 0;
    while (w > s && (w[-1] == ' ' || w[-1] == '\n')) *--w = 0;
}

static void clean_text(char * s) { clean_text_ex(s, 0); }

/* Extract the raw inner text of the first <tag>…</tag> inside [blk,blk_end),
 * stripping a CDATA wrapper. Returns 0 on success. */
static int tag_raw(const char * blk, const char * blk_end, const char * tag,
                   char * out, size_t outsz) {
    char open[24], close[24];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char * a = memmem(blk, blk_end - blk, open, strlen(open));
    /* also accept <tag ...> with attributes */
    if (!a) {
        char open2[24]; snprintf(open2, sizeof open2, "<%s", tag);
        a = memmem(blk, blk_end - blk, open2, strlen(open2));
        if (!a) return -1;
        a = memchr(a, '>', blk_end - a);
        if (!a) return -1;
        a++;
    } else {
        a += strlen(open);
    }
    const char * b = memmem(a, blk_end - a, close, strlen(close));
    if (!b) return -1;
    const char * s = a; const char * e = b;
    /* unwrap CDATA */
    if (e - s > 12 && !strncmp(s, "<![CDATA[", 9)) {
        s += 9;
        const char * cend = memmem(s, e - s, "]]>", 3);
        if (cend) e = cend;
    }
    size_t len = (size_t)(e - s);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, s, len); out[len] = 0;
    return 0;
}

static int tag_text(const char * blk, const char * blk_end, const char * tag,
                    char * out, size_t outsz) {
    if (tag_raw(blk, blk_end, tag, out, outsz) != 0) return -1;
    clean_text(out);
    return 0;
}

/* The channel/feed display name: the <title> that appears before the first
   <item>/<entry>. Falls back to "" if none — caller substitutes the host. */
static void feed_title(const char * xml, size_t len, char * out, size_t outsz) {
    out[0] = 0;
    const char * end = xml + len;
    const char * it = memmem(xml, len, "<item", 5);
    if (!it) it = memmem(xml, len, "<entry", 6);
    const char * hi = it ? it : end;
    tag_text(xml, hi, "title", out, outsz);
}

/* host part of a URL (after scheme, up to the next '/') for a fallback name. */
static void url_host(const char * u, char * out, size_t outsz) {
    const char * p = strstr(u, "://");
    p = p ? p + 3 : u;
    size_t i = 0;
    while (p[i] && p[i] != '/' && i < outsz - 1) { out[i] = p[i]; i++; }
    out[i] = 0;
}

/* Parse one feed into the staging buffers (s_items/s_feeds), appending items
   starting at `start`; returns the new total (capped at NEWS_MAX_ITEMS). */
static int parse_feed_staged(const char * xml, size_t len, int start, const char * feed_name,
                             int max_this_feed) {
    static char raw_desc[16384];
    const char * p = xml;
    const char * end = xml + len;
    int n = start;
    if (start == 0) s_feed_n = 0;
    int feed_slot = -1;                 /* assigned lazily on the first item */
    /* Stop at the global cap OR this feed's fair share (max_this_feed) so one
       busy feed can't eat every slot and starve the others. */
    while (n < NEWS_MAX_ITEMS && (n - start) < max_this_feed) {
        const char * it = memmem(p, end - p, "<item", 5);
        if (!it) it = memmem(p, end - p, "<entry", 6);   /* Atom fallback */
        if (!it) break;
        const char * it_end = memmem(it, end - it, "</item", 6);
        if (!it_end) it_end = memmem(it, end - it, "</entry", 7);
        if (!it_end) it_end = end;

        char title[NEWS_TITLE_MAX] = "", link[NEWS_LINK_MAX] = "";
        if (tag_text(it, it_end, "title", title, sizeof title) == 0 && title[0]) {
            tag_text(it, it_end, "link", link, sizeof link);  /* link optional */
            raw_desc[0] = 0;
            if (tag_raw(it, it_end, "content:encoded", raw_desc, sizeof raw_desc) != 0 || !raw_desc[0])
                tag_raw(it, it_end, "description", raw_desc, sizeof raw_desc);
            if (raw_desc[0]) {
                strip_html(raw_desc);
                clean_text_ex(raw_desc, 1);
            }
            if (feed_slot < 0) {        /* register this feed on its first item */
                feed_slot = s_feed_n < NEWS_MAX_FEEDS ? s_feed_n : NEWS_MAX_FEEDS - 1;
                snprintf(s_feeds[feed_slot], sizeof s_feeds[feed_slot], "%s",
                         feed_name && feed_name[0] ? feed_name : "Nieuws");
                if (s_feed_n < NEWS_MAX_FEEDS) s_feed_n++;
            }
            snprintf(s_items[n].title, sizeof s_items[n].title, "%s", title);
            snprintf(s_items[n].link,  sizeof s_items[n].link,  "%s", link);
            snprintf(s_items[n].body,  sizeof s_items[n].body,  "%s", raw_desc);
            s_items[n].feed = feed_slot;
            n++;
        }
        p = it_end + 1;
        if (p >= end) break;
    }
    fprintf(stderr, "[news] staged %d headlines\n", n);
    return n;
}

static volatile int g_wake_fetch = 0;
static pthread_t    g_fetch_thread = 0;

static void * fetch_thread(void * arg) {
    (void)arg;
    static char buf[131072];     /* RSS feeds fit comfortably in 128 KB */
    for (;;) {
        if (settings.news_enabled && settings.news_rss_url[0]) {
            /* news_rss_url holds one feed URL per line — fetch each, merge.
               A line whose first non-space char is '#' is DISABLED (hidden)
               and skipped (per-feed show/hide toggle in Settings). */
            char urls[sizeof settings.news_rss_url];

            /* Each enabled feed gets up to NEWS_MAX_PER_FEED (12) items for the
               news reader screen; the home-screen ticker caps its own display
               to 12 items total across feeds. */
            int per_feed = NEWS_MAX_PER_FEED;

            /* Fetch + parse each enabled feed into staging buffers, then swap
               atomically into g_items so the UI never sees a partial list. */
            snprintf(urls, sizeof urls, "%s", settings.news_rss_url);
            int total = 0, did = 0;
            s_feed_n = 0;
            char * save = NULL;
            for (char * u = strtok_r(urls, "\n", &save); u && total < NEWS_MAX_ITEMS;
                 u = strtok_r(NULL, "\n", &save)) {
                while (*u == ' ' || *u == '\t' || *u == '\r') u++;
                if (!*u || *u == '#') continue;          /* disabled feed */
                if (!url_ok(u)) continue;
                char cmd[600];
                snprintf(cmd, sizeof cmd,
                    "curl -k -s -L -m 20 -A 'Mozilla/5.0 (freetoon-news)' '%s' 2>/dev/null", u);
                FILE * f = popen(cmd, "r");
                if (!f) continue;
                size_t got = fread(buf, 1, sizeof buf - 1, f);
                pclose(f);
                buf[got] = 0;
                if (got > 64) {
                    char fname[64];
                    feed_title(buf, got, fname, sizeof fname);
                    if (!fname[0]) url_host(u, fname, sizeof fname);
                    total = parse_feed_staged(buf, got, total, fname, per_feed);
                    did = 1;
                }
            }
            if (did) {
                pthread_mutex_lock(&g_mtx);
                memcpy(g_items, s_items, (size_t)total * sizeof(news_item_t));
                memcpy(g_feeds, s_feeds, sizeof(g_feeds));
                g_count  = total;
                g_feed_n = s_feed_n;
                g_seq++;
                pthread_mutex_unlock(&g_mtx);
                fprintf(stderr, "[news] committed %d headlines (seq=%d)\n", total, g_seq);
            }
        }
        for (int s = 0; s < 900 && !g_wake_fetch; s += 2) {
            sleep(2);
        }
        g_wake_fetch = 0;
    }
    return NULL;
}

int news_start(void) {
    if (g_fetch_thread) return 0;
    if (pthread_create(&g_fetch_thread, NULL, fetch_thread, NULL) != 0) return -1;
    pthread_detach(g_fetch_thread);
    return 0;
}

void news_wake_fetch(void) {
    g_wake_fetch = 1;
    if (!g_fetch_thread) news_start();
}

int news_test_feed(const char * raw_url, char * msg, size_t msgsz) {
    if (!raw_url) { snprintf(msg, msgsz, "Geen URL opgegeven"); return -1; }
    while (*raw_url == ' ' || *raw_url == '\t' || *raw_url == '\r' || *raw_url == '\n') raw_url++;
    char url[300];
    size_t len = strlen(raw_url);
    while (len > 0 && (raw_url[len - 1] == ' ' || raw_url[len - 1] == '\t' || raw_url[len - 1] == '\r' || raw_url[len - 1] == '\n'))
        len--;
    if (len >= sizeof(url)) len = sizeof(url) - 1;
    memcpy(url, raw_url, len);
    url[len] = 0;
    if (!url_ok(url)) { snprintf(msg, msgsz, "Ongeldige URL (http/https, geen spaties)"); return -1; }
    char cmd[600];
    snprintf(cmd, sizeof cmd,
        "curl -k -s -L -m 15 -A 'Mozilla/5.0 (freetoon-news)' '%s' 2>/dev/null", url);
    FILE * f = popen(cmd, "r");
    if (!f) { snprintf(msg, msgsz, "Kon curl niet starten"); return -1; }
    static char buf[131072];
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    pclose(f);
    buf[got] = 0;
    if (got < 32) { snprintf(msg, msgsz, "Geen data ontvangen (controleer verbinding)"); return -1; }

    /* Count items + grab the first title. */
    int n = 0;
    char first[NEWS_TITLE_MAX] = "";
    const char * p = buf; const char * end = buf + got;
    while (1) {
        const char * it = memmem(p, end - p, "<item", 5);
        if (!it) it = memmem(p, end - p, "<entry", 6);
        if (!it) break;
        const char * it_end = memmem(it, end - it, "</item", 6);
        if (!it_end) it_end = memmem(it, end - it, "</entry", 7);
        if (!it_end) it_end = end;
        char title[NEWS_TITLE_MAX];
        if (tag_text(it, it_end, "title", title, sizeof title) == 0 && title[0]) {
            if (n == 0) snprintf(first, sizeof first, "%s", title);
            n++;
        }
        p = it_end + 1;
        if (p >= end) break;
    }
    if (n == 0) { snprintf(msg, msgsz, "Geen koppen gevonden (geen RSS/Atom?)"); return 0; }
    snprintf(msg, msgsz, "OK: %d koppen. Eerste: %.80s", n, first);
    return n;
}

int news_count(void) {
    pthread_mutex_lock(&g_mtx);
    int c = g_count;
    pthread_mutex_unlock(&g_mtx);
    return c;
}

int news_seq(void) {
    pthread_mutex_lock(&g_mtx);
    int s = g_seq;
    pthread_mutex_unlock(&g_mtx);
    return s;
}

/* Slave bridge: replace the in-memory list with the master's. The local
 * fetch thread isn't running in client_mode/WASM, so there's no race. */
void news_set_count(int n) {
    if (n < 0) n = 0;
    if (n > NEWS_MAX_ITEMS) n = NEWS_MAX_ITEMS;
    pthread_mutex_lock(&g_mtx);
    g_count = n;
    g_seq++;
    pthread_mutex_unlock(&g_mtx);
}
void news_set_item_data(int i, const char * title, const char * link,
                        const char * body, int feed) {
    if (i < 0 || i >= NEWS_MAX_ITEMS) return;
    pthread_mutex_lock(&g_mtx);
    snprintf(g_items[i].title, sizeof g_items[i].title, "%s", title ? title : "");
    snprintf(g_items[i].link,  sizeof g_items[i].link,  "%s", link  ? link  : "");
    snprintf(g_items[i].body,  sizeof g_items[i].body,  "%s", body  ? body  : "");
    g_items[i].feed = feed;
    pthread_mutex_unlock(&g_mtx);
}

int news_item(int i, char * title, size_t tsz, char * link, size_t lsz) {
    int rc = -1;
    pthread_mutex_lock(&g_mtx);
    if (i >= 0 && i < g_count) {
        if (title) snprintf(title, tsz, "%s", g_items[i].title);
        if (link)  snprintf(link,  lsz, "%s", g_items[i].link);
        rc = 0;
    }
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

int news_body(int i, char * body, size_t bsz) {
    int rc = -1;
    pthread_mutex_lock(&g_mtx);
    if (i >= 0 && i < g_count) {
        snprintf(body, bsz, "%s", g_items[i].body);
        rc = 0;
    }
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

int news_feed_count(void) {
    pthread_mutex_lock(&g_mtx);
    int c = g_feed_n;
    pthread_mutex_unlock(&g_mtx);
    return c;
}

int news_feed_name(int f, char * name, size_t nsz) {
    int rc = -1;
    pthread_mutex_lock(&g_mtx);
    if (f >= 0 && f < g_feed_n) { snprintf(name, nsz, "%s", g_feeds[f]); rc = 0; }
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

int news_item_feed(int i) {
    int rc = -1;
    pthread_mutex_lock(&g_mtx);
    if (i >= 0 && i < g_count) rc = g_items[i].feed;
    pthread_mutex_unlock(&g_mtx);
    return rc;
}
