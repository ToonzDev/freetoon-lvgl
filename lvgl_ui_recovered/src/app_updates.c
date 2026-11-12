/*
 * Marketplace app updates — see app_updates.h (backlog #35).
 *
 * Reads the installed manifests itself (read-only) instead of going through
 * tile_slots: tile_slots_init() rebuilds the registry the UI thread reads, so
 * it must not be called from this background check.
 */
#include "app_updates.h"
#include "update_check.h"   /* BUILD_VERSION */
#include "shell_util.h"
#include "settings.h"
#include "http.h"
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CATALOG_URL    "https://raw.githubusercontent.com/ToonzDev/freetoon-integrations/main/catalog/index.json"
#define INSTALL_HELPER "/mnt/data/integrations-install.sh"
#define INTEG_ROOT     "/mnt/data/integrations"

#define CAT_MAX 32

typedef struct {
    char id[33];
    char name[48];
    char version[24];
    char min_toonui[24];
} cat_entry_t;

typedef struct {
    char id[33];
    char name[48];
    char version[24];   /* catalog version */
} upd_entry_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static cat_entry_t s_cat[CAT_MAX];          /* last fetched catalog */
static int         s_cat_n = 0;
static upd_entry_t s_upd[CAT_MAX];          /* installed apps with an update */
static int         s_upd_n = 0;
static char        s_sig[512];              /* "id=ver;id=ver;" of s_upd */
static char        s_dismissed[512];        /* s_sig the user dismissed */
static int         s_dismissed_loaded = 0;

/* ------------------------------------------------------------- helpers */

static const char * data_dir(void) {
    const char * d = getenv("TOONUI_DATA_DIR");
    if (d && *d) return d;
    if (access("/mnt/data", W_OK) == 0) return "/mnt/data";
    return ".";
}

static const char * integ_root(void) {
    const char * d = getenv("TOONUI_INTEG_ROOT");   /* sim / tests */
    return (d && *d) ? d : INTEG_ROOT;
}

static void parse_ver(const char * s, int * a, int * b, int * c) {
    while (*s == 'v' || *s == 'V' || *s == ' ') s++;
    *a = *b = *c = 0;
    sscanf(s, "%d.%d.%d", a, b, c);
}

static int ver_cmp(const char * x, const char * y) {
    int a1, a2, a3, b1, b2, b3;
    parse_ver(x, &a1, &a2, &a3);
    parse_ver(y, &b1, &b2, &b3);
    if (a1 != b1) return a1 - b1;
    if (a2 != b2) return a2 - b2;
    return a3 - b3;
}

/* "key":"value" inside [src, end). Returns 1 when found. */
static int json_str_in(const char * src, const char * end, const char * key,
                       char * out, size_t outsz) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    size_t pl = strlen(pat);
    out[0] = 0;
    for (const char * p = src; p + pl <= end; p++) {
        if (memcmp(p, pat, pl) != 0) continue;
        const char * q = p + pl;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q >= end || *q != ':') continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q >= end || *q != '"') continue;
        q++;
        size_t n = 0;
        while (q < end && *q != '"' && n + 1 < outsz) {
            if (*q == '\\' && q + 1 < end) q++;
            out[n++] = *q++;
        }
        out[n] = 0;
        return 1;
    }
    return 0;
}

int app_catalog_compatible(const char * min_toonui) {
    if (!min_toonui || !min_toonui[0]) return 1;
    /* A build without a version tag ("dev") can't be compared; let it try —
     * the plugin loader's ABI gate is still the final word. */
    int a, b, c;
    parse_ver(BUILD_VERSION, &a, &b, &c);
    if (a == 0 && b == 0 && c == 0) return 1;
    return ver_cmp(BUILD_VERSION, min_toonui) >= 0;
}

/* End of the JSON object whose '{' is at `open`: the matching '}' (nested
 * objects such as "tile":{...} and braces inside strings skipped), or NULL. */
const char * app_json_obj_end(const char * open) {
    int depth = 0, in_str = 0;
    for (const char * p = open; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') in_str = 0;
        } else if (*p == '"') in_str = 1;
        else if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) return p;
    }
    return NULL;
}

/* Bounding the field search to each entry's object matters for optional
 * fields like min_toonui: an unbounded strstr would pick up the NEXT entry's. */
static int parse_catalog(const char * buf, cat_entry_t * out, int max) {
    int n = 0;
    const char * scan = strstr(buf, "\"integrations\"");
    if (!scan) return 0;
    while (n < max) {
        const char * id = strstr(scan, "\"id\"");
        if (!id) break;
        const char * obj = id;
        while (obj > scan && *obj != '{') obj--;
        const char * end = (*obj == '{') ? app_json_obj_end(obj) : NULL;
        if (!end) break;
        cat_entry_t * e = &out[n];
        memset(e, 0, sizeof *e);
        json_str_in(obj, end, "id", e->id, sizeof e->id);
        if (sh_is_ident(e->id, 32)) {
            json_str_in(obj, end, "name",       e->name,       sizeof e->name);
            json_str_in(obj, end, "version",    e->version,    sizeof e->version);
            json_str_in(obj, end, "min_toonui", e->min_toonui, sizeof e->min_toonui);
            n++;
        }
        scan = end + 1;
    }
    return n;
}

/* Installed version of `id` ("" when not installed). */
static int installed_version(const char * id, char * ver, size_t vsz) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s/manifest.json", integ_root(), id);
    ver[0] = 0;
    FILE * f = fopen(path, "r");
    if (!f) return 0;
    char json[4096];
    size_t n = fread(json, 1, sizeof json - 1, f);
    fclose(f);
    json[n] = 0;
    return json_str_in(json, json + n, "version", ver, vsz);
}

static void load_dismissed(void) {
    if (s_dismissed_loaded) return;
    s_dismissed_loaded = 1;
    char path[256];
    snprintf(path, sizeof path, "%s/toonui_app_updates_seen", data_dir());
    FILE * f = fopen(path, "r");
    if (!f) return;
    if (!fgets(s_dismissed, sizeof s_dismissed, f)) s_dismissed[0] = 0;
    fclose(f);
    s_dismissed[strcspn(s_dismissed, "\r\n")] = 0;
}

/* Caller holds s_lock. */
static void recompute_locked(void) {
    s_upd_n = 0;
    s_sig[0] = 0;
    size_t sl = 0;
    for (int i = 0; i < s_cat_n; i++) {
        const cat_entry_t * c = &s_cat[i];
        if (!c->version[0] || !app_catalog_compatible(c->min_toonui)) continue;
        char inst[24];
        if (!installed_version(c->id, inst, sizeof inst) || !inst[0]) continue;
        if (ver_cmp(c->version, inst) <= 0) continue;
        upd_entry_t * u = &s_upd[s_upd_n++];
        snprintf(u->id, sizeof u->id, "%s", c->id);
        snprintf(u->name, sizeof u->name, "%s", c->name[0] ? c->name : c->id);
        snprintf(u->version, sizeof u->version, "%s", c->version);
        int w = snprintf(s_sig + sl, sizeof s_sig - sl, "%s=%s;", c->id, c->version);
        if (w > 0 && (size_t)w < sizeof s_sig - sl) sl += (size_t)w;
    }
}

/* ---------------------------------------------------------------- API */

void app_updates_check_now(void) {
    if (settings.client_mode) return;   /* apps are managed on the master */
    static char buf[16384];
    char url[256];
    snprintf(url, sizeof url, "%s?t=%ld", CATALOG_URL, (long)time(NULL));
    buf[0] = 0;
    int rc = http_fetch_secure(url, buf, sizeof buf, 10);
    if (rc != 0 || !buf[0]) {
        fprintf(stderr, "[apps] catalog fetch failed (rc=%d)\n", rc);
        return;   /* keep the previous result */
    }
    cat_entry_t tmp[CAT_MAX];
    int n = parse_catalog(buf, tmp, CAT_MAX);
    if (n == 0) {
        fprintf(stderr, "[apps] catalog has no entries — ignored\n");
        return;
    }
    pthread_mutex_lock(&s_lock);
    memcpy(s_cat, tmp, sizeof(cat_entry_t) * (size_t)n);
    s_cat_n = n;
    recompute_locked();
    int cnt = s_upd_n;
    pthread_mutex_unlock(&s_lock);
    fprintf(stderr, "[apps] catalog: %d apps, %d update(s) available\n", n, cnt);
}

void app_updates_recompute(void) {
    pthread_mutex_lock(&s_lock);
    recompute_locked();
    pthread_mutex_unlock(&s_lock);
}

int app_updates_get(char * summary, size_t n) {
    pthread_mutex_lock(&s_lock);
    int cnt = s_upd_n;
    if (summary && n) {
        size_t l = 0;
        summary[0] = 0;
        for (int i = 0; i < s_upd_n && l + 1 < n; i++) {
            int w = snprintf(summary + l, n - l, "%s%s %s",
                             i ? ", " : "", s_upd[i].name, s_upd[i].version);
            if (w < 0) break;
            l += (size_t)w;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return cnt;
}

int app_updates_install_all(void) {
    struct stat st;
    if (stat(INSTALL_HELPER, &st) != 0 || !(st.st_mode & S_IXUSR)) {
        fprintf(stderr, "[apps] no install helper — cannot update apps\n");
        return 0;
    }
    upd_entry_t list[CAT_MAX];
    pthread_mutex_lock(&s_lock);
    int n = s_upd_n;
    memcpy(list, s_upd, sizeof(upd_entry_t) * (size_t)n);
    pthread_mutex_unlock(&s_lock);

    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (!sh_is_ident(list[i].id, 32)) continue;
        char cmd[256];
        snprintf(cmd, sizeof cmd, "%s %s", INSTALL_HELPER, list[i].id);
        fprintf(stderr, "[apps] updating %s to %s\n", list[i].id, list[i].version);
        if (system(cmd) == 0) ok++;
        else fprintf(stderr, "[apps] update of %s failed\n", list[i].id);
    }
    app_updates_recompute();
    return ok;
}

int app_updates_banner_wanted(void) {
    pthread_mutex_lock(&s_lock);
    load_dismissed();
    int want = s_upd_n > 0 && strcmp(s_sig, s_dismissed) != 0;
    pthread_mutex_unlock(&s_lock);
    return want;
}

void app_updates_dismiss_banner(void) {
    pthread_mutex_lock(&s_lock);
    load_dismissed();
    snprintf(s_dismissed, sizeof s_dismissed, "%s", s_sig);
    char path[256], tmp[272];
    snprintf(path, sizeof path, "%s/toonui_app_updates_seen", data_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE * f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "%s\n", s_dismissed);
        fclose(f);
        rename(tmp, path);
    }
    pthread_mutex_unlock(&s_lock);
}
