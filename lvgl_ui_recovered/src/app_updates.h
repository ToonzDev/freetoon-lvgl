#ifndef TOON_APP_UPDATES_H
#define TOON_APP_UPDATES_H

/* Marketplace app updates (backlog #35).
 *
 * Once a day, next to the FreeToon release check (update_check.c), fetch the
 * marketplace catalog and compare each installed integration's manifest
 * version with the catalog version. The result drives the home-screen banner
 * ("2 app-updates"), the Marketplace tile subtitle in Settings and the opt-in
 * nightly "update apps automatically".
 *
 * Plugin ABI gate: a catalog entry may carry "min_toonui": "1.0.8". An update
 * that needs a newer toonui than the running build is never counted or
 * offered (the loader would refuse the new .so anyway, and the app would
 * vanish from the Toon until FreeToon itself is updated).
 *
 * All functions are safe to call from any thread. app_updates_check_now() and
 * app_updates_install_all() block (network / install helper) — never call
 * them on the LVGL thread. */

#include <stddef.h>

/* 1 when an entry with this min_toonui value may be installed on the
 * running build ("" / missing = always). Shared with screen_marketplace.c. */
int  app_catalog_compatible(const char * min_toonui);

/* Matching '}' of the JSON object starting at `open` ('{'), skipping nested
 * objects and strings; NULL when unterminated. */
const char * app_json_obj_end(const char * open);

/* Fetch the catalog (TLS-verified) and recompute. Blocking. */
void app_updates_check_now(void);

/* Re-read the installed manifests against the last fetched catalog, without
 * network access. Call after an install/uninstall. */
void app_updates_recompute(void);

/* Number of installed apps with a compatible newer catalog version.
 * When `summary` is non-NULL it receives e.g. "Sonos Player 1.0.9, NOS
 * Teletekst 1.0.7" (truncated to `n`). */
int  app_updates_get(char * summary, size_t n);

/* Run the install helper for every app with an update. Blocking. Returns the
 * number of apps updated successfully. The caller restarts the UI when > 0
 * (the new plugin .so is only loaded at start-up). */
int  app_updates_install_all(void);

/* Home banner: hide it until the set of available updates changes. The
 * dismissal survives a UI restart. */
int  app_updates_banner_wanted(void);
void app_updates_dismiss_banner(void);

#endif
