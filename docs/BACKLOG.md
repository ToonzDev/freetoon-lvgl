# FreeToon Backlog

## Completed / In Testing

### 1. Marketplace Catalog Dynamic Refresh & Rebuild (Implemented)
- **Status**: Fixed in current working build.
- **Implemented Fixes**:
  - Re-fetch catalog on every Marketplace screen entry (`start_catalog_fetch()`).
  - Added timestamp query parameter (`?t=<timestamp>`) to bypass GitHub CDN 5-minute HTTP cache.
  - Added explicit Refresh button (`LV_SYMBOL_REFRESH`) in Marketplace header.
  - Rescan local installed integration manifests (`tile_slots_init()`).
  - Cleaned up refresh timer and reset entry change detector across screen navigation.
  - Enforce larger touch target for close ('X') and refresh buttons.
  - Automatically sort apps with an update available to the top of the Marketplace list.

### 2. Homescreen Swipe Left vs. Tile Click Discrimination (Implemented)
- **Status**: Fixed in current working build.
- **Implemented Fixes**:
  - Added `lv_indev_wait_release(indev)` inside `on_home_gesture()` on recognized horizontal swipes to cancel pending release/click dispatch on the pressed tile.
  - Added `tile_click_wrapper()` to check pointer gesture direction and displacement (`gesture_sum`, `scroll_sum` > 15px); any swipe drag completely suppresses tile click activation.

### 3. Page 2 Tile Slot 1 (Linksboven) Assignment Conflict (Implemented)
- **Status**: Fixed in current working build.
- **Implemented Fixes**:
  - `tile_slots_bind()` clears `settings.tile_rotate_enabled` when assigning a specific integration to `TILE_SLOT_P1_0`.
  - Auto-rotate background loop and tap handler in `screen_home.c` only run on slot 0 if slot 0 is unassigned (`!tile_slots_binding(TILE_SLOT_P1_0)`).

### 4. Dynamic Font Scaling for Long Titles on Page 2 Tiles (Implemented)
- **Status**: Fixed in current working build.
- **Implemented Fixes**:
  - `render_tile_slot()` and `render_meta_into()` inspect the character length of `m->latest_value`.
  - For values with `length > 15` (e.g. news headlines, Teletekst articles), the label enables multi-line text wrapping (`LV_LABEL_LONG_WRAP`) within the tile's content width and scales font dynamically to `SF(18)` or `SF(15)`. Short numeric values retain large bold fonts (`SF(28)` / `SF(48)`).

### 5. Layout Editor Close & Re-open Double Free Crash (Implemented)
- **Status**: Fixed in current working build.
- **Implemented Fixes**:
  - In `screen_layout_editor.c`, `on_cancel()` and `screen_layout_editor_show()` now properly reset all dangling pointers (`rects[]`, `size_btns[]`, `theme_btn`, `page_btn`, `sel_lbl`) when the editor is closed, preventing use-after-free / double-free crashes when re-opening Indeling.

### 6. Buienradar Rain Forecast Graph & Multi-Timeframe Radar (Implemented)
- **Status**: Completed in Release 0.9.77.
- **Implemented Features**:
  - Integrated dedicated Buienradar rain background polling thread (`buienradar_rain.c`) fetching 3-hour forecast (`RainHistoryForecast`) and 24-hour forecast (`Rain24Hour`).
  - Added homescreen tile `LT_RAINFALL` with toggle buttons (`[3u]`, `[24u]`), rainfall bars, and dynamic time markers.
  - Added ambient dim screen block `DB_RAINFALL` with black background (`#000000`), borderless floating chart, and time labels.
  - Added multi-timeframe view to detailed forecast screen (`screen_forecast.c`) with floating pill bar (`[ Radar ]`, `[ 3 uur ]`, `[ 24 uur ]`).
  - Added horizontal threshold divider lines and labels (*Zwaar*, *Matig*, *Licht*) matching Buienradar mobile app.
  - Implemented clean centered *"Geen neerslag verwacht"* display when dry, hiding chart and redundant top messages.
  - Added GPS coordinate configuration in Settings → Weer with automatic seeding from `buienradar.userSettings.json` or fallback.

### 7. Hourly Weather Forecast Screen, Clock Hour Labels & Layout Polish (Implemented)
- **Status**: Completed in Release 0.9.78.
- **Implemented Features & Fixes**:
  - **Detailed 24-Hour Weather Forecast Screen (`screen_weather_hourly.c`)**:
    - Clickable 5-day forecast cards on the weather screen (`screen_forecast.c`) opening an hourly breakdown for any selected day.
    - Full 24-hour vertical scrollable list with auto-scroll to the current hour on today's view.
    - Shows hourly time, weather icon, temperature, feel temperature, rain probability & rainfall (mm), Beaufort wind force, and rotated wind direction arrow.
    - Integrated day switcher buttons (`<` and `>`) sized identically to the close button (`X`) and placed snug around the title block.
    - High-contrast warm amber temperature text in light mode for excellent legibility.
  - **24-Hour Rain Chart Clock Labels**:
    - Replaced relative time labels (`+6u`, `+12u`, etc.) with actual forecast hour numbers (`10`, `16`, `22`, etc.) on both the homescreen rain tile and the big forecast screen.
  - **Homescreen Rain Tile Proportions & Margins**:
    - Kept header *"Neerslag"* title next to `[3u]` and `[24u]` toggle buttons.
    - Dropped summary subtitle line on compact tiles (e.g. 3x2).
    - Capped graph height to 60px on compact tiles to avoid overlapping the header row.
    - Increased threshold label margin (44px) so *Zwaar*, *Matig*, and *Licht* sit cleanly inside tile padding without clipping.
  - **Dim Screen Program State & Setpoint Alignment**:
    - Horizontally centered the program state and target temperature row on the dim thermostat block (`DB_THERMO`).
    - Anchored flame/faucet icons below the centered label.
    - Automatically hide the *"Neerslag"* tile title when no rain is expected, leaving *"Geen neerslag verwacht"* centered.
  - **Forecast Screen Layout**:
    - Increased clearance above the rain chart to ensure no collision with the subtitle text.

---

## Open / Backlog

### 8. Doorbell Live Snapshot/Feed Investigation ('feed unavailable')
- **Status**: Open / Investigation needed.
- **Description**:
  - After pressing the physical doorbell, the Toon correctly opens the doorbell popup via WebSocket trigger, but displays *"Doorbell: snapshot unavailable"*.
- **Investigation Points**:
  - **Camera Proxy vs. Event Image**: Battery-powered Eufy doorbells remain in sleep mode; check if `camera.doorbell` provides a snapshot immediately or if `image.doorbell_event_image` holds the captured doorbell press frame.
  - **HTTP Response Verification**: Check what `/usr/bin/curl` actually downloads from `http://<ha>:8123/api/camera_proxy/camera.doorbell` to `/tmp/doorbell.jpg` (HTTP status code, 401/404/500 JSON vs valid JPEG header `FF D8 FF`).
  - **Stream/Snapshot Wake-up Latency**: Determine if a short delay or wake-up call (or MJPEG stream via go2rtc/RTSP) is needed for battery-powered cameras.

### 9. Settings Cleanup: Remove Redundant Options from 'Scherm'
- **Status**: Open / Cleanup.
- **Description**:
  - Clean up and streamline `Instellingen -> Scherm` (`screen_settings.c`) by removing obsolete/redundant configuration rows:
    1. **Taal (Language selection)**: Remove toggle; streamline default UI language.
    2. **Licht thema: grote waarde = binnentemp**: Hardcode to 1 (`settings.stock_big_indoor = 1`) and remove the row.
    3. **Maanstand op kloktegel**: Obsolete feature on modern clock tile; remove switch and associated row.

### 10. Sonos Big Screen: Playback Controls (Next/Pause/Prev) & Volume Keys (Fixed in v1.0.6)
- **Status**: Fixed in `freetoon-integrations` (v1.0.6).
- **Root Cause Identified**:
  1. **Binary ABI Struct Drift (`settings_t`)**: In commit `4796144` (Buienradar GPS settings), `weather_lat[16]` and `weather_lon[16]` were inserted into `settings_t` in `settings.h`. This shifted all subsequent members by 32 bytes. Because `sonos_ui.toon2.so` was compiled on Oct 25 before this commit, its read of `settings.sonos_enabled` was evaluating an unaligned offset which was `0`.
  2. **Legacy Guard in Integration**: `send_upnp_cmd()` and `sonos_enqueue_and_play()` guarded all calls with `if (!settings.sonos_enabled || !g_active_ip[0]) return;`. Because `sonos_enabled` evaluated to `0`, all transport (play/pause/next/prev) and volume commands silently aborted, while playlist track enqueueing succeeded because `sonos_play_playlist_track()` only checked `!g_active_ip[0]`.
- **Implemented Fixes**:
  - Removed `!settings.sonos_enabled` check from `send_upnp_cmd()` and `sonos_enqueue_and_play()` so the standalone Sonos integration relies solely on active speaker connection (`g_active_ip[0]`).
  - Added click and command diagnostic logging to `screen_sonos.c` and `sonos.c`.
  - Recompiled all integration packages against the updated `settings.h` ABI.
  - Bumped Sonos version to `1.0.6` in `manifest.json` and `catalog/index.json`.
  - **Marketplace Auto-Restart**: When updating a plugin in Marketplace, `toonui` now automatically restarts via `ui_request_restart()` so new `.so` libraries in RAM are immediately loaded from flash.
  - **Tmpfs Hygiene & Cleanup**:
    - Purged stale `freetoon.<pid>` update folders and added exit traps to `install.sh`.
    - Consolidated Domoticz cookie jar to a single persistent file `/var/volatile/tmp/.dz_cookies` instead of per-process `.dz_cookies_<pid>`.
    - Auto-cleanup of stale update bundles and legacy cookies on `toonui` and `ui_launcher.sh` boot.


