/* Tiny read-only JSON path lookup - see minijson.h. */
#include "minijson.h"
#include <stdlib.h>
#include <string.h>

#define MJ_MAX_DEPTH 32

static const char * ws(const char * p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* p at '"': returns the char after the closing quote, NULL if unterminated. */
static const char * skip_str(const char * p) {
    for (p++; *p; p++) {
        if (*p == '\\') { if (!*++p) return NULL; }
        else if (*p == '"') return p + 1;
    }
    return NULL;
}

/* p at the start of a value: returns the char after it, NULL if malformed. */
static const char * skip_val(const char * p, int depth) {
    if (depth > MJ_MAX_DEPTH) return NULL;
    p = ws(p);
    if (*p == '"') return skip_str(p);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        int obj = *p == '{';
        p = ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (obj) {
                if (*p != '"' || !(p = skip_str(p))) return NULL;
                p = ws(p);
                if (*p++ != ':') return NULL;
            }
            if (!(p = skip_val(p, depth + 1))) return NULL;
            p = ws(p);
            if (*p == ',') { p = ws(p + 1); continue; }
            if (*p == close) return p + 1;
            return NULL;
        }
    }
    /* number / true / false / null */
    const char * s = p;
    while (*p && !strchr(",}] \t\r\n", *p)) p++;
    return p > s ? p : NULL;
}

/* Compare the JSON string at p (at '"') with key[0..klen). Escapes in object
 * keys are rare; an escaped key simply does not match. */
static int key_eq(const char * p, const char * key, size_t klen) {
    return !strncmp(p + 1, key, klen) && p[1 + klen] == '"';
}

const char * mj_find(const char * json, const char * path, int * len) {
    if (!json) return NULL;
    const char * p = ws(json);
    const char * q = path ? path : "";
    while (*q) {
        if (*q == '.') { q++; continue; }
        if (*q == '[') {
            char * e;
            long want = strtol(q + 1, &e, 10);
            if (*e != ']' || want < 0 || *p != '[') return NULL;
            q = e + 1;
            p = ws(p + 1);
            if (*p == ']') return NULL;
            for (long i = 0;; i++) {
                if (i == want) break;
                if (!(p = skip_val(p, 0))) return NULL;
                p = ws(p);
                if (*p != ',') return NULL;
                p = ws(p + 1);
            }
            continue;
        }
        size_t klen = strcspn(q, ".[");
        if (*p != '{') return NULL;
        p = ws(p + 1);
        if (*p == '}') return NULL;
        for (;;) {
            if (*p != '"') return NULL;
            int hit = key_eq(p, q, klen);
            if (!(p = skip_str(p))) return NULL;
            p = ws(p);
            if (*p++ != ':') return NULL;
            p = ws(p);
            if (hit) break;
            if (!(p = skip_val(p, 0))) return NULL;
            p = ws(p);
            if (*p != ',') return NULL;
            p = ws(p + 1);
        }
        q += klen;
    }
    const char * e = skip_val(p, 0);
    if (!e) return NULL;
    if (len) *len = (int)(e - p);
    return p;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int mj_str(const char * json, const char * path, char * out, size_t n) {
    int len;
    const char * p = mj_find(json, path, &len);
    if (!p || !n) return 0;
    size_t o = 0;
    if (*p != '"') {
        if (*p == '{' || *p == '[') return 0;
        while (len-- > 0 && o + 1 < n) out[o++] = *p++;
        out[o] = 0;
        return 1;
    }
    for (p++; *p && *p != '"' && o + 1 < n; p++) {
        if (*p != '\\') { out[o++] = *p; continue; }
        p++;
        switch (*p) {
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'r': out[o++] = '\r'; break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 1; i <= 4; i++) {
                    int h = hexv(p[i]);
                    if (h < 0) { v = '?'; break; }
                    v = v * 16 + (unsigned)h;
                }
                if (hexv(p[1]) >= 0 && hexv(p[2]) >= 0 && hexv(p[3]) >= 0 && hexv(p[4]) >= 0) p += 4;
                if (v < 0x80) out[o++] = (char)v;
                else if (v < 0x800 && o + 2 < n) {
                    out[o++] = (char)(0xC0 | (v >> 6));
                    out[o++] = (char)(0x80 | (v & 0x3F));
                } else if (o + 3 < n) {
                    out[o++] = (char)(0xE0 | (v >> 12));
                    out[o++] = (char)(0x80 | ((v >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (v & 0x3F));
                }
                break;
            }
            case 0: out[o] = 0; return 1;
            default: out[o++] = *p; break;   /* \" \\ \/ */
        }
    }
    out[o] = 0;
    return 1;
}

int mj_num(const char * json, const char * path, double * out) {
    char buf[48];
    if (!mj_str(json, path, buf, sizeof buf) || !buf[0]) return 0;
    char * e;
    double v = strtod(buf, &e);
    if (e == buf) return 0;
    *out = v;
    return 1;
}

int mj_bool(const char * json, const char * path) {
    char buf[16];
    if (!mj_str(json, path, buf, sizeof buf)) return 0;
    if (!strcmp(buf, "true")) return 1;
    return atof(buf) != 0.0;
}
