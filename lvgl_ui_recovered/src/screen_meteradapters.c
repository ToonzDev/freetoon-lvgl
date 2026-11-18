/*
 * Meetadapters — add, configure and remove meter adapters (Settings ->
 * Adapters -> Beheren). Port of the stock qt-gui eMetersSettings flows; the
 * protocol lives in ma_admin.c.
 *
 *   overview  : one card per adapter from hdrv_p1 usageDevicesInfo (name,
 *               driver status, measured meters) + "Meetadapter toevoegen".
 *   add       : Z-Wave inclusion (120 s window, user presses the adapter
 *               button), result checked against the stock meter-type list,
 *               then straight on to "meters".
 *   meters    : pick per resource which input the adapter reads (P1 /
 *               analog / laser), limited to what GetMeasureCapabilities
 *               offers; saved with SetSensorConfiguration for all adapters.
 *   pulses    : for analog elec / gas / solar, the pulse rate (imp/kWh,
 *               Wh/imp, rev/kWh, rev/m3, L/pulse — stock units) with
 *               SetMeterConfiguration; the overview shows what the driver has.
 *   remove    : Z-Wave exclusion (user presses the adapter button); on
 *               failure "Geforceerd verwijderen" (RemoveDevice) after a
 *               confirm.
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "ma_admin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COL_GREEN  THEME_SUCCESS
#define COL_RED    THEME_DANGER
#define COL_AMBER  THEME_WARNING
#define COL_OKBTN  THEME_SUCCESS_BG
#define COL_WARNBT THEME_DANGER_BG

static lv_obj_t * scr_root, * lst, * lbl_empty;
static lv_timer_t * tmr;
static int  g_info_seq = -1;

/* ------------------------------------------------------------ overlay */
enum {
    F_NONE, F_ADD_INTRO, F_ADD_BUSY, F_ADD_RESULT,
    F_REM_INTRO, F_REM_BUSY, F_REM_RESULT, F_FORCE_CONFIRM, F_REM_VERIFY,
    F_CFG_LOADING, F_CFG_EDIT, F_CFG_SAVING, F_CFG_RESULT, F_CFG_PULSE, F_CFG_PSAVING
};
static int  g_flow = F_NONE;
static int  g_admin_seq;
static int  g_verify_force;      /* F_REM_VERIFY after a force remove (vs exclusion) */
static time_t g_busy_since;
static char g_uuid[48];          /* adapter being removed / configured */
static char g_name[48];
static lv_obj_t * ov, * ov_title, * ov_body, * ov_status, * ov_btn[3], * ov_btn_lbl[3], * ov_rows;

static void flow_set(int f);

/* ------------------------------------------------- meter (sensor) model */
/* Resources we let the user choose; anything else already configured
 * (e.g. water) is preserved untouched on save. */
typedef struct { const char * nl, * en; const char * sensor[4]; const char * opt_nl[4], * opt_en[4]; int n; } res_t;
static const res_t RES[] = {
    { "Elektriciteit", "Electricity", { NULL, "p1Elec", "analogElec", "laserElec" },
      { "Geen", "Slimme meter", "Analoog", "Laser" }, { "None", "Smart meter", "Analog", "Laser" }, 4 },
    { "Gas", "Gas", { NULL, "p1Gas", "analogGas" },
      { "Geen", "Slimme meter", "Analoog" }, { "None", "Smart meter", "Analog" }, 3 },
    { "Warmte", "Heat", { NULL, "analogHeat" },
      { "Geen", "Analoog" }, { "None", "Analog" }, 2 },
    { "Zon", "Solar", { NULL, "analogSolar" },
      { "Geen", "Analoog" }, { "None", "Analog" }, 2 },
};
#define NRES (int)(sizeof RES / sizeof RES[0])

static ma_sensconf_t g_conf;      /* full config of all adapters (edited copy) */
static ma_caps_t     g_caps;
static int  g_conf_seq0, g_caps_seq0, g_set_seq0;
static int  g_sel[NRES];          /* chosen option index per resource */
static int  g_optmap[NRES][4];    /* button index -> option index */
static const char * g_btnmap[NRES][6];
static lv_obj_t * g_bm[NRES];

static int is_known_sensor(const char * s) {
    for (int r = 0; r < NRES; r++)
        for (int o = 1; o < RES[r].n; o++)
            if (!strcmp(s, RES[r].sensor[o])) return 1;
    return 0;
}

static int cap_ok(const char * s) {
    if (g_caps.n == 0) return 1;      /* no capability list: offer everything */
    for (int i = 0; i < g_caps.n; i++) if (!strcmp(g_caps.cap[i], s)) return 1;
    return 0;
}

static ma_sens_t * conf_dev(const char * uuid) {
    for (int i = 0; i < g_conf.n; i++) if (!strcmp(g_conf.dev[i].uuid, uuid)) return &g_conf.dev[i];
    if (g_conf.n >= MA_MAX_DEV) return NULL;
    ma_sens_t * d = &g_conf.dev[g_conf.n++];
    memset(d, 0, sizeof *d);
    snprintf(d->uuid, sizeof d->uuid, "%s", uuid);
    return d;
}

static void on_bm(lv_event_t * e) {
    int r = (int)(intptr_t)lv_event_get_user_data(e);
    uint16_t b = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (b != LV_BTNMATRIX_BTN_NONE) g_sel[r] = g_optmap[r][b];
}

static void build_editor(void) {
    lv_obj_clean(ov_rows);
    ma_sens_t * d = conf_dev(g_uuid);
    int y = 0;
    for (int r = 0; r < NRES; r++) {
        g_bm[r] = NULL;
        g_sel[r] = 0;
        if (d) for (int s = 0; s < d->n; s++)
            for (int o = 1; o < RES[r].n; o++)
                if (!strcmp(d->sensor[s], RES[r].sensor[o])) g_sel[r] = o;
        int nb = 0, checked = 0;
        for (int o = 0; o < RES[r].n; o++) {
            if (o > 0 && !cap_ok(RES[r].sensor[o]) && g_sel[r] != o) continue;
            if (o == g_sel[r]) checked = nb;
            g_optmap[r][nb] = o;
            g_btnmap[r][nb++] = tr(RES[r].opt_nl[o], RES[r].opt_en[o]);
        }
        if (nb < 2) continue;            /* nothing to choose for this meter */
        g_btnmap[r][nb] = "";

        lv_obj_t * l = lv_label_create(ov_rows);
        lv_obj_set_style_text_font(l, SF(18), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
        lv_label_set_text(l, tr(RES[r].nl, RES[r].en));
        lv_obj_set_pos(l, 0, y + SY(12));

        lv_obj_t * bm = lv_btnmatrix_create(ov_rows);
        lv_btnmatrix_set_map(bm, g_btnmap[r]);
        lv_btnmatrix_set_btn_ctrl_all(bm, LV_BTNMATRIX_CTRL_CHECKABLE);
        lv_btnmatrix_set_one_checked(bm, true);
        lv_btnmatrix_set_btn_ctrl(bm, checked, LV_BTNMATRIX_CTRL_CHECKED);
        lv_obj_set_size(bm, SX(165) * nb, SY(48));
        lv_obj_set_pos(bm, SX(170), y);
        lv_obj_set_style_text_font(bm, SF(16), 0);
        lv_obj_set_style_pad_all(bm, 4, 0);
        lv_obj_set_style_pad_column(bm, 6, 0);
        lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(bm, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bm, lv_color_hex(THEME_BTN_BG), LV_PART_ITEMS);
        lv_obj_set_style_text_color(bm, lv_color_hex(THEME_TEXT_HI), LV_PART_ITEMS);
        lv_obj_set_style_bg_color(bm, lv_color_hex(THEME_ACCENT_FILL), LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(bm, lv_color_hex(THEME_ON_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_add_event_cb(bm, on_bm, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)r);
        g_bm[r] = bm;
        y += SY(58);
    }
}

/* Returns NULL when valid, else a message. */
static const char * editor_apply(void) {
    const char * s[NRES];
    for (int r = 0; r < NRES; r++) s[r] = RES[r].sensor[g_sel[r]];
    /* analog input 1 = elec or solar, analog input 2 = gas or heat */
    if (s[0] && s[3] && !strcmp(s[0], "analogElec"))
        return tr("Analoog elektriciteit en zon gebruiken dezelfde ingang - kies er een.",
                  "Analog electricity and solar share one input - pick one.");
    if (s[1] && s[2] && !strcmp(s[1], "analogGas"))
        return tr("Analoog gas en warmte gebruiken dezelfde ingang - kies er een.",
                  "Analog gas and heat share one input - pick one.");
    int any = 0;
    for (int r = 0; r < NRES; r++) if (s[r]) any = 1;
    if (!any)
        return tr("Kies minstens een meter.", "Pick at least one meter.");
    ma_sens_t * d = conf_dev(g_uuid);
    if (!d) return tr("Te veel meetadapters.", "Too many meter adapters.");
    ma_sens_t keep = *d;                 /* preserve sensors we don't manage */
    d->n = 0;
    for (int i = 0; i < keep.n && d->n < MA_MAX_SENS; i++)
        if (!is_known_sensor(keep.sensor[i]))
            snprintf(d->sensor[d->n++], sizeof d->sensor[0], "%s", keep.sensor[i]);
    for (int r = 0; r < NRES && d->n < MA_MAX_SENS; r++)
        if (s[r]) snprintf(d->sensor[d->n++], sizeof d->sensor[0], "%s", s[r]);
    return NULL;
}

/* --------------------------------------------- pulses (analog meters) */
/* Stock eMetersSettings Constants.js cValueUnits. The driver stores one
 * number ("divider", pulses per kWh / m3); a unit converts what the user
 * types: divisor ? divisor/value : multi ? value*multi : value. meter_type
 * goes along for elec (0 pulse, 1 disk) and gas; solar sends none, like the
 * stock solar wizard. Driver range: 30 .. max. */
typedef struct { int id, meter_type; const char * name; double divisor, multi, def; int max; } punit_t;
static const punit_t U_ELEC[]  = { { 0, 0, "imp/kWh", 0, 0, 1000, 10000 }, { 4, 0, "Wh/imp", 1000, 0, 1000, 10000 },
                                   { 5, 1, "omw/kWh", 0, 0, 300, 1000 } };
static const punit_t U_GAS[]   = { { 10, 0, "omw/m3", 0, 0, 100, 10000 }, { 11, 0, "L/puls", 0, 10, 100, 10000 } };
static const punit_t U_SOLAR[] = { { 0, -1, "imp/kWh", 0, 0, 10000, 10000 }, { 4, -1, "Wh/imp", 1000, 0, 10000, 10000 } };
typedef struct { const char * res, * sensor, * nl, * en; const punit_t * u; int nu; } pres_t;
static const pres_t PRES[] = {
    { "elec",  "analogElec",  "Elektriciteit", "Electricity", U_ELEC,  3 },
    { "gas",   "analogGas",   "Gas",           "Gas",         U_GAS,   2 },
    { "solar", "analogSolar", "Zon",           "Solar",       U_SOLAR, 2 },
};
#define NPRES (int)(sizeof PRES / sizeof PRES[0])

static double punit_to_divider(const punit_t * u, double v) {
    return u->divisor ? u->divisor / v : u->multi ? v * u->multi : v;
}
static double punit_from_divider(const punit_t * u, double d) {
    return u->divisor ? u->divisor / d : u->multi ? d / u->multi : d;
}
/* Unit of a resource by the driver's dividerType; NULL if unknown. */
static const punit_t * punit_by_id(const pres_t * r, int id) {
    for (int i = 0; i < r->nu; i++) if (r->u[i].id == id) return &r->u[i];
    return NULL;
}
static const pres_t * pres_by_type(const char * type) {
    for (int i = 0; i < NPRES; i++) if (!strcmp(PRES[i].res, type)) return &PRES[i];
    return NULL;
}
/* "1000 imp/kWh" for the overview; "" when the driver gave nothing usable. */
static void pulse_text(const ma_usage_t * us, char * out, size_t sz) {
    out[0] = 0;
    const pres_t * r = pres_by_type(us->type);
    if (!r || us->measure != 0 || !(us->divider > 0)) return;
    const punit_t * u = punit_by_id(r, us->divider_type);
    if (!u) u = &r->u[0];
    snprintf(out, sz, "%.6g %s", punit_from_divider(u, us->divider), u->name);
}

/* Pulse step: one row per analog meter the adapter now reads. */
static int  g_prow_n;
static int  g_prow_res[NPRES];             /* index into PRES */
static int  g_prow_unit[NPRES];            /* selected unit index */
static lv_obj_t * g_prow_ta[NPRES];
static const char * g_prow_map[NPRES][5];
static lv_obj_t * g_kb;
static int  g_mc_seq0, g_mc_sent;

static int conf_has_sensor(const char * uuid, const char * sensor) {
    for (int i = 0; i < g_conf.n; i++) {
        if (strcmp(g_conf.dev[i].uuid, uuid)) continue;
        for (int s = 0; s < g_conf.dev[i].n; s++) if (!strcmp(g_conf.dev[i].sensor[s], sensor)) return 1;
    }
    return 0;
}
static int pulse_rows_wanted(void) {
    int n = 0;
    for (int r = 0; r < NPRES; r++) if (conf_has_sensor(g_uuid, PRES[r].sensor)) n++;
    return n;
}

static void on_ta(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    if (c == LV_EVENT_FOCUSED || c == LV_EVENT_CLICKED) {
        lv_keyboard_set_textarea(g_kb, ta);
        lv_obj_clear_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    }
}
static void on_kb(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL) {
        lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(g_kb, NULL);
    }
}
static void on_punit(lv_event_t * e) {
    int r = (int)(intptr_t)lv_event_get_user_data(e);
    uint16_t b = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (b == LV_BTNMATRIX_BTN_NONE || b == g_prow_unit[r]) return;
    /* convert what is typed to the new unit, so the meter value stays the
     * same (1000 imp/kWh <-> 1 Wh/imp) */
    const pres_t * p = &PRES[g_prow_res[r]];
    char t[16]; snprintf(t, sizeof t, "%s", lv_textarea_get_text(g_prow_ta[r]));
    for (char * c = t; *c; c++) if (*c == ',') *c = '.';
    double v = atof(t);
    const punit_t * from = &p->u[g_prow_unit[r]], * to = &p->u[b];
    g_prow_unit[r] = b;
    if (v > 0 && from->meter_type == to->meter_type) {
        char vb[24];
        snprintf(vb, sizeof vb, "%.6g", punit_from_divider(to, punit_to_divider(from, v)));
        lv_textarea_set_text(g_prow_ta[r], vb);
    }
}

static void build_pulse_editor(void) {
    lv_obj_clean(ov_rows);
    g_prow_n = 0;
    ma_info_t inf; ma_get_info(&inf);
    const ma_dev_t * dev = NULL;
    for (int i = 0; i < inf.n; i++) if (!strcmp(inf.dev[i].uuid, g_uuid)) dev = &inf.dev[i];
    int y = 0;
    for (int r = 0; r < NPRES; r++) {
        if (!conf_has_sensor(g_uuid, PRES[r].sensor)) continue;
        const pres_t * p = &PRES[r];
        int k = g_prow_n++;
        g_prow_res[k] = r;
        /* current value from the driver, else the stock default of unit 0 */
        int ui = 0;
        double val = p->u[0].def;
        if (dev) for (int u = 0; u < dev->n_usage; u++) {
            const ma_usage_t * us = &dev->usage[u];
            if (strcmp(us->type, p->res) || !(us->divider > 0)) continue;
            for (int j = 0; j < p->nu; j++)
                if (p->u[j].id == us->divider_type &&
                    (p->u[j].meter_type < 0 || us->meter_type < 0 || p->u[j].meter_type == us->meter_type)) ui = j;
            val = punit_from_divider(&p->u[ui], us->divider);
        }
        g_prow_unit[k] = ui;

        lv_obj_t * l = lv_label_create(ov_rows);
        lv_obj_set_style_text_font(l, SF(18), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
        lv_label_set_text(l, tr(p->nl, p->en));
        lv_obj_set_pos(l, 0, y + SY(12));

        lv_obj_t * ta = lv_textarea_create(ov_rows);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_accepted_chars(ta, "0123456789.,");
        lv_textarea_set_max_length(ta, 8);
        char vb[24]; snprintf(vb, sizeof vb, "%.6g", val);
        lv_textarea_set_text(ta, vb);
        lv_obj_set_size(ta, SX(150), SY(48));
        lv_obj_set_pos(ta, SX(170), y);
        lv_obj_set_style_text_font(ta, SF(18), 0);
        lv_obj_add_event_cb(ta, on_ta, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(ta, on_ta, LV_EVENT_CLICKED, NULL);
        g_prow_ta[k] = ta;

        for (int j = 0; j < p->nu; j++) g_prow_map[k][j] = p->u[j].name;
        g_prow_map[k][p->nu] = "";
        lv_obj_t * bm = lv_btnmatrix_create(ov_rows);
        lv_btnmatrix_set_map(bm, g_prow_map[k]);
        lv_btnmatrix_set_btn_ctrl_all(bm, LV_BTNMATRIX_CTRL_CHECKABLE);
        lv_btnmatrix_set_one_checked(bm, true);
        lv_btnmatrix_set_btn_ctrl(bm, ui, LV_BTNMATRIX_CTRL_CHECKED);
        lv_obj_set_size(bm, SX(140) * p->nu, SY(48));
        lv_obj_set_pos(bm, SX(335), y);
        lv_obj_set_style_text_font(bm, SF(16), 0);
        lv_obj_set_style_pad_all(bm, 4, 0);
        lv_obj_set_style_pad_column(bm, 6, 0);
        lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(bm, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bm, lv_color_hex(THEME_BTN_BG), LV_PART_ITEMS);
        lv_obj_set_style_text_color(bm, lv_color_hex(THEME_TEXT_HI), LV_PART_ITEMS);
        lv_obj_set_style_bg_color(bm, lv_color_hex(THEME_ACCENT_FILL), LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(bm, lv_color_hex(THEME_ON_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_add_event_cb(bm, on_punit, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
        y += SY(58);
    }
}

/* Validate all rows, then send one SetMeterConfiguration per meter.
 * Returns NULL when sent, else a message. */
static const char * pulse_apply(void) {
    static char err[160];
    double div[NPRES];
    for (int k = 0; k < g_prow_n; k++) {
        const pres_t * p = &PRES[g_prow_res[k]];
        const punit_t * u = &p->u[g_prow_unit[k]];
        char t[16]; snprintf(t, sizeof t, "%s", lv_textarea_get_text(g_prow_ta[k]));
        for (char * c = t; *c; c++) if (*c == ',') *c = '.';
        double v = atof(t);
        div[k] = v > 0 ? punit_to_divider(u, v) : 0;
        if (!(div[k] >= 30 && div[k] <= u->max)) {
            snprintf(err, sizeof err, tr("%s: waarde buiten bereik (%.6g - %.6g %s).",
                                         "%s: value out of range (%.6g - %.6g %s)."),
                     tr(p->nl, p->en),
                     punit_from_divider(u, u->divisor ? u->max : 30), punit_from_divider(u, u->divisor ? 30 : u->max), u->name);
            return err;
        }
    }
    int seq, ok; ma_get_meter_result(&seq, &ok); g_mc_seq0 = seq;
    g_mc_sent = 0;
    for (int k = 0; k < g_prow_n; k++) {
        const pres_t * p = &PRES[g_prow_res[k]];
        const punit_t * u = &p->u[g_prow_unit[k]];
        if (ma_set_meter_config(g_uuid, p->res, u->meter_type, div[k], u->id) == 0) g_mc_sent++;
    }
    return g_mc_sent ? NULL : tr("Versturen mislukt.", "Sending failed.");
}

/* ------------------------------------------------------------- overview */
static const char * usage_name(const char * t) {
    if (!strcmp(t, "elec"))  return tr("Elektriciteit", "Electricity");
    if (!strcmp(t, "gas"))   return tr("Gas", "Gas");
    if (!strcmp(t, "solar")) return tr("Zon", "Solar");
    if (!strcmp(t, "heat"))  return tr("Warmte", "Heat");
    if (!strcmp(t, "water")) return tr("Water", "Water");
    return t;
}
static const char * measure_name(int m) {
    return m == 1 ? tr("slimme meter", "smart meter") : m == 2 ? tr("laser", "laser") : tr("analoog", "analog");
}

static void on_card_btn(lv_event_t * e) {
    const char * u = lv_event_get_user_data(e);
    int is_remove = lv_obj_get_index(lv_event_get_target(e)) & 1;  /* see build_list */
    ma_info_t inf; ma_get_info(&inf);
    snprintf(g_uuid, sizeof g_uuid, "%s", u);
    g_name[0] = 0;
    for (int i = 0; i < inf.n; i++)
        if (!strcmp(inf.dev[i].uuid, u)) snprintf(g_name, sizeof g_name, "%s", inf.dev[i].ident);
    flow_set(is_remove ? F_REM_INTRO : F_CFG_LOADING);
}

static lv_obj_t * mk_btn(lv_obj_t * par, const char * txt, uint32_t col, lv_event_cb_t cb, void * ud, lv_obj_t ** lbl) {
    lv_obj_t * b = lv_btn_create(par);
    lv_obj_set_size(b, SX(200), SY(48));
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_ext_click_area(b, 6);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t * l = lv_label_create(b);
    /* neutral button: normal text; coloured (ok/remove) fills: text on accent */
    lv_obj_set_style_text_color(l, lv_color_hex(col == (uint32_t)THEME_BTN_ACCENT ? THEME_TEXT_HI : THEME_ON_ACCENT), 0);
    lv_obj_set_style_text_font(l, SF(18), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (lbl) *lbl = l;
    return b;
}

static char g_card_uuid[MA_MAX_DEV][48];

static void build_list(const ma_info_t * inf) {
    lv_obj_clean(lst);
    if (inf->seq == 0) {
        lv_label_set_text(lbl_empty, tr("Wachten op gegevens van de meetadapter-driver (hdrv_p1)...",
                                        "Waiting for the meter adapter driver (hdrv_p1)..."));
        lv_obj_clear_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (inf->n == 0) {
        lv_label_set_text(lbl_empty, tr("Geen meetadapter gekoppeld.", "No meter adapter paired."));
        lv_obj_clear_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < inf->n; i++) {
        const ma_dev_t * d = &inf->dev[i];
        snprintf(g_card_uuid[i], sizeof g_card_uuid[i], "%s", d->uuid);
        lv_obj_t * c = lv_obj_create(lst);
        lv_obj_set_size(c, lv_pct(100), SY(140));
        lv_obj_set_style_bg_color(c, lv_color_hex(THEME_CARD_BG), 0);
        lv_obj_set_style_border_width(c, THEME_IS_LIGHT ? 1 : 0, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_radius(c, 14, 0);
        lv_obj_set_style_pad_all(c, 14, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

        /* children 0/1 are the buttons: on_card_btn uses index parity */
        lv_obj_t * bm = mk_btn(c, tr("Meters", "Meters"), THEME_BTN_ACCENT, on_card_btn, g_card_uuid[i], NULL);
        lv_obj_align(bm, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_t * br = mk_btn(c, tr("Verwijderen", "Remove"), COL_WARNBT, on_card_btn, g_card_uuid[i], NULL);
        lv_obj_align(br, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

        lv_obj_t * t = lv_label_create(c);
        lv_obj_set_style_text_font(t, SF(22), 0);
        lv_obj_set_style_text_color(t, lv_color_hex(THEME_TEXT_HI), 0);
        lv_label_set_text(t, d->ident[0] ? d->ident : tr("Meetadapter", "Meter adapter"));
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t * s = lv_label_create(c);
        lv_obj_set_style_text_font(s, SF(18), 0);
        lv_obj_set_style_text_color(s, lv_color_hex(d->dev_status == 2 ? COL_GREEN : d->dev_status == 1 ? COL_RED : COL_AMBER), 0);
        lv_label_set_text(s, d->status_str[0] ? d->status_str :
                          d->dev_status == 2 ? tr("Verbonden", "Connected") :
                          d->dev_status == 1 ? tr("Niet verbonden", "Not connected") : tr("Niet geconfigureerd", "Not configured"));
        lv_obj_align(s, LV_ALIGN_TOP_LEFT, 0, SY(36));

        char m[256] = "";
        size_t o = 0;
        for (int u = 0; u < d->n_usage && o < sizeof m; u++) {
            char pt[32];
            pulse_text(&d->usage[u], pt, sizeof pt);
            o += snprintf(m + o, sizeof m - o, "%s%s: %s%s%s", u ? "   " : "",
                          usage_name(d->usage[u].type), measure_name(d->usage[u].measure),
                          pt[0] ? ", " : "", pt);
        }
        lv_obj_t * ml = lv_label_create(c);
        lv_obj_set_style_text_font(ml, SF(16), 0);
        lv_obj_set_style_text_color(ml, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_width(ml, DISP_HOR - SX(320));
        lv_label_set_long_mode(ml, LV_LABEL_LONG_WRAP);
        lv_label_set_text(ml, m[0] ? m : tr("Geen meters ingesteld - tik op Meters.", "No meters set - tap Meters."));
        lv_obj_align(ml, LV_ALIGN_TOP_LEFT, 0, SY(70));
    }
}

/* ------------------------------------------------------------- flows */
static void ov_buttons(const char * b0, const char * b1, const char * b2) {
    const char * t[3] = { b0, b1, b2 };
    for (int i = 0; i < 3; i++) {
        if (t[i]) { lv_label_set_text(ov_btn_lbl[i], t[i]); lv_obj_clear_flag(ov_btn[i], LV_OBJ_FLAG_HIDDEN); }
        else lv_obj_add_flag(ov_btn[i], LV_OBJ_FLAG_HIDDEN);
    }
}
static void ov_text(const char * title, const char * body, const char * status, uint32_t scol) {
    lv_label_set_text(ov_title, title);
    lv_label_set_text(ov_body, body ? body : "");
    lv_label_set_text(ov_status, status ? status : "");
    lv_obj_set_style_text_color(ov_status, lv_color_hex(scol), 0);
}

static ma_admin_t g_res;   /* last admin result shown */

static void flow_set(int f) {
    g_flow = f;
    if (g_kb) { lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN); lv_keyboard_set_textarea(g_kb, NULL); }
    /* the pulse step has a two-line explanation above its rows */
    if (ov_rows) lv_obj_align(ov_rows, LV_ALIGN_TOP_LEFT, 0, f == F_CFG_PULSE ? SY(120) : SY(84));
    if (f == F_NONE) { lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ov_rows, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ov_body, LV_OBJ_FLAG_HIDDEN);
    const char * T_ADD = tr("Meetadapter toevoegen", "Add meter adapter");
    const char * T_REM = tr("Meetadapter verwijderen", "Remove meter adapter");
    const char * T_CFG = tr("Meters instellen", "Set up meters");
    char buf[256];
    switch (f) {
    case F_ADD_INTRO:
        ov_text(T_ADD, tr("1. Steek de meetadapter in het stopcontact, binnen een paar meter van de Toon.\n"
                          "2. Tik op Koppelen.\n"
                          "3. Druk daarna een paar keer kort op de knop van de meetadapter.",
                          "1. Plug the meter adapter in, within a few metres of the Toon.\n"
                          "2. Tap Link.\n"
                          "3. Then briefly press the button on the meter adapter a few times."), NULL, 0);
        ov_buttons(tr("Koppelen", "Link"), NULL, tr("Annuleren", "Cancel"));
        break;
    case F_ADD_BUSY:
        ov_text(T_ADD, tr("Druk nu een paar keer kort op de knop van de meetadapter.",
                          "Now briefly press the button on the meter adapter a few times."),
                tr("Koppelen... (max. 2 minuten)", "Linking... (up to 2 minutes)"), COL_AMBER);
        ov_buttons(NULL, NULL, tr("Stoppen", "Stop"));
        break;
    case F_ADD_RESULT: {
        int ok = !strcmp(g_res.status, "added") && ma_is_meter_type(g_res.dev_type);
        if (ok)
            ov_text(T_ADD, tr("De meetadapter is gekoppeld. Stel nu in welke meters hij uitleest.",
                              "The meter adapter is linked. Now choose which meters it reads."),
                    tr("Gekoppeld", "Linked"), COL_GREEN);
        else {
            if (!strcmp(g_res.status, "added"))
                snprintf(buf, sizeof buf, tr("Er is een apparaat gekoppeld, maar het is geen meetadapter (%s). "
                                             "Verwijder het eventueel via Instellingen > Z-Wave.",
                                             "A device was linked, but it is not a meter adapter (%s). "
                                             "Remove it via Settings > Z-Wave if needed."), g_res.dev_type);
            else if (g_res.ssa_declined)
                snprintf(buf, sizeof buf, "%s", tr("Dit apparaat vraagt om een S2-pincode; dat wordt niet ondersteund.",
                                                   "This device asks for an S2 PIN code; that is not supported."));
            else
                snprintf(buf, sizeof buf, tr("Koppelen is niet gelukt (%s). Was de adapter eerder aan een andere "
                                             "Toon gekoppeld? Gebruik dan eerst \"Adapter resetten\" en probeer opnieuw.",
                                             "Linking failed (%s). Was the adapter linked to another Toon before? "
                                             "Use \"Reset adapter\" first, then try again."), g_res.status);
            ov_text(T_ADD, buf, tr("Niet gelukt", "Failed"), COL_RED);
        }
        if (ok) {
            snprintf(g_uuid, sizeof g_uuid, "%s", g_res.uuid);
            g_name[0] = 0;
            ov_buttons(tr("Meters instellen", "Set up meters"), NULL, tr("Later", "Later"));
        } else
            ov_buttons(tr("Opnieuw", "Try again"), NULL, tr("Sluiten", "Close"));
        break;
    }
    case F_REM_INTRO:
        if (!g_uuid[0]) {   /* reset: release an adapter paired to another (old) Toon */
            ov_text(tr("Meetadapter resetten", "Reset meter adapter"),
                    tr("Gebruik dit voor een meetadapter die nog aan een andere (oude) Toon gekoppeld is.\n"
                       "1. Steek de meetadapter in binnen 2 meter van deze Toon.\n"
                       "2. Tik op Resetten.\n3. Druk daarna kort op de knop van de meetadapter.\n"
                       "Daarna kun je hem gewoon toevoegen.",
                       "Use this for a meter adapter that is still linked to another (old) Toon.\n"
                       "1. Plug the meter adapter in within 2 metres of this Toon.\n"
                       "2. Tap Reset.\n3. Then briefly press the button on the meter adapter.\n"
                       "After that you can add it as usual."), NULL, 0);
            ov_buttons(tr("Resetten", "Reset"), NULL, tr("Annuleren", "Cancel"));
            break;
        }
        snprintf(buf, sizeof buf, tr("%s verwijderen:\n"
                                     "1. Haal de meetadapter uit het stopcontact en steek hem in binnen 2 meter van de Toon.\n"
                                     "2. Tik op Verwijderen.\n3. Druk daarna kort op de knop van de meetadapter.",
                                     "Removing %s:\n"
                                     "1. Unplug the meter adapter and plug it in within 2 metres of the Toon.\n"
                                     "2. Tap Remove.\n3. Then briefly press the button on the meter adapter."),
                 g_name[0] ? g_name : tr("de meetadapter", "the meter adapter"));
        ov_text(T_REM, buf, tr("Adapter defect of niet meer bij de hand? Tik op \"Niet bij de hand\".",
                               "Adapter broken or no longer at hand? Tap \"Not at hand\"."), THEME_TEXT_DIM);
        ov_buttons(tr("Verwijderen", "Remove"), tr("Niet bij de hand", "Not at hand"), tr("Annuleren", "Cancel"));
        break;
    case F_REM_VERIFY:
        g_busy_since = time(NULL);
        ov_text(T_REM, NULL, tr("Controleren of de adapter weg is...", "Checking the adapter is gone..."), COL_AMBER);
        ov_buttons(NULL, NULL, NULL);
        break;
    case F_REM_BUSY:
        ov_text(T_REM, tr("Druk nu kort op de knop van de meetadapter.", "Now briefly press the button on the meter adapter."),
                tr("Verwijderen... (max. 30 seconden)", "Removing... (up to 30 seconds)"), COL_AMBER);
        ov_buttons(NULL, NULL, tr("Stoppen", "Stop"));
        break;
    case F_REM_RESULT:
        if (!strcmp(g_res.status, "deleted")) {
            /* The controller also answers "deleted" when no adapter button was
             * pressed (nothing / another node excluded): only believe it once
             * the adapter has really left the list. */
            if (g_uuid[0]) { g_verify_force = 0; flow_set(F_REM_VERIFY); break; }
            if (!g_uuid[0])
                ov_text(tr("Meetadapter resetten", "Reset meter adapter"),
                        tr("De meetadapter is losgekoppeld van de oude Toon. Je kunt hem nu toevoegen.",
                           "The meter adapter is released from the old Toon. You can add it now."),
                        tr("Gereset", "Reset done"), COL_GREEN);
            else
                ov_text(T_REM, tr("De meetadapter is verwijderd.", "The meter adapter has been removed."),
                        tr("Verwijderd", "Removed"), COL_GREEN);
            ov_buttons(NULL, NULL, tr("Sluiten", "Close"));
        } else {
            snprintf(buf, sizeof buf, tr("Verwijderen is niet gelukt (%s). Probeer opnieuw, of verwijder de adapter "
                                         "geforceerd als hij defect of weg is.",
                                         "Removing failed (%s). Try again, or force-remove the adapter if it is "
                                         "broken or gone."), g_res.status);
            ov_text(T_REM, buf, tr("Niet gelukt", "Failed"), COL_RED);
            ov_buttons(tr("Opnieuw", "Try again"), g_uuid[0] ? tr("Geforceerd", "Force") : NULL, tr("Sluiten", "Close"));
        }
        break;
    case F_FORCE_CONFIRM:
        ov_text(T_REM, tr("Geforceerd verwijderen haalt de adapter alleen uit de Toon. De adapter zelf blijft "
                          "gekoppeld en moet later gereset worden voordat hij opnieuw gekoppeld kan worden. Doorgaan?",
                          "Force-removing only removes the adapter from the Toon. The adapter itself stays paired "
                          "and must be reset before it can be linked again. Continue?"), NULL, 0);
        ov_buttons(tr("Geforceerd verwijderen", "Force remove"), NULL, tr("Annuleren", "Cancel"));
        break;
    case F_CFG_LOADING:
        g_conf_seq0 = -1; g_caps_seq0 = -1;
        { ma_sensconf_t sc; ma_get_sensor_config(&sc); g_conf_seq0 = sc.seq;
          ma_caps_t cp; ma_get_caps(&cp); g_caps_seq0 = cp.seq; }
        ma_request_sensor_config();
        ma_request_caps(g_uuid);
        g_busy_since = time(NULL);
        ov_text(T_CFG, NULL, tr("Gegevens ophalen...", "Fetching settings..."), COL_AMBER);
        ov_buttons(NULL, NULL, tr("Annuleren", "Cancel"));
        break;
    case F_CFG_EDIT:
        snprintf(buf, sizeof buf, tr("Welke meters leest %s uit?", "Which meters does %s read?"),
                 g_name[0] ? g_name : tr("deze meetadapter", "this meter adapter"));
        ov_text(T_CFG, buf, NULL, 0);
        lv_obj_clear_flag(ov_rows, LV_OBJ_FLAG_HIDDEN);
        build_editor();
        ov_buttons(tr("Opslaan", "Save"), NULL, tr("Annuleren", "Cancel"));
        break;
    case F_CFG_SAVING:
    case F_CFG_PSAVING:
        ov_text(T_CFG, NULL, tr("Opslaan...", "Saving..."), COL_AMBER);
        ov_buttons(NULL, NULL, NULL);
        break;
    case F_CFG_PULSE:
        ov_text(T_CFG, tr("Hoeveel pulsen geeft de meter? Dit staat op de meter of de pulssensor "
                          "(bijv. 1000 imp/kWh, of bij een draaischijfmeter C = 600 omw/kWh).",
                          "How many pulses does the meter give? It is printed on the meter or the "
                          "pulse sensor (e.g. 1000 imp/kWh, or C = 600 rev/kWh on a disc meter)."), NULL, 0);
        lv_obj_clear_flag(ov_rows, LV_OBJ_FLAG_HIDDEN);
        build_pulse_editor();
        ov_buttons(tr("Opslaan", "Save"), NULL, tr("Overslaan", "Skip"));
        break;
    default: break;
    }
}

static void on_ov_btn(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    switch (g_flow) {
    case F_ADD_INTRO:
        if (i == 0 && ma_include_start() == 0) { g_busy_since = time(NULL); flow_set(F_ADD_BUSY); }
        else if (i == 2) flow_set(F_NONE);
        break;
    case F_ADD_BUSY:
        if (i == 2) { ma_include_stop(); flow_set(F_NONE); }
        break;
    case F_ADD_RESULT:
        if (i == 0) flow_set(!strcmp(g_res.status, "added") && ma_is_meter_type(g_res.dev_type) ? F_CFG_LOADING : F_ADD_INTRO);
        else flow_set(F_NONE);
        break;
    case F_REM_INTRO:
        if (i == 0 && ma_exclude_start() == 0) { g_busy_since = time(NULL); flow_set(F_REM_BUSY); }
        else if (i == 1) flow_set(F_FORCE_CONFIRM);
        else if (i == 2) flow_set(F_NONE);
        break;
    case F_REM_BUSY:
        if (i == 2) { ma_exclude_stop(); flow_set(F_NONE); }
        break;
    case F_REM_RESULT:
        if (i == 0) flow_set(F_REM_INTRO);
        else if (i == 1) flow_set(F_FORCE_CONFIRM);
        else flow_set(F_NONE);
        break;
    case F_FORCE_CONFIRM:
        if (i == 0) {
            ma_force_remove(g_uuid);
            g_verify_force = 1;
            flow_set(F_REM_VERIFY);
        } else flow_set(F_NONE);
        break;
    case F_CFG_EDIT:
        if (i == 0) {
            const char * err = editor_apply();
            if (err) { lv_label_set_text(ov_status, err); lv_obj_set_style_text_color(ov_status, lv_color_hex(COL_RED), 0); break; }
            int seq, ok; ma_get_set_result(&seq, &ok); g_set_seq0 = seq;
            g_busy_since = time(NULL);
            flow_set(F_CFG_SAVING);
            if (ma_set_sensor_config(&g_conf) != 0) {
                ov_text(tr("Meters instellen", "Set up meters"), NULL, tr("Versturen mislukt", "Sending failed"), COL_RED);
                g_flow = F_CFG_RESULT; ov_buttons(NULL, NULL, tr("Sluiten", "Close"));
            }
        } else flow_set(F_NONE);
        break;
    case F_CFG_PULSE:
        if (g_kb) { lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN); lv_keyboard_set_textarea(g_kb, NULL); }
        if (i == 0) {
            const char * err = pulse_apply();
            if (err) { lv_label_set_text(ov_status, err); lv_obj_set_style_text_color(ov_status, lv_color_hex(COL_RED), 0); break; }
            g_busy_since = time(NULL);
            flow_set(F_CFG_PSAVING);
        } else flow_set(F_NONE);
        break;
    default:
        flow_set(F_NONE);
        break;
    }
}

/* Poll async results. */
static void tick(lv_timer_t * t) {
    (void)t;
    ma_info_t inf; ma_get_info(&inf);
    if (inf.seq != g_info_seq) { g_info_seq = inf.seq; build_list(&inf); }

    ma_admin_t a; ma_get_admin(&a);
    if ((g_flow == F_ADD_BUSY || g_flow == F_REM_BUSY) && a.seq != g_admin_seq && !a.busy) {
        g_admin_seq = a.seq; g_res = a;
        flow_set(g_flow == F_ADD_BUSY ? F_ADD_RESULT : F_REM_RESULT);
    } else if ((g_flow == F_ADD_BUSY && time(NULL) - g_busy_since > 150) ||
               (g_flow == F_REM_BUSY && time(NULL) - g_busy_since > 45)) {
        ma_admin_timeout();
    }
    if (g_flow != F_ADD_BUSY && g_flow != F_REM_BUSY) g_admin_seq = a.seq;

    if (g_flow == F_REM_VERIFY) {
        static time_t last_req;
        if (time(NULL) - last_req >= 5) { last_req = time(NULL); ma_subscribe(); }  /* fresh full set */
        int present = 0;
        for (int i = 0; i < inf.n; i++) if (!strcmp(inf.dev[i].uuid, g_uuid)) present = 1;
        const char * T_REM = tr("Meetadapter verwijderen", "Remove meter adapter");
        if (!present) {
            ov_text(T_REM, tr("De meetadapter is verwijderd.", "The meter adapter has been removed."),
                    tr("Verwijderd", "Removed"), COL_GREEN);
            g_flow = F_CFG_RESULT; ov_buttons(NULL, NULL, tr("Sluiten", "Close"));
        } else if (time(NULL) - g_busy_since > 20) {
            if (g_verify_force) {
                ov_text(T_REM, tr("De verwijderopdracht is verstuurd, maar de adapter staat nog in de lijst.",
                                  "The remove command was sent, but the adapter is still listed."),
                        tr("Niet gelukt", "Failed"), COL_RED);
                g_flow = F_CFG_RESULT; ov_buttons(NULL, NULL, tr("Sluiten", "Close"));
            } else {
                ov_text(T_REM, tr("De adapter staat nog in de lijst: er is niets verwijderd. Was hij binnen bereik en "
                                  "heb je op de knop gedrukt? Is hij defect of weg, verwijder hem dan geforceerd.",
                                  "The adapter is still listed: nothing was removed. Was it in range and did you "
                                  "press its button? If it is broken or gone, force-remove it."),
                        tr("Niet gelukt", "Failed"), COL_RED);
                g_flow = F_REM_RESULT;      /* Try again / Force / Close */
                ov_buttons(tr("Opnieuw", "Try again"), tr("Geforceerd", "Force"), tr("Sluiten", "Close"));
            }
        }
    }

    if (g_flow == F_CFG_LOADING) {
        ma_sensconf_t sc; ma_get_sensor_config(&sc);
        ma_get_caps(&g_caps);
        int have_conf = sc.seq != g_conf_seq0, have_caps = g_caps.seq != g_caps_seq0;
        if (have_conf && (have_caps || time(NULL) - g_busy_since > 4)) {
            if (!have_caps) g_caps.n = 0;
            g_conf = sc;
            flow_set(F_CFG_EDIT);
        } else if (time(NULL) - g_busy_since > 10) {
            ov_text(tr("Meters instellen", "Set up meters"), NULL,
                    tr("Geen antwoord van de meetadapter-driver.", "No answer from the meter adapter driver."), COL_RED);
            g_flow = F_CFG_RESULT; ov_buttons(NULL, NULL, tr("Sluiten", "Close"));
        }
    } else if (g_flow == F_CFG_SAVING) {
        int seq, ok; ma_get_set_result(&seq, &ok);
        if (seq != g_set_seq0 || time(NULL) - g_busy_since > 10) {
            int done = seq != g_set_seq0 && ok;
            if (done && pulse_rows_wanted()) { flow_set(F_CFG_PULSE); return; }   /* analog: pulses next */
            ov_text(tr("Meters instellen", "Set up meters"),
                    done ? tr("Opgeslagen. Het kan een paar minuten duren voor het verbruik binnenkomt.",
                              "Saved. It can take a few minutes before usage comes in.") : NULL,
                    done ? tr("Opgeslagen", "Saved") : tr("Opslaan is niet gelukt", "Saving failed"),
                    done ? COL_GREEN : COL_RED);
            g_flow = F_CFG_RESULT; ov_buttons(NULL, NULL, tr("Sluiten", "Close"));
        }
    } else if (g_flow == F_CFG_PSAVING) {
        /* one reply per meter; not every driver version may answer, so a
         * timeout reports "sent" and the overview shows what the driver took */
        int seq, ok; ma_get_meter_result(&seq, &ok);
        int got = seq - g_mc_seq0;
        if (got >= g_mc_sent || time(NULL) - g_busy_since > 10) {
            int all = got >= g_mc_sent && ok;
            ma_subscribe();                          /* fresh usageDevicesInfo with the new values */
            ov_text(tr("Meters instellen", "Set up meters"),
                    all ? tr("Opgeslagen. Het kan een paar minuten duren voor het verbruik binnenkomt.",
                             "Saved. It can take a few minutes before usage comes in.")
                        : tr("Verstuurd, maar de meetadapter-driver bevestigde niet. Controleer de waarde in het overzicht.",
                             "Sent, but the meter adapter driver did not confirm. Check the value in the overview."),
                    all ? tr("Opgeslagen", "Saved") : tr("Niet bevestigd", "Not confirmed"),
                    all ? COL_GREEN : COL_AMBER);
            g_flow = F_CFG_RESULT; ov_buttons(NULL, NULL, tr("Sluiten", "Close"));
        }
    }
}

#ifdef SIM
/* Headless sim: BoxTalk can't send, so jump straight to the pulse step for
 * adapter `uuid` with the (mocked) sensor configuration. */
void screen_meteradapters_sim_pulse(const char * uuid) {
    snprintf(g_uuid, sizeof g_uuid, "%s", uuid);
    snprintf(g_name, sizeof g_name, "Meetmodule 1");
    ma_get_sensor_config(&g_conf);
    flow_set(F_CFG_PULSE);
}
#endif

/* ------------------------------------------------------------- screen */
static void back_async(void * u) { (void)u; ui_pop(); }
static void on_back(lv_event_t * e) {
    (void)e;
    if (g_flow == F_ADD_BUSY) ma_include_stop();
    if (g_flow == F_REM_BUSY) ma_exclude_stop();
    flow_set(F_NONE);
    lv_async_call(back_async, NULL);
}
static void on_add(lv_event_t * e) { (void)e; flow_set(F_ADD_INTRO); }
static void on_reset(lv_event_t * e) { (void)e; g_uuid[0] = 0; g_name[0] = 0; flow_set(F_REM_INTRO); }
static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) { g_info_seq = -1; if (tmr) lv_timer_resume(tmr); }
    else if (c == LV_EVENT_SCREEN_UNLOADED && tmr) lv_timer_pause(tmr);
}

static int scr_light_mode = -1;   /* cached UI theme (settings.ui_theme) */

lv_obj_t * screen_meteradapters_create(void) {
    if (scr_root && scr_light_mode == settings.ui_theme) return scr_root;
    if (scr_root) { lv_obj_del(scr_root); scr_root = NULL; }
    if (tmr) { lv_timer_del(tmr); tmr = NULL; }
    scr_light_mode = settings.ui_theme;
    g_flow = F_NONE;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_UNLOADED, NULL);

    lv_obj_t * x = lv_btn_create(scr_root);
    lv_obj_set_size(x, (DISP_HOR < 1000) ? 68 : 74, (DISP_VER < 600) ? 46 : 50);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, SX(-16), SY(12));
    lv_obj_set_style_bg_color(x, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(x, 12, 0);
    lv_obj_set_ext_click_area(x, 25);
    lv_obj_add_event_cb(x, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(xl, SF(22), 0);
    lv_obj_set_style_text_color(xl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_center(xl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("Meetadapters", "Meter adapters"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(24), SY(20));

    lst = lv_obj_create(scr_root);
    lv_obj_remove_style_all(lst);
    lv_obj_set_size(lst, DISP_HOR - SX(44), DISP_VER - SY(170));
    lv_obj_set_pos(lst, SX(22), SY(80));
    lv_obj_set_flex_flow(lst, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(lst, SY(12), 0);

    lbl_empty = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_empty, SF(20), 0);
    lv_obj_set_style_text_color(lbl_empty, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_align(lbl_empty, LV_ALIGN_TOP_LEFT, SX(24), SY(100));
    lv_label_set_text(lbl_empty, "");

    lv_obj_t * add = mk_btn(scr_root, tr("Meetadapter toevoegen", "Add meter adapter"), COL_OKBTN, on_add, NULL, NULL);
    lv_obj_set_width(add, SX(320));
    lv_obj_align(add, LV_ALIGN_BOTTOM_LEFT, SX(22), SY(-18));
    lv_obj_t * rst = mk_btn(scr_root, tr("Adapter resetten", "Reset adapter"), THEME_BTN_ACCENT, on_reset, NULL, NULL);
    lv_obj_set_width(rst, SX(260));
    lv_obj_align(rst, LV_ALIGN_BOTTOM_LEFT, SX(362), SY(-18));

    /* overlay for the flows */
    ov = lv_obj_create(scr_root);
    lv_obj_set_size(ov, DISP_HOR, DISP_VER);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_70, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * card = lv_obj_create(ov);
    lv_obj_set_size(card, DISP_HOR - SX(80), DISP_VER - SY(60));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    ov_title = lv_label_create(card);
    lv_obj_set_style_text_font(ov_title, SF(24), 0);
    lv_obj_set_style_text_color(ov_title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_align(ov_title, LV_ALIGN_TOP_LEFT, 0, 0);
    ov_body = lv_label_create(card);
    lv_obj_set_style_text_font(ov_body, SF(18), 0);
    lv_obj_set_style_text_color(ov_body, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_width(ov_body, DISP_HOR - SX(130));
    lv_label_set_long_mode(ov_body, LV_LABEL_LONG_WRAP);
    lv_obj_align(ov_body, LV_ALIGN_TOP_LEFT, 0, SY(44));
    ov_rows = lv_obj_create(card);
    lv_obj_remove_style_all(ov_rows);
    lv_obj_set_size(ov_rows, DISP_HOR - SX(130), SY(240));
    lv_obj_align(ov_rows, LV_ALIGN_TOP_LEFT, 0, SY(84));
    lv_obj_clear_flag(ov_rows, LV_OBJ_FLAG_SCROLLABLE);
    ov_status = lv_label_create(card);
    lv_obj_set_style_text_font(ov_status, SF(18), 0);
    lv_obj_set_width(ov_status, DISP_HOR - SX(130));
    lv_label_set_long_mode(ov_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(ov_status, LV_ALIGN_BOTTOM_LEFT, 0, SY(-62));
    for (int i = 0; i < 3; i++) {
        ov_btn[i] = mk_btn(card, "", i == 2 ? THEME_BTN_ACCENT : (i == 1 ? COL_WARNBT : COL_OKBTN),
                           on_ov_btn, (void *)(intptr_t)i, &ov_btn_lbl[i]);
        lv_obj_set_width(ov_btn[i], SX(i == 0 ? 260 : 200));
    }
    lv_obj_align(ov_btn[0], LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_align(ov_btn[1], LV_ALIGN_BOTTOM_RIGHT, -SX(280), 0);
    lv_obj_align(ov_btn[2], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    /* number pad for the pulse step (shown while a value field is active) */
    g_kb = lv_keyboard_create(ov);
    lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_size(g_kb, DISP_HOR, DISP_VER / 2);
    lv_obj_align(g_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(g_kb, on_kb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(g_kb, on_kb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);

    g_info_seq = -1;
    tmr = lv_timer_create(tick, 500, NULL);
    tick(NULL);
    return scr_root;
}
