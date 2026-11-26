/* Manifest-declared app settings with a generic web form - see app_settings.h. */
#include "app_settings.h"
#include "minijson.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AS_MAX_KEYS   32
#define AS_KEY_LEN    40
#define AS_VAL_LEN    2400
#define AS_MANIFEST_MAX (64 * 1024)

typedef struct {
    int  n;
    char k[AS_MAX_KEYS][AS_KEY_LEN];
    char v[AS_MAX_KEYS][AS_VAL_LEN];
} kv_t;

static const char * integ_root(void) {
    const char * r = getenv("FT_INTEG_ROOT");     /* sim / tests */
    return (r && *r) ? r : "/mnt/data/integrations";
}

static int id_ok(const char * id) {
    if (!id || !*id || strlen(id) > 40) return 0;
    for (const char * p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}

int app_settings_dir(const char * id, char * out, size_t n) {
    if (!id_ok(id)) return -1;
    snprintf(out, n, "%s/%s", integ_root(), id);
    return 0;
}

static char * read_all(const char * path, size_t max) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    char * b = malloc(max + 1);
    if (!b) { fclose(f); return NULL; }
    size_t n = fread(b, 1, max, f);
    fclose(f);
    b[n] = 0;
    return b;
}

static char * manifest(const char * id) {
    char dir[256], path[300];
    if (app_settings_dir(id, dir, sizeof dir)) return NULL;
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    return read_all(path, AS_MANIFEST_MAX);
}

/* settings.file, checked to be a plain file name */
static int conf_path(const char * id, const char * man, char * out, size_t n) {
    char file[64] = "", dir[256];
    if (!mj_str(man, "settings.file", file, sizeof file) || !file[0]) return -1;
    if (strchr(file, '/') || file[0] == '.') return -1;
    if (app_settings_dir(id, dir, sizeof dir)) return -1;
    snprintf(out, n, "%s/%s", dir, file);
    return 0;
}

static int kv_find(const kv_t * kv, const char * k) {
    for (int i = 0; i < kv->n; i++) if (!strcmp(kv->k[i], k)) return i;
    return -1;
}

static void kv_set(kv_t * kv, const char * k, const char * v) {
    int i = kv_find(kv, k);
    if (i < 0) {
        if (kv->n >= AS_MAX_KEYS) return;
        i = kv->n++;
        snprintf(kv->k[i], AS_KEY_LEN, "%s", k);
    }
    snprintf(kv->v[i], AS_VAL_LEN, "%s", v);
}

static void kv_del(kv_t * kv, const char * k) {
    int i = kv_find(kv, k);
    if (i < 0) return;
    kv->n--;
    if (i != kv->n) {
        memcpy(kv->k[i], kv->k[kv->n], AS_KEY_LEN);
        memcpy(kv->v[i], kv->v[kv->n], AS_VAL_LEN);
    }
}

static void kv_load(const char * path, kv_t * kv) {
    kv->n = 0;
    FILE * f = fopen(path, "r");
    if (!f) return;
    char line[AS_VAL_LEN + AS_KEY_LEN + 4];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char * eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq == line) continue;
        *eq = 0;
        if (strlen(line) >= AS_KEY_LEN) continue;
        kv_set(kv, line, eq + 1);
    }
    fclose(f);
}

static int kv_store(const char * path, const kv_t * kv) {
    char tmp[336];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    fchmod(fd, 0600);
    FILE * f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return -1; }
    fputs("# Written by FreeToon (web page :10081). Secrets are stored here in plain text.\n", f);
    for (int i = 0; i < kv->n; i++) fprintf(f, "%s=%s\n", kv->k[i], kv->v[i]);
    int bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    if (bad || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int app_settings_get(const char * id, const char * key, char * out, size_t n) {
    out[0] = 0;
    char * man = manifest(id);
    if (!man) return 0;
    char path[320];
    int found = 0;
    if (!conf_path(id, man, path, sizeof path)) {
        kv_t * kv = malloc(sizeof *kv);
        if (kv) {
            kv_load(path, kv);
            int i = kv_find(kv, key);
            if (i >= 0) { snprintf(out, n, "%s", kv->v[i]); found = 1; }
            free(kv);
        }
    }
    free(man);
    return found;
}

/* ------------------------------------------------------------ form body */
static int hexd(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decoded value of `key` in an application/x-www-form-urlencoded body. */
static int fget(const char * body, const char * key, char * out, size_t n) {
    size_t kl = strlen(key);
    for (const char * p = body; p && *p; ) {
        const char * amp = strchr(p, '&');
        const char * end = amp ? amp : p + strlen(p);
        const char * eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == kl && !strncmp(p, key, kl)) {
            size_t o = 0;
            for (const char * s = eq + 1; s < end && o + 1 < n; s++) {
                if (*s == '+') out[o++] = ' ';
                else if (*s == '%' && s + 2 < end && hexd(s[1]) >= 0 && hexd(s[2]) >= 0) {
                    out[o++] = (char)(hexd(s[1]) * 16 + hexd(s[2]));
                    s += 2;
                } else out[o++] = *s;
            }
            out[o] = 0;
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    out[0] = 0;
    return 0;
}

static void clean(char * s) {
    char * w = s;
    for (char * r = s; *r; r++) if (*r != '\r' && *r != '\n') *w++ = *r;
    *w = 0;
    char * b = s;
    while (*b == ' ' || *b == '\t') b++;
    memmove(s, b, strlen(b) + 1);
    for (size_t L = strlen(s); L && (s[L - 1] == ' ' || s[L - 1] == '\t'); ) s[--L] = 0;
}

/* -------------------------------------------------------------- render */
static void ap(char * buf, size_t n, int * pos, const char * fmt, ...) {
    if (*pos < 0 || (size_t)*pos >= n) return;
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(buf + *pos, n - (size_t)*pos, fmt, a);
    va_end(a);
    if (k < 0) return;
    *pos = ((size_t)(*pos + k) >= n) ? (int)n - 1 : *pos + k;
}

static void esc(char * dst, size_t n, const char * s) {
    size_t o = 0;
    for (; s && *s && o + 7 < n; s++) {
        switch (*s) {
        case '&':  memcpy(dst + o, "&amp;", 5);  o += 5; break;
        case '<':  memcpy(dst + o, "&lt;", 4);   o += 4; break;
        case '>':  memcpy(dst + o, "&gt;", 4);   o += 4; break;
        case '"':  memcpy(dst + o, "&quot;", 6); o += 6; break;
        case '\'': memcpy(dst + o, "&#39;", 5);  o += 5; break;
        default:   dst[o++] = *s;
        }
    }
    if (n) dst[o] = 0;
}

static int key_ok(const char * k) {
    if (!*k) return 0;
    for (const char * p = k; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    return strlen(k) < AS_KEY_LEN - 6;   /* room for "clear_" */
}

/* Template expansion for manifest texts:
 *   {REDIRECT}  the OAuth callback on this Toon (redirect; given by the page)
 *   {KEY}       the stored value of field KEY, or its default
 * url = 1 percent-encodes the inserted values (for a link). Returns 0 when a
 * placeholder had no value (a link is then not usable yet). */
static void pct(char * dst, size_t n, size_t * o, const char * s) {
    static const char hx[] = "0123456789ABCDEF";
    for (; *s && *o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr("-_.~", c))
            dst[(*o)++] = (char)c;
        else { dst[(*o)++] = '%'; dst[(*o)++] = hx[c >> 4]; dst[(*o)++] = hx[c & 15]; }
    }
}
static int field_default(const char * man, const char * key, char * out, size_t n) {
    for (int i = 0; i < 40; i++) {
        char fp[32], k[AS_KEY_LEN];
        snprintf(fp, sizeof fp, "settings.fields[%d]", i);
        const char * f = mj_find(man, fp, NULL);
        if (!f) break;
        if (mj_str(f, "key", k, sizeof k) && !strcmp(k, key)) return mj_str(f, "default", out, n);
    }
    out[0] = 0;
    return 0;
}
static int expand(char * dst, size_t n, const char * src, const char * man, const kv_t * kv,
                  const char * redirect, int url, int depth) {
    size_t o = 0;
    int all = 1;
    for (const char * p = src; *p && o + 1 < n; ) {
        const char * e = (*p == '{') ? strchr(p, '}') : NULL;
        if (e && e - p - 1 > 0 && e - p - 1 < AS_KEY_LEN) {
            char key[AS_KEY_LEN];
            memcpy(key, p + 1, (size_t)(e - p - 1));
            key[e - p - 1] = 0;
            if (key_ok(key)) {
                static char val[AS_VAL_LEN];
                val[0] = 0;
                if (!strcmp(key, "REDIRECT")) snprintf(val, sizeof val, "%s", redirect ? redirect : "");
                else {
                    int ci = kv ? kv_find(kv, key) : -1;
                    if (ci >= 0 && kv->v[ci][0]) snprintf(val, sizeof val, "%s", kv->v[ci]);
                    else if (depth < 1) {
                        char d[256];
                        if (field_default(man, key, d, sizeof d) && d[0]) {
                            char x[AS_VAL_LEN];
                            expand(x, sizeof x, d, man, kv, redirect, 0, depth + 1);
                            snprintf(val, sizeof val, "%s", x);
                        }
                    }
                }
                if (!val[0]) all = 0;
                if (url) pct(dst, n, &o, val);
                else for (const char * v = val; *v && o + 1 < n; v++) dst[o++] = *v;
                p = e + 1;
                continue;
            }
        }
        dst[o++] = *p++;
    }
    dst[o] = 0;
    return all;
}

int app_settings_form(const char * id, const char * action, const char * redirect, char * buf, size_t n, int * pos) {
    char * man = manifest(id);
    if (!man) return -1;
    char path[320];
    if (!mj_find(man, "settings.fields", NULL) || conf_path(id, man, path, sizeof path)) { free(man); return -1; }
    kv_t * kv = malloc(sizeof *kv);
    if (!kv) { free(man); return -1; }
    kv_load(path, kv);

    char t[600], e[1200];
    if (mj_str(man, "settings.intro", t, sizeof t) && t[0]) { esc(e, sizeof e, t); ap(buf, n, pos, "<p>%s</p>", e); }
    esc(e, sizeof e, action);
    ap(buf, n, pos, "<form method=\"post\" action=\"%s\" autocomplete=\"off\">", e);
    for (int i = 0; i < 40; i++) {
        char fp[32];
        snprintf(fp, sizeof fp, "settings.fields[%d]", i);
        const char * f = mj_find(man, fp, NULL);
        if (!f) break;
        char key[AS_KEY_LEN], type[16] = "text", label[160] = "", show[160] = "", help[600] = "", ph[160] = "", dflt[256] = "";
        if (!mj_str(f, "key", key, sizeof key) || !key_ok(key)) continue;
        mj_str(f, "type", type, sizeof type);
        mj_str(f, "label", label, sizeof label);
        mj_str(f, "show", show, sizeof show);
        mj_str(f, "help", help, sizeof help);
        mj_str(f, "placeholder", ph, sizeof ph);
        mj_str(f, "default", dflt, sizeof dflt);
        if (strchr(dflt, '{')) { char x[256]; expand(x, sizeof x, dflt, man, NULL, redirect, 0, 1); snprintf(dflt, sizeof dflt, "%s", x); }
        if (strchr(help, '{')) { char x[600]; expand(x, sizeof x, help, man, kv, redirect, 0, 0); snprintf(help, sizeof help, "%s", x); }
        int ci = kv_find(kv, key);
        const char * cur = ci >= 0 ? kv->v[ci] : dflt;

        /* "link": a button to an https:// address built from the stored
         * values (e.g. an OAuth authorize URL with the client id and the
         * Toon's callback). Not a value: nothing is stored for it. */
        if (!strcmp(type, "link")) {
            char tmpl[600] = "";
            static char url[2048];
            mj_str(f, "url", tmpl, sizeof tmpl);
            if (strncmp(tmpl, "https://", 8)) continue;
            int ready = expand(url, sizeof url, tmpl, man, kv, redirect, 1, 0);
            esc(e, sizeof e, show);
            ap(buf, n, pos, "<div class=\"row\"%s%s%s>", show[0] ? " data-show=\"" : "", show[0] ? e : "", show[0] ? "\"" : "");
            esc(e, sizeof e, label[0] ? label : key);
            if (ready) {
                static char eu[4096];
                esc(eu, sizeof eu, url);
                ap(buf, n, pos, "<a class=\"btn\" href=\"%s\" rel=\"noreferrer\">%s</a>", eu, e);
            } else {
                ap(buf, n, pos, "<span class=\"btn btn-sec\" style=\"opacity:.6;cursor:default\">%s</span>"
                                "<p class=\"help\">Vul eerst de velden hierboven in en klik op Opslaan.</p>", e);
            }
            if (help[0]) { esc(e, sizeof e, help); ap(buf, n, pos, "<p class=\"help\">%s</p>", e); }
            ap(buf, n, pos, "</div>");
            continue;
        }

        esc(e, sizeof e, show);
        ap(buf, n, pos, "<div class=\"row\"%s%s%s>", show[0] ? " data-show=\"" : "", show[0] ? e : "", show[0] ? "\"" : "");
        esc(e, sizeof e, label[0] ? label : key);
        ap(buf, n, pos, "<label>%s</label>", e);
        if (!strcmp(type, "select")) {
            ap(buf, n, pos, "<select name=\"%s\">", key);
            for (int j = 0; j < 40; j++) {
                char op[32], ov[96] = "", ol[160] = "";
                snprintf(op, sizeof op, "options[%d]", j);
                const char * o = mj_find(f, op, NULL);
                if (!o) break;
                mj_str(o, "value", ov, sizeof ov);
                mj_str(o, "label", ol, sizeof ol);
                char ev[200], el[340];
                esc(ev, sizeof ev, ov);
                esc(el, sizeof el, ol[0] ? ol : ov);
                ap(buf, n, pos, "<option value=\"%s\"%s>%s</option>", ev, strcmp(ov, cur) ? "" : " selected", el);
            }
            ap(buf, n, pos, "</select>");
        } else if (!strcmp(type, "secret")) {
            int set = ci >= 0 && kv->v[ci][0];
            char phs[300];
            if (set) snprintf(phs, sizeof phs, "ingesteld - leeg laten om te houden");
            else if (dflt[0]) snprintf(phs, sizeof phs, "standaard: %s", dflt);
            else snprintf(phs, sizeof phs, "%s", ph);
            esc(e, sizeof e, phs);
            ap(buf, n, pos, "<input type=\"password\" name=\"%s\" placeholder=\"%s\" autocomplete=\"new-password\">", key, e);
            if (set) ap(buf, n, pos, "<span class=\"cb\"><input type=\"checkbox\" name=\"clear_%s\" value=\"1\" id=\"c_%s\">"
                                     "<label for=\"c_%s\">wissen</label></span>", key, key, key);
        } else {
            char ec[AS_VAL_LEN * 2];
            esc(ec, sizeof ec, cur);
            esc(e, sizeof e, ph);
            ap(buf, n, pos, "<input type=\"text\" name=\"%s\" value=\"%s\" placeholder=\"%s\">", key, ec, e);
        }
        if (help[0]) { esc(e, sizeof e, help); ap(buf, n, pos, "<p class=\"help\">%s</p>", e); }
        ap(buf, n, pos, "</div>");
    }
    ap(buf, n, pos,
        "<button class=\"btn\">Opslaan</button></form>"
        "<script>(function(){function u(){document.querySelectorAll('[data-show]').forEach(function(r){"
        "var s=r.getAttribute('data-show'),i=s.indexOf('='),k=s.slice(0,i),v=s.slice(i+1).split('|'),"
        "e=document.querySelector('[name=\"'+k+'\"]');r.style.display=(!e||v.indexOf(e.value)>=0)?'':'none';});}"
        "document.querySelectorAll('select').forEach(function(s){s.addEventListener('change',u);});u();})();</script>");
    free(kv);
    free(man);
    return 0;
}

/* ---------------------------------------------------------------- save */
static int in_list(const char * list, const char * key) {
    size_t kl = strlen(key);
    for (const char * p = list; *p; ) {
        size_t L = strcspn(p, ",");
        if (L == kl && !strncmp(p, key, kl)) return 1;
        p += L;
        if (*p == ',') p++;
    }
    return 0;
}

int app_settings_save(const char * id, const char * body, char * msg, size_t msgn) {
    msg[0] = 0;
    char * man = manifest(id);
    if (!man) { snprintf(msg, msgn, "App niet gevonden."); return -1; }
    char path[320];
    if (!mj_find(man, "settings.fields", NULL) || conf_path(id, man, path, sizeof path)) {
        free(man);
        snprintf(msg, msgn, "Deze app heeft geen instellingen.");
        return -1;
    }
    kv_t * old = malloc(sizeof *old), * nw = malloc(sizeof *nw);
    char * val = malloc(AS_VAL_LEN);
    int rc = -1;
    if (!old || !nw || !val) { snprintf(msg, msgn, "Geen geheugen."); goto out; }
    kv_load(path, old);
    *nw = *old;

    /* 1. new values */
    for (int i = 0; i < 40; i++) {
        char fp[32], key[AS_KEY_LEN], type[16] = "text", ck[AS_KEY_LEN + 8], tmp[8];
        snprintf(fp, sizeof fp, "settings.fields[%d]", i);
        const char * f = mj_find(man, fp, NULL);
        if (!f) break;
        if (!mj_str(f, "key", key, sizeof key) || !key_ok(key)) continue;
        mj_str(f, "type", type, sizeof type);
        int given = fget(body, key, val, AS_VAL_LEN);
        clean(val);
        if (!strcmp(type, "secret")) {
            snprintf(ck, sizeof ck, "clear_%s", key);
            if (fget(body, ck, tmp, sizeof tmp) && tmp[0] == '1') kv_del(nw, key);
            else if (val[0]) kv_set(nw, key, val);
            continue;
        }
        if (!given) continue;               /* field not in this form: keep */
        if (!strcmp(type, "select")) {
            int okv = 0;
            for (int j = 0; j < 40 && !okv; j++) {
                char op[32], ov[96] = "";
                snprintf(op, sizeof op, "options[%d]", j);
                const char * o = mj_find(f, op, NULL);
                if (!o) break;
                mj_str(o, "value", ov, sizeof ov);
                okv = !strcmp(ov, val);
            }
            if (!okv) { snprintf(msg, msgn, "Ongeldige keuze voor %s.", key); goto out; }
        }
        kv_set(nw, key, val);
    }

    /* 2. a changed guard field (an address) needs its secrets typed again,
     *    so a stored password or token is never sent to a new address */
    for (int i = 0; i < 40; i++) {
        char fp[32], key[AS_KEY_LEN], guards[200] = "", label[160] = "";
        snprintf(fp, sizeof fp, "settings.fields[%d]", i);
        const char * f = mj_find(man, fp, NULL);
        if (!f) break;
        if (!mj_str(f, "key", key, sizeof key) || !mj_str(f, "guards", guards, sizeof guards) || !guards[0]) continue;
        int oi = kv_find(old, key), ni = kv_find(nw, key);
        const char * ov = oi >= 0 ? old->v[oi] : "";
        const char * nv = ni >= 0 ? nw->v[ni] : "";
        if (!ov[0] || !strcmp(ov, nv)) continue;
        mj_str(f, "label", label, sizeof label);
        for (int j = 0; j < 40; j++) {
            char sp[32], sk[AS_KEY_LEN], sl[160] = "";
            snprintf(sp, sizeof sp, "settings.fields[%d]", j);
            const char * s = mj_find(man, sp, NULL);
            if (!s) break;
            if (!mj_str(s, "key", sk, sizeof sk) || !in_list(guards, sk)) continue;
            int was = kv_find(old, sk);
            if (was < 0 || !old->v[was][0]) continue;          /* nothing stored */
            int still = kv_find(nw, sk);
            if (still < 0) continue;                           /* cleared */
            if (strcmp(old->v[was], nw->v[still])) continue;   /* re-entered (new value) */
            fget(body, sk, val, AS_VAL_LEN);
            clean(val);
            if (val[0]) continue;                              /* re-entered (same value) */
            mj_str(s, "label", sl, sizeof sl);
            snprintf(msg, msgn, "%s is veranderd: vul ook '%s' opnieuw in (of vink wissen aan).",
                     label[0] ? label : key, sl[0] ? sl : sk);
            goto out;
        }
    }

    if (kv_store(path, nw)) { snprintf(msg, msgn, "Opslaan mislukt."); goto out; }
    rc = 0;
out:
    free(old);
    free(nw);
    free(val);
    free(man);
    return rc;
}

int app_settings_find_solar(char * id, size_t idn, char * name, size_t namen) {
    DIR * d = opendir(integ_root());
    if (!d) return 0;
    struct dirent * de;
    int found = 0;
    while (!found && (de = readdir(d)) != NULL) {
        if (!id_ok(de->d_name)) continue;
        char * man = manifest(de->d_name);
        if (!man) continue;
        if (mj_bool(man, "solar_source") && mj_find(man, "settings.fields", NULL)) {
            snprintf(id, idn, "%s", de->d_name);
            if (!mj_str(man, "name", name, namen)) snprintf(name, namen, "%s", de->d_name);
            found = 1;
        }
        free(man);
    }
    closedir(d);
    return found;
}
