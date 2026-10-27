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

---

## Open / Backlog

### 7. Doorbell Live Snapshot/Feed Investigation ('feed unavailable')
- **Status**: Open / Investigation needed.
- **Description**:
  - After pressing the physical doorbell, the Toon correctly opens the doorbell popup via WebSocket trigger, but displays *"Doorbell: snapshot unavailable"*.
- **Investigation Points**:
  - **Camera Proxy vs. Event Image**: Battery-powered Eufy doorbells remain in sleep mode; check if `camera.doorbell` provides a snapshot immediately or if `image.doorbell_event_image` holds the captured doorbell press frame.
  - **HTTP Response Verification**: Check what `/usr/bin/curl` actually downloads from `http://<ha>:8123/api/camera_proxy/camera.doorbell` to `/tmp/doorbell.jpg` (HTTP status code, 401/404/500 JSON vs valid JPEG header `FF D8 FF`).
  - **Stream/Snapshot Wake-up Latency**: Determine if a short delay or wake-up call (or MJPEG stream via go2rtc/RTSP) is needed for battery-powered cameras.

### 8. Settings Cleanup: Remove Redundant Options from 'Scherm'
- **Status**: Open / Cleanup.
- **Description**:
  - Clean up and streamline `Instellingen -> Scherm` (`screen_settings.c`) by removing obsolete/redundant configuration rows:
    1. **Taal (Language selection)**: Remove toggle; streamline default UI language.
    2. **Licht thema: grote waarde = binnentemp**: Hardcode to 1 (`settings.stock_big_indoor = 1`) and remove the row.
    3. **Maanstand op kloktegel**: Obsolete feature on modern clock tile; remove switch and associated row.

### 9. Restore Program State & Setpoint Temperature on Dim Screen Thermostat Block
- **Status**: Open / High Priority (First task for tomorrow).
- **Description**:
  - The program state (e.g. *Comfort*, *Thuis*, *Slapen*, *Weg*) and setpoint temperature (e.g. *"naar 20.0°C"*) on the thermostat tile in dim mode (`DB_THERMO` in `screen_dim_grid.c` / `screen_dim.c`) disappeared.
  - Audit the commit history where this was previously implemented, identify where `lbl_program` and `lbl_setpoint` were created and aligned relative to `lbl_temp`, and restore them permanently so they do not get lost across layout/dim changes.

