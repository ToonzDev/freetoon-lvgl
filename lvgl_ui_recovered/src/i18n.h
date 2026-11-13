#ifndef TOON_I18N_H
#define TOON_I18N_H

#include "settings.h"
#include <time.h>
#include <stdio.h>

typedef enum { LANG_NL = 0, LANG_EN = 1, LANG_FR = 2, LANG_COUNT } lang_t;

/* French lookup (i18n_fr.c). Call sites keep the two-argument tr("nl", "en")
 * form; the French text is looked up from a table keyed on that (nl, en) pair,
 * so adding a language does not touch the ~870 call sites. A pair that is not
 * in the table falls back to the English text. Coverage and printf-format
 * consistency are checked by tools/i18n_fr_check.py. */
const char * tr_fr(const char *nl, const char *en);

/* Return the string for the current UI language.
 * Takes effect on the next UI build/refresh — no restart needed. */
static inline const char * tr(const char *nl, const char *en) {
    switch (settings.language) {
    case LANG_EN: return en;
    case LANG_FR: return tr_fr(nl, en);
    default:      return nl;
    }
}

/* Pick one of three per-language literals (for small fixed tables). */
static inline const char * i18n_pick(const char *nl, const char *en, const char *fr) {
    switch (settings.language) {
    case LANG_EN: return en;
    case LANG_FR: return fr;
    default:      return nl;
    }
}

/* Native name of a language, for the settings selector. */
static inline const char * i18n_lang_name(int lang) {
    switch (lang) {
    case LANG_EN: return "English";
    case LANG_FR: return "Fran\xc3\xa7" "ais";
    default:      return "Nederlands";
    }
}

/* Localised date formatting. strftime() honours the C locale, which on the
 * Toon is plain "C" → always English day/month names regardless of the UI
 * language. So we build the date string ourselves from the language tables
 * instead of relying on %A/%B. Day name is capitalised to match the old
 * "Tuesday 16 June" look. tm_wday: 0=Sun..6=Sat; tm_mon: 0=Jan..11=Dec. */
static inline const char * i18n_day_long(int wday) {
    static const char * nl[7] = { "Zondag","Maandag","Dinsdag","Woensdag",
                                  "Donderdag","Vrijdag","Zaterdag" };
    static const char * en[7] = { "Sunday","Monday","Tuesday","Wednesday",
                                  "Thursday","Friday","Saturday" };
    static const char * fr[7] = { "Dimanche","Lundi","Mardi","Mercredi",
                                  "Jeudi","Vendredi","Samedi" };
    if (wday < 0 || wday > 6) wday = 0;
    return i18n_pick(nl[wday], en[wday], fr[wday]);
}
static inline const char * i18n_month_long(int mon) {
    static const char * nl[12] = { "januari","februari","maart","april","mei",
        "juni","juli","augustus","september","oktober","november","december" };
    static const char * en[12] = { "January","February","March","April","May",
        "June","July","August","September","October","November","December" };
    static const char * fr[12] = { "janvier","f\xc3\xa9vrier","mars","avril","mai",
        "juin","juillet","ao\xc3\xbbt","septembre","octobre","novembre","d\xc3\xa9" "cembre" };
    if (mon < 0 || mon > 11) mon = 0;
    return i18n_pick(nl[mon], en[mon], fr[mon]);
}
static inline const char * i18n_day_short(int wday) {
    static const char * nl[7] = { "zo","ma","di","wo","do","vr","za" };
    static const char * en[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char * fr[7] = { "dim","lun","mar","mer","jeu","ven","sam" };
    if (wday < 0 || wday > 6) wday = 0;
    return i18n_pick(nl[wday], en[wday], fr[wday]);
}
static inline const char * i18n_month_short(int mon) {
    static const char * nl[12] = { "jan","feb","mrt","apr","mei","jun",
                                   "jul","aug","sep","okt","nov","dec" };
    static const char * en[12] = { "Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec" };
    static const char * fr[12] = { "janv","f\xc3\xa9vr","mars","avr","mai","juin",
                                   "juil","ao\xc3\xbbt","sept","oct","nov","d\xc3\xa9" "c" };
    if (mon < 0 || mon > 11) mon = 0;
    return i18n_pick(nl[mon], en[mon], fr[mon]);
}
/* "Dinsdag 16 juni" / "Tuesday 16 June" / "Mardi 16 juin" */
static inline void i18n_date_long(char *out, int n, const struct tm *tm) {
    snprintf(out, n, "%s %d %s", i18n_day_long(tm->tm_wday),
             tm->tm_mday, i18n_month_long(tm->tm_mon));
}
/* "di 16 jun" / "Tue 16 Jun" / "mar 16 juin" */
static inline void i18n_date_short(char *out, int n, const struct tm *tm) {
    snprintf(out, n, "%s %d %s", i18n_day_short(tm->tm_wday),
             tm->tm_mday, i18n_month_short(tm->tm_mon));
}

#endif
