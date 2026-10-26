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
     Add an explicit Refresh button (similar to the update modal or Teletekst) in the Marketplace header to force a re-fetch of `catalog/index.json` and rescan of local manifests on demand.
