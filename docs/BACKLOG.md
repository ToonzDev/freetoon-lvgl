# FreeToon Backlog

## Marketplace

### 1. Fix Marketplace Catalog Refresh & In-Memory Row Rebuild
- **Issue**: Navigating to the Marketplace without restarting the UI does not show newly pushed integration updates or catalog changes. Restarting the UI is currently required for updates to appear.
- **Root Causes**:
  1. **Static `prev_count` in `refresh_cb()` (`screen_marketplace.c`)**:
     `static int prev_count = -1;` retains its value across screen open/close cycles because it is never reset in `screen_marketplace_create()`. When re-entering Marketplace, if `entry_count == prev_count`, `build_rows()` is not invoked.
  2. **GitHub Raw CDN Caching**:
     `raw.githubusercontent.com` caches HTTP GET responses for up to 5 minutes (`max-age=300`). Requests to `CATALOG_URL` need a cache-busting query parameter (e.g. `?t=<timestamp>`) or `Cache-Control: no-cache` header.
  3. **In-Memory Installed Manifest Cache**:
     `tile_slots_integration_by_id()` relies on `g_integ[]` which is only rescanned from `/mnt/data/integrations` on UI startup or after an install/uninstall action. Opening Marketplace should rescan local manifests via `tile_slots_init()`.
  4. **Manual Refresh Action**:
     Add an explicit Refresh button in the Marketplace header to force a re-fetch of `catalog/index.json` and rescan of local manifests on demand.

---

## Homescreen & Gestures

### 2. Swipe Gesture Treated as Click on Initial Tile Component
- **Issue**: When swiping left on the homescreen (over the tile area to switch from page 1 to page 2), the component where the swipe started also receives an unintended click event, opening that tile's modal/app while switching pages.
- **Root Cause**:
  In `screen_home.c`, `on_home_gesture()` handles `LV_EVENT_GESTURE` and switches pages via `home_show_page()`, but does not consume/cancel the active touch on the input device. When the finger is lifted, LVGL delivers `LV_EVENT_CLICKED` to the child tile button under the initial touch point.
- **Solution**:
  1. In `on_home_gesture()`, call `lv_indev_wait_release(indev)` whenever a swipe gesture is recognized to suppress subsequent click/release events for that touch sequence.
  2. In tile click handlers, check if the input device experienced dragging or scrolling before executing the click action (e.g., verify `lv_indev_get_gesture_dir(indev) == LV_DIR_NONE` or check displacement).
