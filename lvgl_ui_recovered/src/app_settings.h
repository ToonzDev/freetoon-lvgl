#ifndef TOON_APP_SETTINGS_H
#define TOON_APP_SETTINGS_H
#include <stddef.h>

/* Settings that a marketplace app declares in its manifest.json, edited in
 * a generic web form on :10081 (STRATEGY pillar B, BACKLOG #60):
 *
 *   "settings": {
 *     "file": "inverters.conf",            KEY=VALUE file in the app's dir
 *     "title": "...", "intro": "...",
 *     "fields": [
 *       {"key":"TYPE","label":"Merk","type":"select","options":[{"value":"x","label":"X"}]},
 *       {"key":"HOST","label":"IP","type":"text","placeholder":"..","default":"..",
 *        "show":"TYPE=a|b",                 row visible only for these TYPE values
 *        "guards":"TOKEN,PASS"},             changing HOST needs these secrets re-entered
 *       {"key":"TOKEN","label":"..","type":"secret","help":".."}
 *     ]}
 *
 * Secrets are write-only: the form never shows them, an empty field keeps
 * the stored value. The app re-reads its file when the mtime changes. */

/* Directory of an installed integration; 0 = ok (id checked: [a-z0-9_-]). */
int app_settings_dir(const char * id, char * out, size_t n);

/* Append the form for app `id` to buf at *pos (HTML, posting to `action`).
 * 0 = done, -1 = app missing or without a settings block. */
int app_settings_form(const char * id, const char * action, const char * redirect, char * buf, size_t n, int * pos);

/* Apply a urlencoded form body. 0 = saved; otherwise msg says why not
 * (e.g. a guarded secret must be re-entered). */
int app_settings_save(const char * id, const char * body, char * msg, size_t msgn);

/* First installed app whose manifest has "solar_source": true. 1 = found. */
int app_settings_find_solar(char * id, size_t idn, char * name, size_t namen);

/* Value of KEY in the app's settings file ("" if unset). 1 = present. */
int app_settings_get(const char * id, const char * key, char * out, size_t n);

#endif
