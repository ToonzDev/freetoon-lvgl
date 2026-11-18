/*
 * clock_studio.c — Klok Studio (#55): configurable clock faces.
 * See clock_studio.h for the overview.
 */
#include "clock_studio.h"
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "layout.h"
#include "calendar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

cs_cfg_t g_cs_cfg[CS_NCFG];
static int g_cs_loaded = 0;

/* ======================================================================== */
/* Themes                                                                   */
/* ======================================================================== */
/* bg, fg, accent, hour hand, minute hand, second hand */
static const uint32_t THEMES[CS_THEME_COUNT][CS_COL_COUNT] = {
    [CS_THEME_BLUE]   = { 0x0f2238, 0xe6eef7, 0x4aa3ff, 0xe6eef7, 0xe6eef7, 0xff5a5a },
    [CS_THEME_PURPLE] = { 0x1e1433, 0xefe7ff, 0xa77bff, 0xefe7ff, 0xefe7ff, 0xff7ad9 },
    [CS_THEME_WHITE]  = { 0xf4f5f7, 0x1c2430, 0x2f6fd6, 0x1c2430, 0x1c2430, 0xd93a3a },
    [CS_THEME_RED]    = { 0x2a0e12, 0xffeceb, 0xff4d4d, 0xffeceb, 0xffeceb, 0xffc94d },
    [CS_THEME_GREEN]  = { 0x0e2418, 0xe6fff0, 0x3ccf7a, 0xe6fff0, 0xe6fff0, 0xffd24a },
};

static const char * theme_name(int t) {
    switch (t) {
        case CS_THEME_BLUE:   return tr("Blauw", "Blue");
        case CS_THEME_PURPLE: return tr("Paars", "Purple");
        case CS_THEME_WHITE:  return tr("Wit", "White");
        case CS_THEME_RED:    return tr("Rood", "Red");
        case CS_THEME_GREEN:  return tr("Groen", "Green");
    }
    return "?";
}

const char * cs_face_name(int face) {
    switch (face) {
        case CS_FACE_AUTO:     return tr("Automatisch", "Automatic");
        case CS_FACE_ANALOG:   return tr("Klassiek analoog", "Classic analog");
        case CS_FACE_MINIMAL:  return tr("Minimaal analoog", "Minimal analog");
        case CS_FACE_SQUARE:   return tr("Vierkant analoog", "Square analog");
        case CS_FACE_LED:      return tr("LED digitaal", "LED digital");
        case CS_FACE_STACKED:  return tr("Gestapeld", "Stacked");
        case CS_FACE_DIGITAL:  return tr("Digitaal", "Digital");
        case CS_FACE_CALENDAR: return tr("Agenda", "Calendar");
    }
    return "?";
}

/* ======================================================================== */
/* Config file                                                              */
/* ======================================================================== */
static const char * cs_dir(void) {
    const char * d = getenv("TOONUI_DATA_DIR");
    if (d && *d) return d;
    if (access("/mnt/data", W_OK) == 0) return "/mnt/data";
    return ".";
}

static void cs_defaults(int which, cs_cfg_t * c) {
    memset(c, 0, sizeof *c);
    c->face    = CS_FACE_AUTO;
    c->theme   = CS_THEME_BLUE;
    c->seconds = (which == CS_HOME);   /* dim: no ticking second hand by default */
    c->date    = 1;
    c->h12     = 0;
    c->scale   = 100;
    c->radius  = (which == CS_HOME) ? 14 : 0;
    for (int i = 0; i < CS_COL_COUNT; i++) c->col[i] = -1;
}

static const char * COL_KEYS[CS_COL_COUNT] = { "bg", "fg", "accent", "hour", "min", "sec" };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void cs_load(void) {
    for (int w = 0; w < CS_NCFG; w++) cs_defaults(w, &g_cs_cfg[w]);
    g_cs_loaded = 1;
    char path[256];
    snprintf(path, sizeof path, "%s/toonui_clockstudio.cfg", cs_dir());
    FILE * f = fopen(path, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        int w;
        char * k = line;
        if (!strncmp(k, "home.", 5)) { w = CS_HOME; k += 5; }
        else if (!strncmp(k, "dim.", 4)) { w = CS_DIM; k += 4; }
        else continue;
        char * eq = strchr(k, '=');
        if (!eq) continue;
        *eq = 0;
        const char * v = eq + 1;
        cs_cfg_t * c = &g_cs_cfg[w];
        long iv = strtol(v, NULL, 0);
        if      (!strcmp(k, "face"))    c->face    = clampi((int)iv, 0, CS_FACE_COUNT - 1);
        else if (!strcmp(k, "theme"))   c->theme   = clampi((int)iv, 0, CS_THEME_COUNT - 1);
        else if (!strcmp(k, "seconds")) c->seconds = iv ? 1 : 0;
        else if (!strcmp(k, "date"))    c->date    = iv ? 1 : 0;
        else if (!strcmp(k, "h12"))     c->h12     = iv ? 1 : 0;
        else if (!strcmp(k, "scale"))   c->scale   = clampi((int)iv, 65, 100);
        else if (!strcmp(k, "radius"))  c->radius  = clampi((int)iv, 0, 40);
        else if (!strcmp(k, "title"))   snprintf(c->title, sizeof c->title, "%s", v);
        else {
            for (int i = 0; i < CS_COL_COUNT; i++)
                if (!strcmp(k, COL_KEYS[i]))
                    c->col[i] = (iv < 0) ? -1 : (int)(iv & 0xffffff);
        }
    }
    fclose(f);
}

void cs_save(void) {
    char path[256], tmp[264];
    snprintf(path, sizeof path, "%s/toonui_clockstudio.cfg", cs_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE * f = fopen(tmp, "w");
    if (!f) return;
    for (int w = 0; w < CS_NCFG; w++) {
        const char * p = (w == CS_HOME) ? "home" : "dim";
        const cs_cfg_t * c = &g_cs_cfg[w];
        fprintf(f, "%s.face=%d\n%s.theme=%d\n%s.seconds=%d\n%s.date=%d\n%s.h12=%d\n"
                   "%s.scale=%d\n%s.radius=%d\n%s.title=%s\n",
                p, c->face, p, c->theme, p, c->seconds, p, c->date, p, c->h12,
                p, c->scale, p, c->radius, p, c->title);
        for (int i = 0; i < CS_COL_COUNT; i++) {
            if (c->col[i] < 0) fprintf(f, "%s.%s=-1\n", p, COL_KEYS[i]);
            else               fprintf(f, "%s.%s=0x%06x\n", p, COL_KEYS[i], c->col[i]);
        }
    }
    fclose(f);
    rename(tmp, path);
}

static void ensure_loaded(void) { if (!g_cs_loaded) cs_load(); }

/* ======================================================================== */
/* Live faces                                                               */
/* ======================================================================== */
#define CS_MAX_LIVE  8
#define CS_CAL_ROWS  6

typedef struct {
    lv_obj_t * box;            /* outer object (owned by the caller's parent) */
    lv_obj_t * draw;           /* custom-draw area (analog / LED), or NULL */
    lv_obj_t * l_title, * l_time, * l_time2, * l_sec, * l_date;
    lv_obj_t * l_cal[CS_CAL_ROWS];
    int n_cal;
    int which, ctx, face;      /* face = resolved (never AUTO) */
    int w, h;
    int transparent;           /* dim block without explicit background */
    /* resolved colours */
    lv_color_t c_bg, c_fg, c_acc, c_hh, c_mh, c_sh;
    /* analog geometry, relative to `draw` */
    int cx, cy, r;
    lv_area_t inv_sec;         /* last second-hand bbox (absolute) */
    int inv_sec_valid;
    /* LED geometry, relative to `draw` */
    int led_x0, led_y0, led_dw, led_dh, led_gap, led_cw, led_t;
    lv_area_t colon_area;      /* absolute */
    /* state */
    int hh, mm, ss, mday;
    long last_t;
    int cal_sig;
} cs_face_t;

static cs_face_t * g_live[CS_MAX_LIVE];
static lv_timer_t * g_tick = NULL;

static lv_color_t cfg_color(const cs_cfg_t * c, int slot, int transparent) {
    if (c->col[slot] >= 0) return lv_color_hex((uint32_t)c->col[slot]);
    uint32_t v = THEMES[c->theme][slot];
    /* The white theme has dark ink; on the black dim screen (no own
     * background) that would be invisible — use light ink there. */
    if (transparent && c->theme == CS_THEME_WHITE &&
        (slot == CS_COL_FG || slot == CS_COL_HOUR || slot == CS_COL_MIN))
        v = 0xf4f5f7;
    return lv_color_hex(v);
}

static void now_tm(struct tm * tm) {
    time_t t = time(NULL);
    localtime_r(&t, tm);
#ifdef SIM
    /* Deterministic renders: TOONUI_SIM_TIME="10:09:30" */
    const char * st = getenv("TOONUI_SIM_TIME");
    int h, m, s;
    if (st && sscanf(st, "%d:%d:%d", &h, &m, &s) == 3) {
        tm->tm_hour = h; tm->tm_min = m; tm->tm_sec = s;
    }
#endif
}

/* ---- fonts -------------------------------------------------------------- */
static const lv_font_t * const FONTS_TXT[] = {
    &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_18,
    &lv_font_montserrat_20, &lv_font_montserrat_22, &lv_font_montserrat_24,
    &lv_font_montserrat_28, &lv_font_montserrat_36, &lv_font_montserrat_48,
};
/* digits, ':', '.', '-', space only */
static const lv_font_t * const FONTS_DIG[] = {
    &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_18,
    &lv_font_montserrat_20, &lv_font_montserrat_22, &lv_font_montserrat_24,
    &lv_font_montserrat_28, &lv_font_montserrat_36, &lv_font_montserrat_48,
    &lv_font_montserrat_64_custom, &lv_font_montserrat_76_bold,
    &lv_font_montserrat_96_custom,
};

/* Largest font in which `txt` fits maxw x maxh (smallest font otherwise). */
static const lv_font_t * fit_font(const char * txt, int maxw, int maxh, int digits) {
    const lv_font_t * const * L = digits ? FONTS_DIG : FONTS_TXT;
    int n = digits ? (int)(sizeof FONTS_DIG / sizeof FONTS_DIG[0])
                   : (int)(sizeof FONTS_TXT / sizeof FONTS_TXT[0]);
    for (int i = n - 1; i > 0; i--) {
        lv_point_t sz;
        lv_txt_get_size(&sz, txt, L[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= maxw && L[i]->line_height <= maxh) return L[i];
    }
    return L[0];
}

static lv_obj_t * mk_label(lv_obj_t * parent, const lv_font_t * f, lv_color_t c, const char * txt) {
    lv_obj_t * l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, txt);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t * mk_plain(lv_obj_t * parent) {
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* ---- time formatting ---------------------------------------------------- */
static int disp_hour(const cs_face_t * f, int h) {
    if (!g_cs_cfg[f->which].h12) return h;
    h %= 12;
    return h ? h : 12;
}

static void fmt_date(char * out, int n, const struct tm * tm, int longform) {
    if (longform) i18n_date_long(out, n, tm);
    else          i18n_date_short(out, n, tm);
}

static void date_text(const cs_face_t * f, const struct tm * tm, char * out, int n, int longform) {
    char d[48];
    fmt_date(d, sizeof d, tm, longform);
    if (g_cs_cfg[f->which].h12 && f->face != CS_FACE_ANALOG &&
        f->face != CS_FACE_MINIMAL && f->face != CS_FACE_SQUARE)
        snprintf(out, n, "%s  %s", d, tm->tm_hour < 12 ? "AM" : "PM");
    else
        snprintf(out, n, "%s", d);
}

/* ======================================================================== */
/* Analog                                                                   */
/* ======================================================================== */
static void hand_pts(const cs_face_t * f, int deg, int len, int tail,
                     const lv_area_t * a, lv_point_t * p1, lv_point_t * p2) {
    int32_t s = lv_trigo_sin((int16_t)(deg % 360));
    int32_t c = lv_trigo_cos((int16_t)(deg % 360));
    int cx = a->x1 + f->cx, cy = a->y1 + f->cy;
    p1->x = (lv_coord_t)(cx - (s * tail) / LV_TRIGO_SIN_MAX);
    p1->y = (lv_coord_t)(cy + (c * tail) / LV_TRIGO_SIN_MAX);
    p2->x = (lv_coord_t)(cx + (s * len) / LV_TRIGO_SIN_MAX);
    p2->y = (lv_coord_t)(cy - (c * len) / LV_TRIGO_SIN_MAX);
}

/* point on the dial outline at angle `deg`, `d` px inward (square aware) */
static void dial_pt(const cs_face_t * f, int deg, int rr, const lv_area_t * a, lv_point_t * p) {
    int32_t s = lv_trigo_sin((int16_t)(deg % 360));
    int32_t c = lv_trigo_cos((int16_t)(deg % 360));
    int cx = a->x1 + f->cx, cy = a->y1 + f->cy;
    if (f->face == CS_FACE_SQUARE) {
        int32_t as = s < 0 ? -s : s, ac = c < 0 ? -c : c;
        int32_t m = as > ac ? as : ac;
        if (m == 0) m = 1;
        p->x = (lv_coord_t)(cx + (int32_t)((int64_t)s * rr / m));
        p->y = (lv_coord_t)(cy - (int32_t)((int64_t)c * rr / m));
    } else {
        p->x = (lv_coord_t)(cx + (s * rr) / LV_TRIGO_SIN_MAX);
        p->y = (lv_coord_t)(cy - (c * rr) / LV_TRIGO_SIN_MAX);
    }
}

static void hand_dims(const cs_face_t * f, int which_hand, int * len, int * tail, int * width) {
    int r = f->r;
    int minimal = (f->face == CS_FACE_MINIMAL);
    switch (which_hand) {
        case 0: *len = r * 50 / 100; *tail = r * 8 / 100;
                *width = LV_MAX(3, r / (minimal ? 18 : 13)); break;
        case 1: *len = r * 78 / 100; *tail = r * 10 / 100;
                *width = LV_MAX(2, r / (minimal ? 26 : 20)); break;
        default: *len = r * 86 / 100; *tail = minimal ? 0 : r * 18 / 100;
                *width = LV_MAX(1, r / 70); break;
    }
}

static void sec_bbox(const cs_face_t * f, lv_area_t * out) {
    lv_area_t a;
    lv_obj_get_coords(f->draw, &a);
    int len, tail, w;
    hand_dims(f, 2, &len, &tail, &w);
    lv_point_t p1, p2;
    hand_pts(f, f->ss * 6, len, tail, &a, &p1, &p2);
    int m = w + 3;
    out->x1 = LV_MIN(p1.x, p2.x) - m; out->x2 = LV_MAX(p1.x, p2.x) + m;
    out->y1 = LV_MIN(p1.y, p2.y) - m; out->y2 = LV_MAX(p1.y, p2.y) + m;
    /* centre cap */
    int cr = LV_MAX(3, f->r / 16) + 2;
    int cx = a.x1 + f->cx, cy = a.y1 + f->cy;
    if (out->x1 > cx - cr) out->x1 = cx - cr;
    if (out->x2 < cx + cr) out->x2 = cx + cr;
    if (out->y1 > cy - cr) out->y1 = cy - cr;
    if (out->y2 < cy + cr) out->y2 = cy + cr;
}

static void draw_line(lv_draw_ctx_t * dc, lv_point_t p1, lv_point_t p2, int w,
                      lv_color_t c, lv_opa_t opa, int round) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c; d.width = (lv_coord_t)w; d.opa = opa;
    d.round_start = round; d.round_end = round;
    lv_draw_line(dc, &d, &p1, &p2);
}

static void draw_dot(lv_draw_ctx_t * dc, int x, int y, int rad, lv_color_t c) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c; d.bg_opa = LV_OPA_COVER; d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = { (lv_coord_t)(x - rad), (lv_coord_t)(y - rad),
                    (lv_coord_t)(x + rad), (lv_coord_t)(y + rad) };
    lv_draw_rect(dc, &d, &a);
}

/* Dial (ticks, numerals) — under the children (date label). */
static void analog_draw_main(lv_event_t * e) {
    cs_face_t * f = (cs_face_t *)lv_event_get_user_data(e);
    lv_draw_ctx_t * dc = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(f->draw, &a);
    int r = f->r;

    if (f->face == CS_FACE_MINIMAL) {
        for (int i = 0; i < 12; i++) {
            lv_point_t p;
            dial_pt(f, i * 30, r * 88 / 100, &a, &p);
            int big = (i % 3 == 0);
            draw_dot(dc, p.x, p.y, LV_MAX(big ? 3 : 2, r / (big ? 24 : 36)),
                     big ? f->c_acc : f->c_fg);
        }
        return;
    }

    /* outer ring (classic only; the square dial has its own box outline) */
    if (f->face == CS_FACE_ANALOG) {
        lv_draw_arc_dsc_t ad;
        lv_draw_arc_dsc_init(&ad);
        ad.color = f->c_acc; ad.width = (lv_coord_t)LV_MAX(2, r / 30);
        lv_point_t c = { (lv_coord_t)(a.x1 + f->cx), (lv_coord_t)(a.y1 + f->cy) };
        lv_draw_arc(dc, &ad, &c, (uint16_t)r, 0, 360);
    }
    int edge = (f->face == CS_FACE_ANALOG) ? r - LV_MAX(2, r / 30) - 2 : r - 4;
    for (int i = 0; i < 60; i++) {
        int major = (i % 5 == 0);
        if (!major && r < 55) continue;
        int len = major ? LV_MAX(4, r * 12 / 100) : LV_MAX(2, r * 5 / 100);
        lv_point_t p1, p2;
        dial_pt(f, i * 6, edge, &a, &p1);
        dial_pt(f, i * 6, edge - len, &a, &p2);
        draw_line(dc, p1, p2, major ? LV_MAX(2, r / 36) : 1, f->c_fg,
                  major ? LV_OPA_COVER : LV_OPA_60, 0);
    }
    /* numerals */
    int step = 0;
    if (r >= SX(110)) step = 1;           /* all 12 */
    else if (r >= SX(48)) step = 3;      /* 12 / 3 / 6 / 9 */
    if (step) {
        lv_draw_label_dsc_t ld;
        lv_draw_label_dsc_init(&ld);
        ld.color = f->c_fg;
        ld.font = fit_font("12", r * 26 / 100, r * 19 / 100, 1);
        ld.align = LV_TEXT_ALIGN_CENTER;
        int inner = edge - LV_MAX(4, r * 12 / 100) - LV_MAX(2, r / 30);   /* just inside the major ticks */
        for (int i = (step == 1 ? 1 : 3); i <= 12; i += step) {
            char t[4];
            snprintf(t, sizeof t, "%d", i % 100);
            lv_point_t sz;
            lv_txt_get_size(&sz, t, ld.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            /* half the label's extent towards the centre (digit glyphs fill
             * ~75 % of the line height), so wide "10"/"11" clear the ticks too */
            int32_t s = lv_trigo_sin((int16_t)(i * 30)), c = lv_trigo_cos((int16_t)(i * 30));
            if (s < 0) s = -s;
            if (c < 0) c = -c;
            int ext;
            if (f->face == CS_FACE_SQUARE)
                ext = (s > c) ? sz.x / 2 : sz.y * 75 / 200;
            else
                ext = (int)((s * sz.x / 2 + c * sz.y * 75 / 200) / LV_TRIGO_SIN_MAX);
            lv_point_t p;
            dial_pt(f, i * 30, inner - ext, &a, &p);
            lv_area_t ta = { (lv_coord_t)(p.x - sz.x / 2 - 1), (lv_coord_t)(p.y - sz.y / 2),
                             (lv_coord_t)(p.x + sz.x / 2 + 1), (lv_coord_t)(p.y + sz.y / 2) };
            lv_draw_label(dc, &ld, &ta, t, NULL);
        }
    }
}

/* Hands — over the children. */
static void analog_draw_post(lv_event_t * e) {
    cs_face_t * f = (cs_face_t *)lv_event_get_user_data(e);
    lv_draw_ctx_t * dc = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(f->draw, &a);
    int len, tail, w;
    lv_point_t p1, p2;

    hand_dims(f, 0, &len, &tail, &w);
    hand_pts(f, (f->hh % 12) * 30 + f->mm / 2, len, tail, &a, &p1, &p2);
    draw_line(dc, p1, p2, w, f->c_hh, LV_OPA_COVER, 1);

    hand_dims(f, 1, &len, &tail, &w);
    hand_pts(f, f->mm * 6, len, tail, &a, &p1, &p2);
    draw_line(dc, p1, p2, w, f->c_mh, LV_OPA_COVER, 1);

    if (g_cs_cfg[f->which].seconds) {
        hand_dims(f, 2, &len, &tail, &w);
        hand_pts(f, f->ss * 6, len, tail, &a, &p1, &p2);
        draw_line(dc, p1, p2, w, f->c_sh, LV_OPA_COVER, 1);
    }
    draw_dot(dc, a.x1 + f->cx, a.y1 + f->cy, LV_MAX(3, f->r / 16),
             g_cs_cfg[f->which].seconds ? f->c_sh : f->c_acc);
}

static void build_analog(cs_face_t * f, int x, int y, int w, int h, const struct tm * tm) {
    int side = LV_MIN(w, h);
    f->draw = mk_plain(f->box);
    lv_obj_set_pos(f->draw, x + (w - side) / 2, y + (h - side) / 2);
    lv_obj_set_size(f->draw, side, side);
    f->cx = side / 2; f->cy = side / 2;
    f->r  = side / 2 - 1;
    if (f->face != CS_FACE_MINIMAL) {
        /* dial plate a shade off the background */
        lv_obj_set_style_bg_color(f->draw, lv_color_mix(f->c_fg, f->c_bg, LV_OPA_10), 0);
        lv_obj_set_style_bg_opa(f->draw, f->transparent ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_radius(f->draw, f->face == CS_FACE_SQUARE ? side / 7 : LV_RADIUS_CIRCLE, 0);
        if (f->face == CS_FACE_SQUARE) {
            lv_obj_set_style_border_color(f->draw, f->c_acc, 0);
            lv_obj_set_style_border_width(f->draw, LV_MAX(2, side / 60), 0);
        }
    }
    lv_obj_add_event_cb(f->draw, analog_draw_main, LV_EVENT_DRAW_MAIN, f);
    lv_obj_add_event_cb(f->draw, analog_draw_post, LV_EVENT_DRAW_POST, f);

    if (g_cs_cfg[f->which].date) {
        char d[48];
        date_text(f, tm, d, sizeof d, 0);
        f->l_date = mk_label(f->draw, fit_font(d, f->r * 80 / 100, f->r * 14 / 100, 0), f->c_fg, d);
        lv_obj_set_style_text_opa(f->l_date, LV_OPA_80, 0);
        lv_obj_align(f->l_date, LV_ALIGN_CENTER, 0, f->r * 36 / 100);
    }
}

/* ======================================================================== */
/* LED 7-segment                                                            */
/* ======================================================================== */
static const uint8_t SEG_MASK[10] = {
    /* bit: a b c d e f g */
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
};

static void led_seg(lv_draw_ctx_t * dc, lv_draw_rect_dsc_t * d, int horiz,
                    int p, int q1, int q2, int t) {
    lv_point_t pt[6];
    int h = t / 2;
    if (horiz) {   /* p = y centre, q = x from..to */
        pt[0] = (lv_point_t){ (lv_coord_t)q1, (lv_coord_t)p };
        pt[1] = (lv_point_t){ (lv_coord_t)(q1 + h), (lv_coord_t)(p - h) };
        pt[2] = (lv_point_t){ (lv_coord_t)(q2 - h), (lv_coord_t)(p - h) };
        pt[3] = (lv_point_t){ (lv_coord_t)q2, (lv_coord_t)p };
        pt[4] = (lv_point_t){ (lv_coord_t)(q2 - h), (lv_coord_t)(p + h) };
        pt[5] = (lv_point_t){ (lv_coord_t)(q1 + h), (lv_coord_t)(p + h) };
    } else {       /* p = x centre, q = y from..to */
        pt[0] = (lv_point_t){ (lv_coord_t)p, (lv_coord_t)q1 };
        pt[1] = (lv_point_t){ (lv_coord_t)(p + h), (lv_coord_t)(q1 + h) };
        pt[2] = (lv_point_t){ (lv_coord_t)(p + h), (lv_coord_t)(q2 - h) };
        pt[3] = (lv_point_t){ (lv_coord_t)p, (lv_coord_t)q2 };
        pt[4] = (lv_point_t){ (lv_coord_t)(p - h), (lv_coord_t)(q2 - h) };
        pt[5] = (lv_point_t){ (lv_coord_t)(p - h), (lv_coord_t)(q1 + h) };
    }
    lv_draw_polygon(dc, d, pt, 6);
}

static void led_digit(lv_draw_ctx_t * dc, const cs_face_t * f, int x0, int y0, int digit) {
    int dw = f->led_dw, dh = f->led_dh, t = f->led_t;
    int g = LV_MAX(1, t / 5);
    uint8_t m = (digit >= 0 && digit <= 9) ? SEG_MASK[digit] : 0;
    lv_draw_rect_dsc_t on, off;
    lv_draw_rect_dsc_init(&on);
    on.bg_color = f->c_acc; on.bg_opa = LV_OPA_COVER;
    lv_draw_rect_dsc_init(&off);
    off.bg_color = f->c_fg; off.bg_opa = LV_OPA_10;
    int xl = x0 + t / 2, xr = x0 + dw - t / 2;
    int yt = y0 + t / 2, ym = y0 + dh / 2, yb = y0 + dh - t / 2;
    /* a b c d e f g */
    led_seg(dc, (m & 0x01) ? &on : &off, 1, yt, xl + g, xr - g, t);
    led_seg(dc, (m & 0x02) ? &on : &off, 0, xr, yt + g, ym - g, t);
    led_seg(dc, (m & 0x04) ? &on : &off, 0, xr, ym + g, yb - g, t);
    led_seg(dc, (m & 0x08) ? &on : &off, 1, yb, xl + g, xr - g, t);
    led_seg(dc, (m & 0x10) ? &on : &off, 0, xl, ym + g, yb - g, t);
    led_seg(dc, (m & 0x20) ? &on : &off, 0, xl, yt + g, ym - g, t);
    led_seg(dc, (m & 0x40) ? &on : &off, 1, ym, xl + g, xr - g, t);
}

static void led_draw(lv_event_t * e) {
    cs_face_t * f = (cs_face_t *)lv_event_get_user_data(e);
    lv_draw_ctx_t * dc = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(f->draw, &a);
    int h = disp_hour(f, f->hh);
    int d[4] = { h / 10, h % 10, f->mm / 10, f->mm % 10 };
    if (g_cs_cfg[f->which].h12 && d[0] == 0) d[0] = -1;   /* blank leading 0 */
    int x = a.x1 + f->led_x0, y = a.y1 + f->led_y0;
    for (int i = 0; i < 4; i++) {
        led_digit(dc, f, x, y, d[i]);
        x += f->led_dw + f->led_gap;
        if (i == 1) x += f->led_cw;
    }
    /* colon */
    int on = !g_cs_cfg[f->which].seconds || (f->ss % 2 == 0);
    lv_draw_rect_dsc_t cd;
    lv_draw_rect_dsc_init(&cd);
    cd.bg_color = on ? f->c_acc : f->c_fg;
    cd.bg_opa = on ? LV_OPA_COVER : LV_OPA_10;
    cd.radius = 2;
    int cs = f->led_t;
    /* between digit 2 (ends at x0+2dw+gap) and digit 3 (starts gap+cw later) */
    int ccx = a.x1 + f->led_x0 + 2 * f->led_dw + f->led_gap + (f->led_gap + f->led_cw) / 2;
    for (int k = 0; k < 2; k++) {
        int cy = a.y1 + f->led_y0 + (k ? f->led_dh * 70 / 100 : f->led_dh * 30 / 100);
        lv_area_t r = { (lv_coord_t)(ccx - cs / 2), (lv_coord_t)(cy - cs / 2),
                        (lv_coord_t)(ccx + cs / 2), (lv_coord_t)(cy + cs / 2) };
        lv_draw_rect(dc, &cd, &r);
    }
}

static void build_led(cs_face_t * f, int x, int y, int w, int h, const struct tm * tm) {
    int date_h = 0;
    char d[48] = "";
    const lv_font_t * df = NULL;
    if (g_cs_cfg[f->which].date) {
        date_text(f, tm, d, sizeof d, w > SX(300));
        df = fit_font(d, w, LV_MAX(h / 6, 14), 0);
        date_h = df->line_height + 4;
    }
    int ah = h - date_h;
    /* digit height dh, width dw = 0.56 dh, gap 0.16 dw, colon 0.5 dw */
    int dh = ah * 92 / 100;
    int total_w_per_dh_x100 = 4 * 56 + 3 * 9 + 28;   /* in hundredths of dh */
    if (dh * total_w_per_dh_x100 / 100 > w) dh = w * 100 / total_w_per_dh_x100;
    f->led_dh = dh;
    f->led_dw = dh * 56 / 100;
    f->led_gap = LV_MAX(2, dh * 9 / 100);
    f->led_cw = LV_MAX(4, dh * 28 / 100);
    f->led_t = LV_MAX(3, f->led_dw * 18 / 100);
    int tw = 4 * f->led_dw + 3 * f->led_gap + f->led_cw;
    f->draw = mk_plain(f->box);
    lv_obj_set_pos(f->draw, x, y);
    lv_obj_set_size(f->draw, w, ah);
    f->led_x0 = (w - tw) / 2;
    f->led_y0 = (ah - dh) / 2;
    lv_obj_add_event_cb(f->draw, led_draw, LV_EVENT_DRAW_MAIN, f);
    if (df) {
        f->l_date = mk_label(f->box, df, f->c_fg, d);
        lv_obj_set_style_text_opa(f->l_date, LV_OPA_80, 0);
        lv_obj_align(f->l_date, LV_ALIGN_TOP_MID, 0, y + ah + 2);
    }
}

static void led_colon_area(cs_face_t * f, lv_area_t * out) {
    lv_area_t a;
    lv_obj_get_coords(f->draw, &a);
    int x1 = a.x1 + f->led_x0 + 2 * f->led_dw + f->led_gap;
    out->x1 = x1 - 2; out->x2 = x1 + f->led_cw + f->led_gap + 2;
    out->y1 = a.y1 + f->led_y0; out->y2 = a.y1 + f->led_y0 + f->led_dh;
}

/* ======================================================================== */
/* Label faces: digital / stacked / calendar                                */
/* ======================================================================== */
static void set_time_labels(cs_face_t * f) {
    char t[16];
    int h = disp_hour(f, f->hh);
    if (f->face == CS_FACE_STACKED) {
        snprintf(t, sizeof t, "%02d", h);
        lv_label_set_text(f->l_time, t);
        snprintf(t, sizeof t, "%02d", f->mm);
        lv_label_set_text(f->l_time2, t);
    } else if (f->l_time) {
        if (g_cs_cfg[f->which].h12) snprintf(t, sizeof t, "%d:%02d", h, f->mm);
        else                        snprintf(t, sizeof t, "%02d:%02d", h, f->mm);
        lv_label_set_text(f->l_time, t);
    }
    if (f->l_sec) {
        snprintf(t, sizeof t, "%02d", f->ss);
        lv_label_set_text(f->l_sec, t);
    }
}

static void build_digital(cs_face_t * f, int x, int y, int w, int h, const struct tm * tm) {
    char d[48] = "";
    const lv_font_t * df = NULL;
    int date_h = 0;
    if (g_cs_cfg[f->which].date) {
        date_text(f, tm, d, sizeof d, 1);
        df = fit_font(d, w, LV_MAX(h / 5, 14), 0);
        if (lv_txt_get_width(d, (uint32_t)strlen(d), df, 0, LV_TEXT_FLAG_NONE) > w) {
            date_text(f, tm, d, sizeof d, 0);
            df = fit_font(d, w, LV_MAX(h / 5, 14), 0);
        }
        date_h = df->line_height;
    }
    int sec = g_cs_cfg[f->which].seconds;
    int tw = sec ? w * 80 / 100 : w;
    const lv_font_t * tf = fit_font("88:88", tw, h - date_h, 1);
    lv_obj_t * col = mk_plain(f->box);
    lv_obj_set_pos(col, x, y);
    lv_obj_set_size(col, w, h);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t * row = mk_plain(col);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, LV_MAX(2, tf->line_height / 12), 0);
    f->l_time = mk_label(row, tf, f->c_fg, "--:--");
    if (sec) {
        const lv_font_t * sf = fit_font("88", w * 18 / 100, tf->line_height * 45 / 100, 1);
        f->l_sec = mk_label(row, sf, f->c_acc, "--");
        lv_obj_set_style_pad_bottom(f->l_sec, tf->line_height / 8, 0);
    }
    if (df) {
        f->l_date = mk_label(col, df, f->c_fg, d);
        lv_obj_set_style_text_opa(f->l_date, LV_OPA_80, 0);
    }
}

static void build_stacked(cs_face_t * f, int x, int y, int w, int h, const struct tm * tm) {
    char d[48] = "";
    const lv_font_t * df = NULL;
    int date_h = 0;
    if (g_cs_cfg[f->which].date) {
        date_text(f, tm, d, sizeof d, 0);
        df = fit_font(d, w, LV_MAX(h / 8, 14), 0);
        date_h = df->line_height;
    }
    const lv_font_t * tf = fit_font("88", w, (h - date_h) * 55 / 100, 1);
    lv_obj_t * col = mk_plain(f->box);
    lv_obj_set_pos(col, x, y);
    lv_obj_set_size(col, w, h);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* the big digit fonts have generous line height: pull the rows together */
    lv_obj_set_style_pad_row(col, -(tf->line_height / 7), 0);
    f->l_time  = mk_label(col, tf, f->c_fg, "--");
    f->l_time2 = mk_label(col, tf, f->c_acc, "--");
    if (df) {
        f->l_date = mk_label(col, df, f->c_fg, d);
        lv_obj_set_style_text_opa(f->l_date, LV_OPA_80, 0);
        lv_obj_set_style_pad_top(f->l_date, tf->line_height / 7, 0);
    }
}

static void cal_fill(cs_face_t * f) {
    if (!f->n_cal) return;
    struct tm tm;
    now_tm(&tm);
    char today[12], tomorrow[12];
    strftime(today, sizeof today, "%Y-%m-%d", &tm);
    struct tm tt = tm; tt.tm_mday += 1; tt.tm_hour = 12; mktime(&tt);
    strftime(tomorrow, sizeof tomorrow, "%Y-%m-%d", &tt);
    char hm[8];
    snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    int shown = 0;
    for (int i = 0; i < calendar_state.count && shown < f->n_cal; i++) {
        const calendar_event_t * ev = &calendar_state.ev[i];
        if (strcmp(ev->date, today) < 0) continue;
        if (!strcmp(ev->date, today) && ev->end_time[0] && strcmp(ev->end_time, hm) <= 0) continue;
        char when[24];
        if (!strcmp(ev->date, today))         snprintf(when, sizeof when, "%s", tr("vandaag", "today"));
        else if (!strcmp(ev->date, tomorrow)) snprintf(when, sizeof when, "%s", tr("morgen", "tomorrow"));
        else {
            struct tm et; memset(&et, 0, sizeof et);
            if (sscanf(ev->date, "%d-%d-%d", &et.tm_year, &et.tm_mon, &et.tm_mday) == 3) {
                et.tm_year -= 1900; et.tm_mon -= 1; et.tm_hour = 12; mktime(&et);
                snprintf(when, sizeof when, "%s %d", i18n_day_short(et.tm_wday), et.tm_mday);
            } else snprintf(when, sizeof when, "%s", ev->date);
        }
        char line[140];
        if (ev->time[0]) snprintf(line, sizeof line, "%s %s  %s", when, ev->time, ev->summary);
        else             snprintf(line, sizeof line, "%s  %s", when, ev->summary);
        lv_label_set_text(f->l_cal[shown], line);
        lv_obj_clear_flag(f->l_cal[shown], LV_OBJ_FLAG_HIDDEN);
        shown++;
    }
    for (int i = shown; i < f->n_cal; i++) {
        if (i == 0) {
            lv_label_set_text(f->l_cal[0], tr("Geen afspraken", "No appointments"));
            lv_obj_clear_flag(f->l_cal[0], LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(f->l_cal[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static int cal_signature(void) {
    int s = calendar_state.count * 131;
    if (calendar_state.count > 0) {
        const char * p = calendar_state.ev[0].date;
        while (*p) s = s * 31 + *p++;
        p = calendar_state.ev[0].summary;
        while (*p) s = s * 31 + *p++;
    }
    return s;
}

static void build_calendar(cs_face_t * f, int x, int y, int w, int h, const struct tm * tm) {
    /* top: time (left) + date (right); below: agenda rows */
    int top_h = LV_MAX(h * 34 / 100, 24);
    const lv_font_t * tf = fit_font("88:88", w * 48 / 100, top_h, 1);
    f->l_time = mk_label(f->box, tf, f->c_fg, "--:--");
    lv_obj_set_pos(f->l_time, x, y);
    if (g_cs_cfg[f->which].date) {
        char d[48];
        date_text(f, tm, d, sizeof d, 0);
        const lv_font_t * df = fit_font(d, w * 48 / 100, LV_MAX(top_h / 3, 14), 0);
        f->l_date = mk_label(f->box, df, f->c_acc, d);
        lv_obj_set_pos(f->l_date, x + w - lv_txt_get_width(d, (uint32_t)strlen(d), df, 0, LV_TEXT_FLAG_NONE),
                       y + tf->line_height - df->line_height - tf->line_height / 8);
    }
    int ly = y + tf->line_height + 4;
    /* separator */
    lv_obj_t * sep = mk_plain(f->box);
    lv_obj_set_pos(sep, x, ly);
    lv_obj_set_size(sep, w, LV_MAX(1, h / 120));
    lv_obj_set_style_bg_color(sep, f->c_acc, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_60, 0);
    ly += 6;
    int rest = y + h - ly;
    /* rows sized to the space: ~5 rows, 14..28 px text */
    const lv_font_t * rf = fit_font("Ag", w, rest / 5, 0);
    if (rf->line_height > SF(28)->line_height) rf = SF(28);
    if (rf->line_height < SF(14)->line_height) rf = SF(14);
    int rows = rest / (rf->line_height + 2);
    if (rows > CS_CAL_ROWS) rows = CS_CAL_ROWS;
    if (rows < 1) rows = 1;
    f->n_cal = rows;
    for (int i = 0; i < rows; i++) {
        f->l_cal[i] = mk_label(f->box, rf, f->c_fg, "");
        lv_obj_set_width(f->l_cal[i], w);
        lv_label_set_long_mode(f->l_cal[i], LV_LABEL_LONG_DOT);
        lv_obj_set_pos(f->l_cal[i], x, ly + i * (rf->line_height + 2));
    }
    f->cal_sig = cal_signature();
    cal_fill(f);
}

/* ======================================================================== */
/* Build / update                                                           */
/* ======================================================================== */
static int resolve_face(int face, int w, int h) {
    if (face != CS_FACE_AUTO) return face;
    if (h < SY(70) || w >= 2 * h) return CS_FACE_DIGITAL;
    if (w * 100 >= h * 135) return CS_FACE_LED;
    if (LV_MIN(w, h) >= SX(150)) return CS_FACE_ANALOG;
    return CS_FACE_MINIMAL;
}

static void face_build(cs_face_t * f) {
    const cs_cfg_t * c = &g_cs_cfg[f->which];
    lv_obj_clean(f->box);
    f->draw = NULL;
    f->l_title = f->l_time = f->l_time2 = f->l_sec = f->l_date = NULL;
    for (int i = 0; i < CS_CAL_ROWS; i++) f->l_cal[i] = NULL;
    f->n_cal = 0;
    f->inv_sec_valid = 0;

    f->transparent = (f->ctx == CS_CTX_DIM) && c->col[CS_COL_BG] < 0;
    f->c_bg  = cfg_color(c, CS_COL_BG, f->transparent);
    f->c_fg  = cfg_color(c, CS_COL_FG, f->transparent);
    f->c_acc = cfg_color(c, CS_COL_ACCENT, f->transparent);
    f->c_hh  = cfg_color(c, CS_COL_HOUR, f->transparent);
    f->c_mh  = cfg_color(c, CS_COL_MIN, f->transparent);
    f->c_sh  = cfg_color(c, CS_COL_SEC, f->transparent);

    lv_obj_set_style_bg_color(f->box, f->c_bg, 0);
    lv_obj_set_style_bg_opa(f->box, f->transparent ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_radius(f->box, f->ctx == CS_CTX_FULL ? 0 : SX(c->radius), 0);
    lv_obj_set_style_clip_corner(f->box, false, 0);
    int pad = LV_MAX(4, LV_MIN(f->w, f->h) / 22);
    if (f->ctx == CS_CTX_DIM) pad = 2;
    lv_obj_set_style_pad_all(f->box, pad, 0);

    int x = 0, y = 0;
    int w = f->w - 2 * pad, h = f->h - 2 * pad;

    /* Full screen: the close / settings buttons float in the top corners.
     * The title goes in the band between them; the calendar face (content
     * starts top-left) is moved below the band. */
    int bw = (DISP_VER < 600) ? 60 : 70, bh = (DISP_VER < 600) ? 46 : 50;
    int band = SY(12) + bh + 4 - pad;
    if (f->ctx == CS_CTX_FULL && c->face == CS_FACE_CALENDAR) { y += band; h -= band; }

    if (c->title[0]) {
        const lv_font_t * tf = (f->ctx == CS_CTX_FULL) ? SF(22) : (f->h < SY(140)) ? SF(14) : SF(18);
        f->l_title = mk_label(f->box, tf, f->c_fg, c->title);
        lv_obj_set_style_text_opa(f->l_title, LV_OPA_70, 0);
        lv_label_set_long_mode(f->l_title, LV_LABEL_LONG_DOT);
        if (f->ctx == CS_CTX_FULL) {
            int side = bw + 2 * SX(12);
            lv_obj_set_width(f->l_title, w - 2 * side);
            lv_obj_set_style_text_align(f->l_title, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(f->l_title, side, SY(12) + (bh - tf->line_height) / 2 - pad);
            if (c->face != CS_FACE_CALENDAR) { y += band; h -= band; }
        } else {
            lv_obj_set_width(f->l_title, w);
            lv_obj_set_pos(f->l_title, 0, 0);
            y += tf->line_height + 2;
            h -= tf->line_height + 2;
        }
    }
    /* scale: shrink around the centre of the remaining area */
    if (c->scale < 100) {
        int nw = w * c->scale / 100, nh = h * c->scale / 100;
        x += (w - nw) / 2; y += (h - nh) / 2;
        w = nw; h = nh;
    }
    if (w < 10 || h < 10) return;

    f->face = resolve_face(c->face, w, h);
    struct tm tm;
    now_tm(&tm);
    f->hh = tm.tm_hour; f->mm = tm.tm_min; f->ss = tm.tm_sec; f->mday = tm.tm_mday;
    f->last_t = (long)time(NULL);

    switch (f->face) {
        case CS_FACE_ANALOG:
        case CS_FACE_MINIMAL:
        case CS_FACE_SQUARE:   build_analog(f, x, y, w, h, &tm); break;
        case CS_FACE_LED:      build_led(f, x, y, w, h, &tm); break;
        case CS_FACE_STACKED:  build_stacked(f, x, y, w, h, &tm); break;
        case CS_FACE_CALENDAR: build_calendar(f, x, y, w, h, &tm); break;
        default:               build_digital(f, x, y, w, h, &tm); break;
    }
    set_time_labels(f);
    if (f->face == CS_FACE_ANALOG || f->face == CS_FACE_MINIMAL || f->face == CS_FACE_SQUARE) {
        lv_obj_update_layout(f->box);
    }
}

static int face_on_screen(const cs_face_t * f) {
    lv_obj_t * scr = lv_obj_get_screen(f->box);
    if (scr != lv_scr_act() && scr != lv_layer_top()) return 0;
    return lv_obj_is_visible(f->box);
}

static void face_tick(cs_face_t * f, const struct tm * tm) {
    long now = (long)time(NULL);
    int gap = (now - f->last_t) > 2 || (now < f->last_t);
    f->last_t = now;
    int sec_changed = (tm->tm_sec != f->ss);
    int min_changed = (tm->tm_min != f->mm) || (tm->tm_hour != f->hh);
    int day_changed = (tm->tm_mday != f->mday);
    if (!sec_changed && !min_changed && !day_changed && !gap) return;
    const cs_cfg_t * c = &g_cs_cfg[f->which];
    int analog = (f->face == CS_FACE_ANALOG || f->face == CS_FACE_MINIMAL || f->face == CS_FACE_SQUARE);

    if (analog && f->draw) {
        if (min_changed || gap) {
            f->hh = tm->tm_hour; f->mm = tm->tm_min; f->ss = tm->tm_sec;
            lv_obj_invalidate(f->draw);
            f->inv_sec_valid = 0;
        } else if (sec_changed && c->seconds) {
            lv_area_t old;
            if (f->inv_sec_valid) old = f->inv_sec; else sec_bbox(f, &old);
            f->ss = tm->tm_sec;
            sec_bbox(f, &f->inv_sec);
            f->inv_sec_valid = 1;
            lv_obj_invalidate_area(f->draw, &old);
            lv_obj_invalidate_area(f->draw, &f->inv_sec);
        }
        f->ss = tm->tm_sec;
    } else if (f->face == CS_FACE_LED && f->draw) {
        if (min_changed || gap) {
            f->hh = tm->tm_hour; f->mm = tm->tm_min; f->ss = tm->tm_sec;
            lv_obj_invalidate(f->draw);
        } else if (sec_changed && c->seconds) {
            f->ss = tm->tm_sec;
            lv_area_t ca;
            led_colon_area(f, &ca);
            lv_obj_invalidate_area(f->draw, &ca);
        }
        f->ss = tm->tm_sec;
    } else {
        f->hh = tm->tm_hour; f->mm = tm->tm_min; f->ss = tm->tm_sec;
        if (min_changed || gap || (sec_changed && f->l_sec)) set_time_labels(f);
        if (f->face == CS_FACE_CALENDAR) {
            int sig = cal_signature();
            if (min_changed || gap || sig != f->cal_sig) { f->cal_sig = sig; cal_fill(f); }
        }
    }
    f->hh = tm->tm_hour; f->mm = tm->tm_min;
    if (day_changed || (gap && f->l_date)) {
        f->mday = tm->tm_mday;
        if (f->l_date) {
            char d[48];
            int longform = (f->face == CS_FACE_DIGITAL) ||
                           (f->face == CS_FACE_LED && f->w > SX(300));
            date_text(f, tm, d, sizeof d, longform);
            lv_label_set_text(f->l_date, d);
        }
    }
}

static void tick_cb(lv_timer_t * t) {
    (void)t;
    struct tm tm;
    int have = 0;
    for (int i = 0; i < CS_MAX_LIVE; i++) {
        cs_face_t * f = g_live[i];
        if (!f || !face_on_screen(f)) continue;
        if (!have) { now_tm(&tm); have = 1; }
        face_tick(f, &tm);
    }
}

static void on_box_delete(lv_event_t * e) {
    cs_face_t * f = (cs_face_t *)lv_event_get_user_data(e);
    for (int i = 0; i < CS_MAX_LIVE; i++) if (g_live[i] == f) g_live[i] = NULL;
    lv_mem_free(f);
}

lv_obj_t * cs_create(lv_obj_t * parent, int which, int ctx, int x, int y, int w, int h) {
    ensure_loaded();
    if (which < 0 || which >= CS_NCFG) which = CS_HOME;
    lv_obj_t * box = mk_plain(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    cs_face_t * f = (cs_face_t *)lv_mem_alloc(sizeof *f);
    if (!f) return box;
    memset(f, 0, sizeof *f);
    f->box = box; f->which = which; f->ctx = ctx; f->w = w; f->h = h;
    int slot = -1;
    for (int i = 0; i < CS_MAX_LIVE; i++) if (!g_live[i]) { slot = i; break; }
    if (slot >= 0) g_live[slot] = f;
    lv_obj_add_event_cb(box, on_box_delete, LV_EVENT_DELETE, f);
    face_build(f);
    if (!g_tick) g_tick = lv_timer_create(tick_cb, 250, NULL);
    return box;
}

void cs_apply(int which) {
    for (int i = 0; i < CS_MAX_LIVE; i++)
        if (g_live[i] && g_live[i]->which == which) face_build(g_live[i]);
}

/* ======================================================================== */
/* Full-screen view                                                         */
/* ======================================================================== */
static void on_full_close(lv_event_t * e) { (void)e; ui_pop(); }
static void on_full_gear(lv_event_t * e) { (void)e; cs_open_settings(CS_HOME); }

static lv_obj_t * corner_btn(lv_obj_t * parent, const char * sym, lv_align_t al,
                             lv_event_cb_t cb) {
    int bw = (DISP_VER < 600) ? 60 : 70, bh = (DISP_VER < 600) ? 46 : 50;
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, bw, bh);
    lv_obj_align(b, al, al == LV_ALIGN_TOP_LEFT ? SX(12) : -SX(12), SY(12));
    lv_obj_set_style_bg_color(b, lv_color_hex(0x223344), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_ext_click_area(b, 25);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, SF(22), 0);
    lv_obj_center(l);
    return b;
}

void cs_open_fullscreen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /* the face fills the screen; the two buttons float in the corners */
    cs_create(scr, CS_HOME, CS_CTX_FULL, 0, 0, LV_HOR_RES, LV_VER_RES);
    corner_btn(scr, LV_SYMBOL_CLOSE, LV_ALIGN_TOP_LEFT, on_full_close);
    corner_btn(scr, LV_SYMBOL_SETTINGS, LV_ALIGN_TOP_RIGHT, on_full_gear);
    ui_push_owned(scr);
}

/* ======================================================================== */
/* Settings modal                                                           */
/* ======================================================================== */
static lv_obj_t * m_overlay = NULL;
static lv_obj_t * m_list = NULL;
static lv_obj_t * m_prev_wrap = NULL;
static lv_obj_t * m_seg[CS_NCFG];
static lv_obj_t * m_kb = NULL;
static int        m_which = CS_HOME;
static int        m_dirty = 0;

static const uint32_t SWATCHES[] = {
    0xffffff, 0x000000, 0x0f2238, 0x4aa3ff, 0xa77bff, 0xff4d4d,
    0xffc94d, 0x3ccf7a, 0xff7ad9, 0x8899aa,
};
#define N_SWATCH ((int)(sizeof SWATCHES / sizeof SWATCHES[0]))

static void m_build_list(void);
static void m_build_preview(void);

/* Rebuilding the list deletes the button whose CLICKED event is running —
 * do it after the event has finished. */
static void m_build_list_cb(void * p) { (void)p; m_build_list(); }
static void m_build_list_async(void) { lv_async_call(m_build_list_cb, NULL); }

static void m_changed(void) {
    m_dirty = 1;
    cs_apply(m_which);
}

static void m_close(void) {
    if (m_kb) { lv_obj_del(m_kb); m_kb = NULL; }
    if (m_overlay) { lv_obj_del(m_overlay); m_overlay = NULL; }
    m_list = NULL; m_prev_wrap = NULL;
    if (m_dirty) { cs_save(); m_dirty = 0; }
}
static void on_m_done(lv_event_t * e) { (void)e; m_close(); }
void cs_close_settings(void) { if (m_overlay) m_close(); }

static void on_m_target(lv_event_t * e) {
    int w = (int)(intptr_t)lv_event_get_user_data(e);
    if (w == m_which) return;
    m_which = w;
    if (m_kb) { lv_obj_del(m_kb); m_kb = NULL; }
    for (int i = 0; i < CS_NCFG; i++)
        lv_obj_set_style_bg_color(m_seg[i], lv_color_hex(i == m_which ? 0x2f6fd6 : 0x223344), 0);
    m_build_preview();
    m_build_list();
}

static lv_obj_t * m_row(const char * title) {
    lv_obj_t * r = mk_plain(m_list);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(r, SY(6), 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, SX(6), 0);
    lv_obj_set_style_pad_row(r, SY(4), 0);
    lv_obj_t * l = mk_label(r, SF(18), lv_color_hex(0xe6eef7), title);
    lv_obj_set_width(l, LV_PCT(100));
    return r;
}

static lv_obj_t * m_btn(lv_obj_t * parent, const char * txt, int w, lv_event_cb_t cb, void * ud) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, w, SY(44));
    lv_obj_set_style_bg_color(b, lv_color_hex(0x223344), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t * l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, SF(18), 0);
    lv_obj_center(l);
    return b;
}

/* face cycler */
static lv_obj_t * m_face_lbl = NULL;
static void on_m_face(lv_event_t * e) {
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    cs_cfg_t * c = &g_cs_cfg[m_which];
    c->face = (c->face + d + CS_FACE_COUNT) % CS_FACE_COUNT;
    if (m_face_lbl) lv_label_set_text(m_face_lbl, cs_face_name(c->face));
    m_changed();
}

static void on_m_theme(lv_event_t * e) {
    g_cs_cfg[m_which].theme = (int)(intptr_t)lv_event_get_user_data(e);
    m_changed();
    m_build_list_async();
}

static void on_m_switch(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    int * p = (int *)lv_event_get_user_data(e);
    *p = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    m_changed();
}

static void on_m_slider(lv_event_t * e) {
    lv_obj_t * s = lv_event_get_target(e);
    int * p = (int *)lv_event_get_user_data(e);
    *p = lv_slider_get_value(s);
    lv_obj_t * vl = (lv_obj_t *)lv_obj_get_user_data(s);
    if (vl) lv_label_set_text_fmt(vl, "%d", *p);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) m_changed();
}

static void on_m_color(lv_event_t * e) {
    intptr_t ud = (intptr_t)lv_event_get_user_data(e);
    int slot = (int)(ud >> 8), idx = (int)(ud & 0xff);
    g_cs_cfg[m_which].col[slot] = (idx == 0xff) ? -1 : (int)SWATCHES[idx];
    m_changed();
    m_build_list_async();
}

static void on_m_kb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        if (m_kb) { lv_obj_del_async(m_kb); m_kb = NULL; }
    }
}

static void on_m_title(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        if (!m_kb) {
            m_kb = lv_keyboard_create(lv_layer_top());
            lv_obj_set_size(m_kb, LV_HOR_RES, LV_VER_RES * 45 / 100);
            lv_obj_align(m_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_style_text_font(m_kb, SF(20), 0);
            lv_obj_add_event_cb(m_kb, on_m_kb, LV_EVENT_ALL, NULL);
        }
        lv_keyboard_set_textarea(m_kb, ta);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        cs_cfg_t * c = &g_cs_cfg[m_which];
        const char * src = lv_textarea_get_text(ta);
        size_t o = 0;
        for (const char * p = src; *p && o + 1 < sizeof c->title; p++)
            if ((unsigned char)*p >= 0x20) c->title[o++] = *p;   /* no newlines: one cfg line */
        c->title[o] = 0;
        m_changed();
    }
}

static void m_switch_row(const char * title, int * val) {
    lv_obj_t * r = mk_plain(m_list);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, SY(52));
    lv_obj_t * l = mk_label(r, SF(18), lv_color_hex(0xe6eef7), title);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t * sw = lv_switch_create(r);
    lv_obj_set_size(sw, SX(70), SY(36));
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -SX(4), 0);
    if (*val) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_m_switch, LV_EVENT_VALUE_CHANGED, val);
}

static void m_slider_row(const char * title, int * val, int lo, int hi) {
    lv_obj_t * r = mk_plain(m_list);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, SY(70));
    lv_obj_t * l = mk_label(r, SF(18), lv_color_hex(0xe6eef7), title);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t * vl = mk_label(r, SF(18), lv_color_hex(0x88aabb), "");
    lv_label_set_text_fmt(vl, "%d", *val);
    lv_obj_align(vl, LV_ALIGN_TOP_RIGHT, -SX(4), 0);
    lv_obj_t * s = lv_slider_create(r);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, *val, LV_ANIM_OFF);
    lv_obj_set_width(s, LV_PCT(92));
    lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, -SY(10));
    lv_obj_set_ext_click_area(s, SY(14));
    lv_obj_set_user_data(s, vl);
    lv_obj_add_event_cb(s, on_m_slider, LV_EVENT_VALUE_CHANGED, val);
    lv_obj_add_event_cb(s, on_m_slider, LV_EVENT_RELEASED, val);
}

static void m_color_row(const char * title, int slot) {
    cs_cfg_t * c = &g_cs_cfg[m_which];
    lv_obj_t * r = m_row(title);
    int sz = SX(40);
    lv_obj_t * th = lv_btn_create(r);
    lv_obj_set_size(th, SX(76), sz);
    lv_obj_set_style_radius(th, 8, 0);
    lv_obj_set_style_bg_color(th, lv_color_hex(0x223344), 0);
    lv_obj_set_style_border_color(th, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(th, c->col[slot] < 0 ? 3 : 0, 0);
    lv_obj_add_event_cb(th, on_m_color, LV_EVENT_CLICKED, (void *)(intptr_t)((slot << 8) | 0xff));
    lv_obj_t * tl = lv_label_create(th);
    lv_label_set_text(tl, tr("Thema", "Theme"));
    lv_obj_set_style_text_font(tl, SF(14), 0);
    lv_obj_center(tl);
    for (int i = 0; i < N_SWATCH; i++) {
        lv_obj_t * b = lv_btn_create(r);
        lv_obj_set_size(b, sz, sz);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(SWATCHES[i]), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        int sel = (c->col[slot] == (int)SWATCHES[i]);
        lv_obj_set_style_border_color(b, lv_color_hex(SWATCHES[i] == 0xffffff ? 0x4aa3ff : 0xffffff), 0);
        lv_obj_set_style_border_width(b, sel ? 3 : 1, 0);
        lv_obj_set_style_border_opa(b, sel ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_add_event_cb(b, on_m_color, LV_EVENT_CLICKED, (void *)(intptr_t)((slot << 8) | i));
    }
}

static void m_build_list(void) {
    if (!m_list) return;
    lv_coord_t sy = lv_obj_get_scroll_y(m_list);
    lv_obj_clean(m_list);
    cs_cfg_t * c = &g_cs_cfg[m_which];

    /* face */
    lv_obj_t * r = m_row(tr("Wijzerplaat", "Clock face"));
    m_btn(r, LV_SYMBOL_LEFT, SX(56), on_m_face, (void *)(intptr_t)-1);
    m_face_lbl = mk_label(r, SF(18), lv_color_hex(0xffffff), cs_face_name(c->face));
    lv_obj_set_width(m_face_lbl, SX(200));
    lv_obj_set_style_text_align(m_face_lbl, LV_TEXT_ALIGN_CENTER, 0);
    m_btn(r, LV_SYMBOL_RIGHT, SX(56), on_m_face, (void *)(intptr_t)1);

    /* theme */
    r = m_row(tr("Thema", "Theme"));
    for (int t = 0; t < CS_THEME_COUNT; t++) {
        lv_obj_t * b = lv_btn_create(r);
        lv_obj_set_size(b, SX(78), SY(40));
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(THEMES[t][CS_COL_BG]), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(THEMES[t][CS_COL_ACCENT]), 0);
        lv_obj_set_style_border_width(b, c->theme == t ? 4 : 1, 0);
        lv_obj_add_event_cb(b, on_m_theme, LV_EVENT_CLICKED, (void *)(intptr_t)t);
        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text(l, theme_name(t));
        lv_obj_set_style_text_font(l, SF(14), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(THEMES[t][CS_COL_FG]), 0);
        lv_obj_center(l);
    }

    m_switch_row(tr("Seconden (wijzer / knipperende dubbele punt)",
                    "Seconds (hand / blinking colon)"), &c->seconds);
    m_switch_row(tr("Datum tonen", "Show date"), &c->date);
    m_switch_row(tr("12-uursklok", "12-hour clock"), &c->h12);
    m_slider_row(tr("Schaal (%)", "Scale (%)"), &c->scale, 65, 100);
    m_slider_row(tr("Hoekafronding", "Corner radius"), &c->radius, 0, 40);

    /* title */
    r = m_row(tr("Titel (leeg = geen)", "Title (empty = none)"));
    lv_obj_t * ta = lv_textarea_create(r);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, sizeof c->title - 1);
    lv_textarea_set_text(ta, c->title);
    lv_obj_set_width(ta, LV_PCT(96));
    lv_obj_add_event_cb(ta, on_m_title, LV_EVENT_ALL, NULL);

    m_color_row(tr("Achtergrond", "Background"), CS_COL_BG);
    m_color_row(tr("Voorgrond (cijfers)", "Foreground (digits)"), CS_COL_FG);
    m_color_row(tr("Accent", "Accent"), CS_COL_ACCENT);
    m_color_row(tr("Uurwijzer", "Hour hand"), CS_COL_HOUR);
    m_color_row(tr("Minutenwijzer", "Minute hand"), CS_COL_MIN);
    m_color_row(tr("Secondewijzer", "Second hand"), CS_COL_SEC);

    lv_obj_update_layout(m_list);
    lv_obj_scroll_to_y(m_list, sy, LV_ANIM_OFF);
}

/* Preview at the real size of the tile / dim block where that fits,
 * otherwise scaled down to the preview area with the same aspect. */
static void m_build_preview(void) {
    if (!m_prev_wrap) return;
    lv_obj_clean(m_prev_wrap);
    lv_obj_update_layout(m_prev_wrap);
    int aw = lv_obj_get_content_width(m_prev_wrap);
    int ah = lv_obj_get_content_height(m_prev_wrap);
    int w = 0, h = 0, x, y;
    if (m_which == CS_HOME) {
        const layout_tile_t * L = layout_find(LT_CLOCKSTUDIO);
        if (L) { layout_cell_px(L->col, L->row, L->w, L->h, &x, &y, &w, &h); w -= 8; h -= 8; }
        else { layout_cell_px(0, 0, 3, 3, &x, &y, &w, &h); w -= 8; h -= 8; }
    } else {
        const dim_block_t * b = &g_dim_blocks[DB_CLOCKSTUDIO];
        layout_cell_px(b->col, b->row, b->w, b->h, &x, &y, &w, &h);
    }
    if (w > aw) { h = h * aw / w; w = aw; }
    if (h > ah) { w = w * ah / h; h = ah; }
    lv_obj_set_style_bg_color(m_prev_wrap, lv_color_hex(m_which == CS_DIM ? 0x000000 : 0x0b1626), 0);
    lv_obj_t * lab = mk_label(m_prev_wrap, SF(14), lv_color_hex(0x88aabb),
                              m_which == CS_DIM ? tr("Voorbeeld dimscherm", "Dim screen preview")
                                                : tr("Voorbeeld tegel", "Tile preview"));
    lv_obj_align(lab, LV_ALIGN_TOP_MID, 0, 0);
    int top = lab ? SF(14)->line_height + 4 : 0;
    if (h > ah - top) { w = w * (ah - top) / h; h = ah - top; }
    cs_create(m_prev_wrap, m_which, m_which == CS_DIM ? CS_CTX_DIM : CS_CTX_TILE,
              (aw - w) / 2, top + (ah - top - h) / 2, w, h);
}

void cs_open_settings(int which) {
    ensure_loaded();
    if (m_overlay) m_close();
    m_which = (which == CS_DIM) ? CS_DIM : CS_HOME;
    m_dirty = 0;

    m_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(m_overlay);
    lv_obj_set_size(m_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(m_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(m_overlay, LV_OPA_70, 0);
    lv_obj_add_flag(m_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(m_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * card = lv_obj_create(m_overlay);
    lv_obj_set_size(card, LV_HOR_RES - 16, LV_VER_RES - 16);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x16243a), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    int cw = LV_HOR_RES - 16 - 20, ch = LV_VER_RES - 16 - 20;
    int head_h = SY(50);

    lv_obj_t * t = mk_label(card, SF(22), lv_color_hex(0xffffff), "Klok Studio");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, SY(8));

    for (int i = 0; i < CS_NCFG; i++) {
        m_seg[i] = lv_btn_create(card);
        lv_obj_set_size(m_seg[i], SX(150), SY(42));
        lv_obj_align(m_seg[i], LV_ALIGN_TOP_LEFT, SX(190) + i * SX(160), 0);
        lv_obj_set_style_radius(m_seg[i], 10, 0);
        lv_obj_set_style_bg_color(m_seg[i], lv_color_hex(i == m_which ? 0x2f6fd6 : 0x223344), 0);
        lv_obj_add_event_cb(m_seg[i], on_m_target, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t * l = lv_label_create(m_seg[i]);
        lv_label_set_text(l, i == CS_HOME ? tr("Thuistegel", "Home tile") : tr("Dimscherm", "Dim screen"));
        lv_obj_set_style_text_font(l, SF(18), 0);
        lv_obj_center(l);
    }
    lv_obj_t * done = lv_btn_create(card);
    lv_obj_set_size(done, SX(130), SY(42));
    lv_obj_align(done, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(done, lv_color_hex(0x1f9d55), 0);
    lv_obj_set_style_radius(done, 10, 0);
    lv_obj_add_event_cb(done, on_m_done, LV_EVENT_CLICKED, NULL);
    lv_obj_t * dl = lv_label_create(done);
    lv_label_set_text(dl, tr("Klaar", "Done"));
    lv_obj_set_style_text_font(dl, SF(18), 0);
    lv_obj_center(dl);

    int pw = cw * 38 / 100;
    m_prev_wrap = mk_plain(card);
    lv_obj_set_pos(m_prev_wrap, 0, head_h);
    lv_obj_set_size(m_prev_wrap, pw, ch - head_h);
    lv_obj_set_style_bg_opa(m_prev_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(m_prev_wrap, 12, 0);
    lv_obj_set_style_pad_all(m_prev_wrap, 6, 0);

    m_list = lv_obj_create(card);
    lv_obj_remove_style_all(m_list);
    lv_obj_set_pos(m_list, pw + SX(12), head_h);
    lv_obj_set_size(m_list, cw - pw - SX(12), ch - head_h);
    lv_obj_set_flex_flow(m_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_right(m_list, SX(8), 0);
    lv_obj_add_flag(m_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(m_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(m_list, LV_SCROLLBAR_MODE_AUTO);

    m_build_preview();
    m_build_list();
}
