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

### 8. Release 0.9.79: Classic Look Integrations, Dynamic Filtering & Auto Light Theme (Implemented)
- **Status**: Completed in Release 0.9.79.
- **Implemented Features**:
  - **Dynamic Integrations in Classic/Stock Theme (`screen_home_stock.c`)**:
    - Tile picker modal (`open_picker`) now lists all installed marketplace integrations (Sonos, Temperature Logger, NOS Teletekst, Domoticz, etc.) and enabled extra local tiles (Afval, Water P1).
    - Integrations render in native stock card style with matching Open Sans typography, dynamic title, live value, and subtitle.
    - Live values and subtitles update automatically every second from BoxTalk/REST state cache (and directly from `templogger` / `wastecollection`).
    - Tapping an integration tile opens its full native detail screen (`open_integration_by_id`).
    - Dynamic layout persistence in `settings.stock_tiles` preserves integration IDs seamlessly.
  - **Dynamic Tile Filtering**:
    - `tile_is_available()` dynamically filters the picker so disabled services (Ventilation, Calendar, Trackers, Solar, Fan+Lamp) are hidden.
    - Default layout slots are sanitized so disabled hardware does not appear as dead tiles.
  - **Auto Light Theme for Classic Look**:
    - Switching to Classic Toon look in *Instellingen → Scherm* automatically enables the Light Theme (`settings.home_light_mode = 1`) and live updates the UI and switches.
  - **Carousel Arrow Wrap-Around**:
    - Pressing right arrow on page 4 wraps to page 1; pressing left arrow on page 1 wraps to page 4.
  - **Marketplace Auto-Restart**:
    - Successfully updating a plugin in the Marketplace automatically triggers `ui_request_restart()` to reload fresh `.so` libraries.
  - **Tmpfs Hygiene & Cleanup**:
    - Auto-purge of stale `/tmp/freetoon.*` update folders and consolidated Domoticz cookie jar (`/var/volatile/tmp/.dz_cookies`).

### 9. Release 0.9.80: Vertical Usage Bars, Preset Black Screen & UI Polish (Implemented)
- **Status**: Completed in Release 0.9.80.
- **Implemented Features**:
  - **Vertical Segmented Usage Bars (`dim_bars_vertical`)**:
    - New toggle switch in *Instellingen → Scherm*: *"Balken: verticaal (klassieke Toon-stijl)"*.
    - Replaces continuous/horizontal energy and gas bars with 12-segment vertical bars inspired by the classic Toon look (bottom 7 green, top 5 amber/yellow, unlit dark grey).
    - Fully supported in both standard standby screen (`screen_dim.c`) and custom dim grid (`screen_dim_grid.c`).
  - **Black Standby Screen on Slapen & Weg Presets**:
    - Optional setting (*"Zwart scherm bij Slapen/Weg"*) that blanks the dim screen completely when the heating program is in *Slapen* or *Weg* preset, matching stock Eneco GUI behavior.
    - Automatically wakes up on screen touch or whenever the program state changes.
  - **Classic Look Home Screen Alignment**:
    - Shifted carousel `G_X0` to 64 and refined the collapsible Devices/HA handle in `screen_home_stock.c` to prevent any overlap with the left tiles on both Toon 1 and Toon 2.
  - **Auto-Rotate Modal Layout Fix**:
    - Resolved collision in *Tegel automatisch wisselen* where the *Toepassen* button overlapped the marketplace notice and checkbox list.
    - Checkbox list now dynamically sizes and clips neatly within view.
  - **Z-Wave Rename Keyboard Fix**:
    - Decoupled the virtual keyboard from the rename card in `screen_zwave.c`, placing the card at the top and the full keyboard at the bottom so the device name input field is never obstructed during typing.

### 10. Release 0.9.81: Solar Integration, Vertical 1-Col Bars, Master Schedule Sync & Appliances Cleanup (Implemented)
- **Status**: Completed in Release 0.9.81.
- **Implemented Features**:
  - **Solar Return Metrics Tab in Energy Statistics Screen (`screen_stats.c`)**:
    - Added dedicated *Zon* (Solar) tab alongside Elektriciteit, Gas, Water en CV bar.
    - Queries DSMR smart meter return registers (`elec_flow_prod`, `elec_quantity_nt_prod`, `elec_quantity_lt_prod`) with automatic summation of normal- and low-tariffs.
    - Built-in automatic fallback to `solar_flow` and `solar_quantity` for dedicated Toon Zon solar inverters.
    - Gracefully handles absent or empty solar RRD archives without errors or crashes.
  - **Dim Block Zon (`DB_SOLAR`) & 1-Column Vertical Bars**:
    - Added dedicated *Zon* block to custom dim layout grid (`screen_dim_grid.c`, `layout.c`, `layout.h`, `screen_dim_layout_editor.c`).
    - Allowed minimum width of 1 unit (`min_w = 1`) for energy, gas, and solar blocks when vertical bar mode (`dim_bars_vertical`) is enabled.
    - When 1-column wide, the value and measurement unit (e.g. `m³`, `W`) are stacked onto two rows with adjusted chart height so labels never truncate or clip outside tile bounds.
  - **Master Toon Heating Schedule Synchronization (`screen_schedule.c`)**:
    - Fixed schedule editing from a Client Toon to reliably update the Master Toon via XML payload (`updateThermostatSettings` and `setThermostatSettings` on `happ_thermstat`), ensuring persistence across master restarts.
  - **Obsolete Apparaten & NILM UI Removal**:
    - Removed redundant *Apparaten* tile from Instellingen (`screen_settings.c`).
    - Removed *Apparaten >* button from Energy Statistics (`screen_stats.c`).
    - Removed obsolete step-change timeline (`screen_energy_timeline.c`) and custom appliance naming screen (`screen_appliances.c`), freeing memory and simplifying UI.

### 11. Release 0.9.82: Auto-Dim Wake Fix, Screen Cleaning Fix, Tile Localization, HA/Z-Wave Modal Overlaps & Stats Euro Mode (Implemented)
- **Status**: Completed in Release 0.9.82.
- **Implemented Features & Fixes**:
  - **Auto-Dim Spontaneous Wake Fix (`screen_settings.c`)**:
    - Fixed an issue where the Toon would automatically wake from the dim standby screen every few minutes without user touch.
    - When the auto-home timer expired while dimmed, `screen_settings_close_modals()` was called, invoking `clean_finish()`.
    - `clean_finish()` previously marked user activity unconditionally (`ui_mark_activity()`), causing `ui_idle_tick()` to register pending wake and dismiss the dim screen.
    - Added an active check guard in `clean_finish()` (`if (!g_clean_overlay && !g_clean_tmr) return;`) to ensure inactive screen-cleaning logic never emits false activity wakeups.
  - **Screen Cleaning Stability on Toon 1 (`screen_settings.c`)**:
    - Completely decoupled cleaning overlay cleanup from UI timer callbacks to prevent access to deallocated objects on Toon 1.
  - **Energy Statistics: Euro Display Mode, Rates Editing & Gas Polish (`screen_stats.c`, `stats.c`, `settings.c`, `settings.h`, `client_link.c`)**:
    - Added toggle between physical units (*Verbruik*: kWh, m³) and financial cost (*Euro*: €) across Energy statistics tabs (Elektriciteit, Gas).
    - Added interactive modal dialogs on the left panel to configure electricity rate (€/kWh) and gas rate (€/m³).
    - Persisted `rate_elec_kwh`, `rate_gas_m3`, and `stats_show_euro` in `toonui.cfg`.
    - Extended LVGL Montserrat fonts with Euro (`€`) and superscript 3 (`³`) glyphs across all font sizes.
    - Displayed gas flow in m³/h instead of l/h in *Gas - Nu*.
    - Added hourly delta fallback in *Gas - Nu* for DSMR smart meters reporting NaN in 5-minute `gas_flow`.
    - Fixed bar rounding in *Gas - Dagen* by enforcing higher Y-scale for small values.
    - Defaulted stock Toon HTTP queries in client/slave mode to port 80.
  - **Tile Localization (`tile_metadata.c`)**:
    - Localized Weather integration tile title to Dutch *"Weer"* instead of English *"Weather"*.
  - **Modal Layout & Component Overlap Fixes**:
    - Fixed Home Assistant entity picker list overlapping the top close button (`screen_ha_settings.c`).
    - Fixed button overlap on Z-Wave rename device modal (`screen_zwave.c`).
    - Fixed field row overlap on Domoticz settings modal (Host row overlapping the *"Domoticz inschakelen"* row, `screen_settings.c`).
    - Fixed input field overlap with labels on OT Bridge settings modal (Gebruiker en Wachtwoord fields overlapping label text, `screen_settings.c`).
    - Fixed Agenda settings modal layout and theming on Toon 1 (consistent `SY()` coordinate scaling preventing *"Meldingen"* row from covering ICS buttons, and themed calendar feeds list for light/dark mode, `screen_settings.c`).

### 28. Homey Integration (Implemented)
- **Status**: Done: released in 1.0.17 (2026-11-22) and confirmed working by the requester on his Homey Pro. History: Requester has a Homey Pro (2023) -> local API key route confirmed (2026-11-10). Sample data received 2026-11-21 (devices + flows export, ~130 devices); not stored in the repo because it holds the requester's third-party passwords/API keys in device `settings`. Poller + parser built 2026-11-21 (homey.c, homey_parse.c, synthetic fixtures + `make test-homey`, ASan); real exports only in the git-ignored tests/local/. Control queue + setters built 2026-11-21 (non-blocking, coalesced, optimistic). Settings -> Homey modal built 2026-11-21 (address, masked API key, threaded "Save + test", live status). Web page http://<toon>:10081/homey built 2026-11-21 (behind the web login; key write-only; cross-site POSTs refused; a new address needs the key again so the stored key cannot be redirected). The Toon modal writes only fields edited there and follows web changes live. Devices hub (Apparaten) shows Homey devices by zone (merged with Hue/HA rooms by name) and manual flows, 2026-11-21: on/off (state with dim % or W), covers open/stop/close (up/idle/down, or position 1/0), scene/button + flow Start; locks left out on purpose; hub cap 96 -> 160 rows; names folded to the ASCII fonts (accents -> base letter, emoji dropped). Homey screen (hub chip "Homey", screen_homey.c) 2026-11-22: per zone, dim and position sliders, setpoint -/+ (0.5), sensor values (temp, humidity, power, energy, lux, contact, motion, smoke, water, battery), locks and garage doors read-only (garage doors also left out of the hub). Test against tests/homey_mock.py (synthetic, --bind 0.0.0.0 for a real Toon). Verlichting home tile 2026-11-22: the home button on the Homey screen puts a device on the tile (a third drops the oldest). Tile chooser 2026-11-22: long press on the Verlichting tile opens a list of every switchable light (Domoticz, HA, Homey, Z-Wave) with buttons 1/2; stored as settings tile_light1/2 ("dz:<idx>", "ha:<entity>", "hy:<id>", "zw:<uuid>", "" or "auto" = the old Domoticz T1/T2 + HA Home + Z-Wave rule, fixed on the first change). Domoticz T1/T2, the HA Home flag and the Homey home button write the same two slots (tile_lights.c). Remote page (:10081) Lampen card 2026-11-22: Homey on/off devices (state "hy_devs", max 40, pins first; POST /api/homey/toggle only for listed switchable ids). Locks and garage doors are refused in the setters themselves (homey_read_only), not just hidden in the UI.
- **Findings from the sample (2026-11-21)**:
  - Devices: object keyed by id; per device `name`, `zone` (uuid only: names need `GET manager/zones/zone/`), `class`, `virtualClass`, `capabilities[]`, `capabilitiesObj{cap:{value,units,setable,getable,min,max,decimals,values[]}}`, `available`, `ready`, `unavailableMessage`.
  - Classify by `virtualClass` first: many lights are `class:"socket"` + `virtualClass:"light"` (KaKu, Fibaro, Zigbee strips); also `virtualClass` fan / coffeemachine.
  - Unavailable devices (`available:false`, "App Unavailable" / Tuya "subscription expired") come with `capabilitiesObj:{}` and `settingsObj:null`: show greyed or skip, never crash on missing objects.
  - Controls seen: `onoff`, `dim` (0..1), `light_temperature`/`light_hue`/`light_saturation` (0..1), `windowcoverings_state` enum up/idle/down (Somfy RTS + group devices, no position), `windowcoverings_set` 0..1 (curtain/Tuya blind), `play_scene` and `button` (momentary, `getable:false`), `locked` (Nuki), `target_temperature`.
  - Sensors worth tiles: `measure_temperature`, `measure_humidity`, `measure_power`, `meter_power`, `alarm_contact`, `alarm_motion`, `measure_battery`/`alarm_battery`.
  - 433 MHz devices (KaKu, Somfy) report assumed state only; remotes have `capabilities:[]` (skip).
  - The requester's Homey also controls this Toon via the "Toon Water" app (`nl.toonwater.toonwater`, thermostat + water meter, polls the Toon's local IP; flows set `temperature_state` home/sleep and mirror `target_temperature` to an airco). Hide that device from FreeToon's Homey list (it is the Toon itself), and keep the stock local endpoints it uses working.
  - Flows export holds only standard flows (`manager/flow/flow/`); advanced flows referenced by other flows need `manager/flow/advancedflow/`. Good trigger candidates: flows with `trigger.id == "homey:manager:flow:programmatic_trigger"`. The "Start a flow" ACTION card uses the same id, so advanced flows must check the card type too.
  - Payload is large (full device list several hundred kB): stream/parse into a compact table, keep only id/name/zone/class/caps/values; never keep `settings`.
- **Description**:
  - Add Athom Homey as a smart-home source next to Home Assistant and Domoticz: show and switch Homey devices (lights, plugs, blinds) and read sensors on FreeToon tiles.
- **Decisions (2026-11-10)**:
  - Built into toonui (like Domoticz/HA), not a Marketplace app: Settings -> Homey (IP + API key), devices on the Lights screen, home tile switches.
  - First version: switch lights/plugs (on/off + dim), blinds/covers (open/close/position), sensor values on tiles, trigger Flows.
  - Target the Homey Pro (2023) local Web API with an API key. Older Homey Pro / Homey Bridge need Athom cloud OAuth — only if the requester has one of those.
- **API plan (Homey Pro 2023, local)**: `Authorization: Bearer <API key>` against `http://<homey>/api/`:
  - devices: `GET manager/devices/device/` (class, capabilities, capabilitiesObj values/units, zone); control: `PUT manager/devices/device/<id>/capability/<cap>` `{"value": ...}` (onoff, dim 0..1, windowcoverings_state / windowcoverings_set).
  - flows: `GET manager/flow/flow/` + `manager/flow/advancedflow/`; trigger: `POST .../<id>/trigger`.
  - zones for grouping: `GET manager/zones/zone/`. Poll (~5 s); Homey's realtime API is socket.io, not worth it for v1.
  - API key scopes needed: device read + control, flow start.
  - Build/test against real JSON from the requester first (device list can be large; parser must be robust).
- **Investigation Points**:
  - **API**: Homey Pro exposes a local Web API (`http://<homey>/api/...`, bearer token / API key from the Homey developer tools); Homey Bridge/cloud-only setups need the cloud API. Decide which to support first.
  - **Reuse**: Follow the Domoticz/HA pattern (`domoticz.c`, `homeassistant.c`): poller thread, device list picker, tile binding. TLS-verified fetches (Toon CA bundle is kept current via tsc).
  - **Events**: Check whether Homey can push state changes (websocket / Flow webhook to the PWA server) instead of polling.

---

## Open / Backlog

### 13. Settings Cleanup: Remove Redundant Options from 'Scherm'
- **Status**: Implemented.
- **Description**:
  - Clean up and streamline `Instellingen -> Scherm` (`screen_settings.c`) by removing obsolete/redundant configuration rows:
    1. **Taal (Language selection)**: Remove toggle; streamline default UI language.
    2. **Licht thema: grote waarde = binnentemp**: Hardcode to 1 (`settings.stock_big_indoor = 1`) and remove the row.
    3. **Maanstand op kloktegel**: Obsolete feature on modern clock tile; remove switch and associated row.

### 14. Waste Tile on Dim Screen: Category Name Abbreviations
- **Status**: Implemented (Dim Screen).
- **Description**:
  - On the standby/dim screen waste tile (`DB_WASTE` in `screen_dim.c` / `screen_dim_grid.c` / `wastecollection.c`), shorten long category names for better fit and legibility:
    - *"GFT"* instead of *"Groente/Fruit/Tuinafval"*.
    - *"PMD"* instead of *"Plastic/Metaal/Drankpakken"*.

### 15. Dim Screen Transition Over Detail / Modal Screens
- **Status**: Implemented.
- **Description**:
  - When detail/settings modals (`lv_layer_top()`) are open, the dim screen now cleanly covers them by hiding the top layer while dimmed, and restoring it on wake (or closing it when auto-home expires). Doorbell alerts remain on `lv_layer_sys()` so they always appear over the dim screen with full brightness.

### 16. Dim Screen Waste Tile: Dynamic Category Icon
- **Status**: Implemented (Dim Screen).
- **Description**:
  - On the Dim screen, the next upcoming waste collection date now displays the category-specific waste icon (e.g. leaf for GFT, milk carton for PMD, newspaper for paper, bin for residual) rendered in crisp 80x80 white.

### 17. Thermostat Home Tile: Preset Buttons Should Resume Program Mode from Manual
- **Status**: Implemented.
- **Description**:
  - When the thermostat is in Manual mode and the user taps one of the preset buttons (Comfort, Thuis, Slapen, Weg) on the homescreen tile, the thermostat should activate Program mode (`PROG_TEMPOVERRIDE` or `PROG_BASE`) with that preset, rather than staying in Manual mode.

### 18. Vertical Meter Bars on Dim Screen: Value Text Below Bar
- **Status**: Implemented.
- **Description**:
  - When vertical segmented usage bars (`dim_bars_vertical`) are selected on the dim screen, position the reading / unit text underneath the bar rather than beside it to optimize horizontal space.

### 19. Calendar Tile Layout & Background Consistency Across Tile Slots
- **Status**: Implemented.
- **Description**:
  - If the Agenda/Calendar function is assigned to a different slot (such as the ventilation or family tile), the background color and layout differ from the dedicated Agenda tile. Standardize them so all calendar representations use the preferred native Agenda tile style.

### 20. Temporary Setpoint Override Alignment Across Dim, Tile, and Full Thermostat Screen
- **Status**: Implemented.
- **Description**:
  - When the thermostat is in Program mode and the user adjusts the setpoint temperature on the homescreen tile (+/-), the dim screen switches its label to "Handmatig" while the homescreen tile and big thermostat screen stay in Program mode.
  - Align behavior so that a setpoint change is treated as a temporary override in Program mode (holding the new setpoint until the next scheduled program switch), consistently reflected across all screens.

### 21. Big Thermostat Screen: BoxTalk Debug Banner Cleanup
- **Status**: Implemented.
- **Description**:
  - The big thermostat screen currently displays `"BoxTalk OK msg=xxx"`.
  - Hide this debug status during normal operation, only surfacing an alert if communication fails (or routing connectivity warnings to the Inbox).

### 22. Animation Overlay Engine (Oepi-Loepi QML Animation App Port)
- **Status**: Implemented.
- **Description**:
  - Full native LVGL animation overlay engine (`anim_overlay.c` / `anim_overlay.h`) running on `lv_layer_sys()`, inspired by the rooted Toon Animation app by Oepi-Loepi.
  - Click-transparent overlay where touches pass straight through to whatever screen, modal, or tile is underneath (`LV_OBJ_FLAG_CLICKABLE` cleared).
  - Lightweight, high-performance particle engine with zero heap allocations during the tick loop (24-particle object pool, ~1-2 ms per frame on Toon 1 ARM9).
  - Supported animation effects:
    - **Ballonnen** (`balloons`): Realistic teardrop balloons with tapered neck, flared tied knot, dangling wavy ribbon string, and 3D specular curved gloss highlight (`icon_anim_balloon.h`, restored 54x84 full size), floating upwards with natural sine sway. Particle count balanced by 1/3 reduction (16 in burst mode, 8 in continuous 24h mode).
    - **Confetti** (`confetti`): Festive colored strips fluttering downwards with a 3D tumble flip effect.
    - **Sneeuw** (`snow`): Realistische 6-puntige kristallijne sneeuwvlokken (`icon_anim_snowflake.h`, 64x64) met dendrieten, diamantvormige tips, centrale hexagoon en een langzame dwarrelende rotatie tijdens het vallen.
    - **Vuurwerk** (`fireworks`): Bright particle bursts radiating outward, decelerating with gravity and fading out.
    - **Herfstbladeren** (`leaves`): Realistisch Canadees esdoornblad (sugar maple, Acer saccharum, `icon_anim_leaf.h`, 64x64) met 5 karakteristieke scherpe lobben, diepe U-vormige insnijdingen (sinuses), gekromde bladsteel en delicate handvormige nervenstructuur (palmate venation). Vliegt en dwarrelt met een natuurlijke aerodynamische wiebeling begrensd op maximaal ±35° (strikt binnen ±45°).
    - **Hartjes** (`hearts`): Rijk gevulde 64x64 Valentijnshart asset (`icon_anim_heart.h`, ~55x50px body), 100% frontaal zichtbaar zonder rotatie omklap, met een subtiele zweeftilt begrensd op maximaal ±22° (ruim binnen ±45°), gecentreerd draaipunt (32, 32) en herkleurd in levendige robijn- en rozetinten.
    - **Sinterklaas** (`sint`): Feestelijke ingepakte pakjes met deksel, lint en strik (`icon_anim_present.h`, 64x64) vallend uit de lucht met een zachte slingerbeweging, geïnspireerd op de originele rooted Toon `Present.qml` animatie.
    - **Kerst** (`christmas`): Een sfeervolle combinatie van dwarrelende 6-puntige kristallijne sneeuwvlokken en feestelijke kerstcadeaus.
    - **LVGL 8 Software Transform Renderer Fix**: Opgelost dat LVGL 8 in `lv_draw_sw_transform.c` en `lv_draw_img.c` getransformeerde (geroteerde of ingezoomde) `LV_IMG_CF_ALPHA_8BIT` alpha maskers niet tekende (afgekapt naar 0 pixels door ontbrekende case in de software transform pipeline). Volledige alpha-masker transformatie met recoloring en anti-aliasing direct in het LVGL tekencircuit geïmplementeerd.
  - Multi-source triggers:
    1. **Agenda app (verjaardagen)**: When calendar events refresh in `calendar.c`, any event matching birthday terms (*"jarig"*, *"verjaardag"*, *"birthday"*, *"bday"*, *"gefeliciteerd"*, *"feest"*) automatically launches continuous 24h celebratory balloons throughout the birthday. Animations pause/hide during screen dimming to preserve zero CPU overhead and quiet sleep, resuming immediately upon wake.
    2. **Seizoensauto-triggers**: Built-in date checker for Sinterklaas (5 dec: `sint` cadeautjes), Kerst (24-26 dec: `christmas`), Winter (1-5 jan: `snow`), Oud & Nieuw (31 dec 18:00 - 1 jan 06:00: `fireworks`), en Valentijnsdag (14 feb: `hearts`).
    3. **Remote API & Socket triggers**: Callable via HTTP `POST /api/animation` (of `GET /api/animation?effect=balloons`), en via het `ui_cmd` socket (`anim <effect>`, `anim stop`).
    4. **Instellingen modal**: Nieuwe tegel *"Animaties"* in *Instellingen* met toggles voor verjaardagen, seizoenen, dimstand, en directe handmatige startknoppen voor alle effecten (Ballonnen, Confetti, Sneeuw, Vuurwerk, Herfst, Hartjes, Sint, Kerst, Stop). Handmatig gestarte animaties lopen tot het einde van de dag (automatische middernacht-stop) of tot de 'Stop'-knop wordt ingedrukt.

### 23. Release 1.0.0: Animation Engine, Neerslag & Dim Polish, Thermostat Manual Presets, Stats Bar Tooltips & Afval Venster (Milestone Release)
- **Status**: Completed in Release 1.0.0.
- **Implemented Features & Highlights**:
  - **Animation Overlay Engine (Oepi-Loepi QML Port)**:
    - Native LVGL particle overlay engine running click-transparently on `lv_layer_sys()` with zero heap allocations during the tick loop.
    - Complete suite of animations: 3D realistic Balloons, Confetti, Crystalline 6-point Snowflakes, Fireworks, realistic Sugar Maple Leaves with palmate veins, Valentine Hearts, Sinterklaas Presents, and Christmas Snow/Present blend.
    - Built-in LVGL software transform engine fix for `LV_IMG_CF_ALPHA_8BIT` masks.
    - 24-hour continuous birthday celebrations triggered by calendar events, seasonal auto-triggers, remote socket/HTTP endpoints, and interactive Instellingen modal.
  - **Energy Statistics: Interactive Bar Popup (`screen_stats.c`)**:
    - Tapping any bar in the day, week, month, or year charts opens a compact balloon popup displaying the exact consumption (kWh, m³) or cost (€) value.
  - **Thermostat Preset Buttons in Manual Mode (`screen_home.c`)**:
    - In Manual mode, tapping any of the 4 program preset buttons applies that state's temperature setpoint and marks the button with a highlighted border (without inner background), preserving Manual mode while showing the active preset state.
  - **Neerslag Tile & Grid Polish (`screen_home.c`, `screen_dim_grid.c`, `screen_forecast.c`)**:
    - Matched the dim screen Neerslag tile layout to the homescreen design (droplet icon, styled container, typography).
    - Added subtle vertical division lines for the +1h and +2h marks across homescreen, dim screen, and full forecast charts.
    - On the dim screen, when no rain is expected, the tile header and droplet icon are hidden and *"Geen neerslag verwacht"* is cleanly centered.
  - **Waste Collection Alert Window & Configurable Time (`wastecollection.c`, `screen_settings.c`, `screen_dim.c`, `screen_dim_grid.c`, `pwa_server.c`)**:
    - Added configurable *Afval-waarschuwingstijd* slider in Settings (default 18:00, range 00:00–23:00).
    - Standby dim screen waste icon now appears starting at the configured hour (18:00) $N$ days before collection, and remains visible until 18:00 on collection day.






### 24. Release 1.0.1: Doorbell Timing & go2rtc Integration, Calendar Add & webcal Support, UI-Modus Polish
- **Status**: Completed in Release 1.0.1.
- **Implemented Features & Highlights**:
  - **Deurbel Timing & go2rtc Integratie (`doorbell.c`, `homeassistant.c`)**:
    - Direct scherm openen en ontwaken uit dimstand zodra het deurbelsignaal binnenkomt, zonder te blokkeren op een initieel beschikbaar snapshot.
    - Achtergrond polling om de 800 ms zodat slapende camera's (zoals Eufy) die enkele seconden nodig hebben om te ontwaken naadloos getoond worden zodra de stream beschikbaar is.
    - Automatische herkenning en conversie van go2rtc `stream.html?src=...` naar de rechtstreekse `/api/frame.jpeg?src=...` snapshot-endpoint.
    - Dynamische canvas-creatie zodra het eerste frame binnenkomt, ter vervanging van de statische foutmelding.
  - **Agenda Toevoegknop & webcal:// Ondersteuning (`screen_settings.c`, `calendar.c`)**:
    - Duidelijke statusfeedback in het Agenda-instellingenvenster bij ontbrekende invoer.
    - Ondersteuning voor `webcal://` URLs en domeinen zonder http/https-prefix (automatische rewrite naar `https://`).
    - Vergroot touch-oppervlak (`ext_click_area`) op Toon 1 voor betrouwbare registratie op resistieve schermen.
    - Toetsenbord "OK" (Enter) koppeling: direct toevoegen van de agenda bij afronden via het schermtoetsenbord.
    - Automatisch inschakelen van de Agenda bij toevoegen van een feed.
  - **UI-Modus Venster Polish (`screen_settings.c`)**:
    - Tekst aangepast naar "... Eneco-UI neemt het daarna over."
    - Actieknoppen ("Annuleren" en "Naar qt-gui") worden direct verborgen na het indrukken om duidelijke visuele feedback te geven dat de overgang naar qt-gui gestart is.
  - **UI Herstarten & Toon Reboot Keuze (`screen_settings.c`, `screens.h`, `ui_stack.c`)**:
    - Dialoog uitgebreid met een tweede optie om het Toon-apparaat volledig te herstarten (`sync` + `/sbin/reboot`).
    - Drie knoppen: Annuleren, UI herstarten en Toon herstarten.
    - Direct verbergen van de knoppen en tonen van statusmelding met geforceerde render (`lv_refr_now`) bij indrukken.
    - Vergroot touch target op Toon 1 en tegel-subtitel aangepast naar "UI of Toon herstarten".

### 25. Release 1.0.2: Device Reboot Optie & Toon 1 Opstart-vervorming Fix
- **Status**: Completed in Release 1.0.2.
- **Implemented Features & Bugfixes**:
  - **Volledige Toon Herstart (Device Reboot)**:
    - Dialoog *Instellingen → UI herstarten* uitgebreid met een optie om het Toon-apparaat volledig te herstarten (`sync` + `/sbin/reboot`).
    - Drie knoppen: *Annuleren*, *UI herstarten* en *Toon herstarten*.
    - Direct verbergen van de knoppen en tonen van statusmelding met geforceerde render (`lv_refr_now`) bij indrukken.
    - Vergroot touch target op Toon 1 en tegel-subtitel aangepast naar *"UI of Toon herstarten"*.
  - **Toon 1 Boot-vervorming & Koude vs. Warme Herstart Fix (`fbdev.c`, `bootpick.c`, `ui_launcher.sh`)**:
    - **Oorzaak**: Koude boot op Toon 1 duurt circa 140–180 seconden. Daardoor overschreed de uptime de `BP_WARM_RESTART_S` drempel (120 s) en toonde `bootpick` direct het *"UI herstart"* scherm. Daarnaast faalde `ioctl(FBIOPUT_VSCREENINFO)` door ontbrekende RGB565 bitfields, waardoor de LCD-controller in 32bpp bleef staan en de 1600-byte stride door 4 gedeeld werd (400 px), wat resulteerde in een dubbele horizontale weergave en een zichtbare onderste helft van de 32bpp bootsplash.
    - **`fbdev.c`**: Correcte RGB565 bitfields (R 5/11, G 6/5, B 5/0, transp 0/0) en `FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE` toegevoegd. Framebuffer wordt bij start direct met `memset(0)` geschoond. Forceren van 32bpp in `fbdev_exit()` verwijderd. Stride in `fbdev_flush()` geclamped op minimaal `vinfo.xres`.
    - **`bootpick.c`**: `BP_WARM_RESTART_S` verhoogd naar 300.0 s op Toon 1 (`#ifdef TOON1 300.0 #else 120.0 #endif`). Warme herstart wordt nu exclusief herkend via `TOONUI_WARM_RESTART` of de restart marker file.
    - **`ui_launcher.sh`**: Volledige `PATH` export toegevoegd bovenaan het script, `fbset` uitgebreid met expliciete geometrie (`-g 800 480 800 480 16`), en `TOONUI_WARM_RESTART=1` export bij bewuste UI herstart.

### 26. Release 1.0.3: Toon 1 Inittab Beveiliging, Oude BusyBox Compatibiliteit & Aandelen Marktplaats App
- **Status**: Completed in Release 1.0.3.
- **Implemented Features & Bugfixes**:
  - **Inittab Beveiliging & Zero-Day Boot Hang Fix**:
    - **Oorzaak**: Op zeer oude Toon 1 apparaten (BusyBox < 1.27) faalden `sed -i -E` commando's met een syntax error. Door het ontbreken van inhoudsvalidatie bij tijdelijke bestanden (`> inittab.new && mv inittab.new inittab`) kon `/etc/inittab` leeg raken, waardoor init tijdens het opstarten geen enkel proces startte en de Toon bleef hangen op de kernel splash.
    - **POSIX Regex**: Alle scripts (`install.sh`, `scripts/switch_to_freetoon.sh`, `scripts/ot_mode_switch.sh`, `scripts/ui_launcher.sh`, `scripts/integrations-install.sh`, `toonvnc.sh`, `screen_settings.c`) herschreven naar pure POSIX basic regular expressions zonder `-E`.
    - **Validerende Inittab Backup**: Ieder script maakt nu automatisch een backup (`/etc/inittab.bak`) en verifieert vóór overschrijving dat het nieuwe bestand niet-leeg is en minimaal 5 regels telt (`[ -s /etc/inittab.new ] && [ $(wc -l < /etc/inittab.new) -ge 5 ]`).
    - **Runlevel 2345**: Runlevels voor alle UI en VNC regels uniform ingesteld op `2345` (oudere Toon 1 firmwares starten standaard in runlevel `2`).
    - **Oude Stock Qt Fallback**: `ui_launcher.sh` ondersteunt nu ook oudere Qt-installatiepaden (`/qmf/bin/startqt`, `/qmf/sbin/startqt`, `/qmf/bin/qt-gui` met `-platform linuxfb -plugin Tslib`).
  - **Aandelen Marktplaats App & Dynamische Tegel Titels**:
    - Aandelen / AEX koers app toegevoegd aan de ingebouwde marktplaatscatalogus (`stocks` met fallback manifest).
    - Ondersteuning voor dynamische tegeltitels (`title` property) in de UI zodat tegels de actuele naam van het aandeel of de feed kunnen tonen.

### 27. Release 1.0.5: Toon 1 Stock UI Splash Herstel, Bulletproof Inittab Engine, Philips Hue Integratie & UI Verbeteringen
- **Status**: Completed in Release 1.0.5.
- **Implemented Features & Bugfixes**:
  - **Bulletproof Inittab & Flash Filesystem Beveiliging (`scripts/inittab_edit.sh`)**:
    - **Oorzaak & Risico's**: Analyse wees uit dat schrijfoperaties naar `/etc/inittab` kwetsbaar waren voor afkapping bij schijf-vol (BusyBox `grep -v > new && mv` gaf rc 0 ondanks lege buffer), race-conditions door gelijktijdig schrijven naar dezelfde `.new` file, ongevalideerde writes (`sed -i` en directe `>>`), en zero-byte risico op UBIFS door het ontbreken van expliciete `sync` voor en na `mv`. Bovendien konden 4-letterige integratie-IDs (`${id:0:4}`) botsen met essentiële stock Toon daemons (zoals `ther` voor `happ_thermstat` of `toon` voor de UI).
    - **Centrale `inittab_edit.sh` Helper**: Alle inittab operaties in het hele ecosysteem (`install.sh`, `switch_to_freetoon.sh`, `integrations-install.sh`, `ot_mode_switch.sh`, `ui_launcher.sh`, `toonvnc.sh`, `screen_settings.c`, `toon_tailscale.sh`) lopen nu uitsluitend via een zwaarbeveiligde atomic helper.
    - **Atomic Locking & Space Pre-flight**: Beveiligd met `mkdir /etc/inittab.lock` (spin-lock met time-out en automatische opruiming van stale locks). Schrijfoperaties weigeren direct als er minder dan 100 KB vrij is op `/etc`.
    - **PID-geïsoleerde Temp Files**: Iedere operatie gebruikt een unieke `/etc/inittab.tmp.$$` file.
    - **Strikte Multi-point Validatie**: Nieuwe configuratie wordt pas geaccepteerd als het bestand niet-leeg is, minimaal 15 regels telt, alle vitale sysvinit-ankers bevat (`:initdefault:`, `:sysinit:`, `rcS`, `/etc/init.d/rc`) én binnen een veilige line-delta valt ten opzichte van het origineel.
    - **Permanente Fabrieksbackup & Fallback**: Eenmalige aanmaak van `/etc/inittab.orig` die **nooit** wordt overschreven. Bij validatiefout herstelt de helper direct automatisch de backup.
    - **UBIFS Flash Veiligheid**: Verplichte `sync` vóór `mv` en direct na `mv` om write buffers op UBIFS/JFFS2 naar flash te forceren vóórdat init een SIGHUP ontvangt.
    - **Gegarandeerde ID Isolatie**: Alle integratie-IDs worden voortaan geforceerd voorafgegaan door een `i`-prefix (`i<id:0:3>`, bijv. `itel`, `isto`, `ison`, `ithe`), waardoor botsing met de 4-letterige stock Toon daemons wiskundig uitgesloten is.
    - **Veilig Downloadpad**: `DL_DIR` in `install.sh` verplaatst van `/tmp` naar `/mnt/data/.freetoon_dl.$$` om defecte symlinks of ramfs/tmpfs-tekorten op Toon 1 te omzeilen.
  - **Toon 1 Stock UI Switch & Opstartsplash Herstel (`scripts/ui_launcher.sh`)**:
    - **Oorzaak**: Op Toon 1 start `psplash` tijdens vroege boot (`rcS`). Wanneer `bootpick` opent, tekent het over de framebuffer. Bij het kiezen van Stock UI (`qt-gui`) controleerde `startqt` met `pidof psplash` of psplash al draaide; omdat het oude proces nog aanwezig was, werd psplash niet herstart en bleef de framebuffer circa 2-3 minuten bevroren op het keuzemenu totdat Qt klaar was met laden.
    - **Fix**: `ui_launcher.sh` herstart `psplash` nu direct vers bij de selectie van `qt-gui` zodat de officiële Toon laadbalk direct getoond wordt. Bij het opstarten van FreeToon wordt `psplash` juist netjes afgesloten om framebuffer- en geheugengebruik te voorkomen.
  - **Philips Hue Bridge Ondersteuning (`hue.c`, `hue.h`, `screen_hue.c`)**:
    - Automatische detectie en migratie van gekoppelde Philips Hue bridges via bestaande Toon instellingen.
    - Directe bediening van lampen, groepen en scènes met native UI-schermen en helderheidssliders.
  - **Home Assistant Entity Picker & Instellingen Modal Fixes (`screen_ha_picker.c`, `screen_settings.c`)**:
    - Top-layer (`lv_layer_top`) wordt nu correct verborgen tijdens het openen van de HA entity picker en hersteld bij terugkeren, zodat zoekresultaten en lijsten niet overlapt worden.
    - Geheugenbeheer van `ha_browse_ctx`: context wordt pas vrijgegeven bij vernietiging van de knop (`on_browse_delete`) i.p.v. direct bij de eerste klik, wat crash-resistentie biedt bij herhaaldelijk klikken.
  - **Quby Bridge & Afvalwijzer Optimalisaties**:
    - Logging reductie en automatische rotatie voor `quby-bridge` om tmpfs opslagruimte te besparen.
    - Standaard afvalwijzer provider ingesteld op *Overige gemeentes* voor verbeterde directe werking.

### 29. P1 Meter Readings (Meterstanden) Like the Stock QML UI
- **Status**: Implemented (495d80a, 2026-11-10).
- **Description**:
  - The stock (QML) firmware shows the actual P1 meter readings (cumulative kWh import/export per tariff, gas m3). Show these in FreeToon too, e.g. on the Energy statistics screen.
  - Stock UI: a "Meterstanden" button top-right on the "Grafiek" screen (Energy statistics) opens a pop-up with the three absolute P1 meter readings. Mirror that.
  - Note: hcb_rrd already serves the cumulative loggers (`elec_quantity_nt`, `elec_quantity_lt`, `gas_quantity`; Wh / L) — the latest sample of each is the current reading.
- **Investigation Points**:
  - Source: which BoxTalk/hcb service the stock UI reads them from (meteradapter / `hdrv_p1` dataset), or the HomeWizard/ESPHome P1 integrations when those are the energy source.

### 30. Energy Statistics Show Data for the Next Day (User Report)
- **Status**: Fixed (4566336, 2026-11-10).
- **Cause**: hcb_rrd stamps a bucket with its END time (the daily archive already returns "11-11 01:00" with today's running total), and each usage delta took the later reading's label. Deltas are now labelled with the interval start.
- **Description**:
  - A user sees energy statistics attributed to the following day.
- **Investigation Points**:
  - Likely a day-boundary / time zone issue: check how `stats.c` / the rrd queries bucket samples per day (UTC vs local, rrd bucket end timestamps that lie at the next midnight). Note the Toon 1 clock bug fixed in dc2b27f (toonui ran in Europe/London) — check whether the reporter is on a Toon 1 without that fix.

### 31. Close-Button (X) Touch Area Smaller on Toon 1
- **Status**: Fixed (8528245, 2026-11-10), confirmed on a Toon 1.
- **Cause**: Toon 1 touch ignored the factory tslib calibration (/etc/pointercal); the linear mapping was ~27 px off at the top and ~18 px at the right edge. toon1_touch now applies the calibration.
- **Description**:
  - On Toon 2 the whole X button responds; on Toon 1 only the upper part seems to.
- **Investigation Points**:
  - Toon 1 touch goes through `toon1_touch.c` (own calibration/scaling, 800x480); check the Y mapping near the top edge. Also check the buttons: sizes are hard-coded per screen (`x_w`/`x_h` with `DISP_VER < 600`) while positions use SY(); compare `lv_obj_set_ext_click_area` per screen.

### 32. Installer Takes Scripts From the Release Instead of `main`
- **Status**: Done (2026-11-19). install.sh takes helper scripts from the release tag (main only as a logged fallback) and installs exactly the release in FREETOON_TAG when given. toonui (update button, nightly auto-update) fetches install.sh from the tag it found for its own channel and passes FREETOON_TAG, so a push to main no longer reaches updating Toons and a stable-channel Toon never gets a beta prerelease (install.sh used to pick the newest release incl. prereleases). Toons on <= 1.0.14 still run main's install.sh without a tag: newest release, but with matching scripts. Not device-tested yet.
- **Description**:
  - `install.sh` downloads the binary from the latest release but the helper scripts (ui_launcher.sh, inittab_edit.sh, integrations-install.sh, ...) from the `main` branch, so every push to `main` is live for every updating Toon and the scripts can mismatch the binary.
- **Approach**:
  - Ship the scripts as release assets (or a scripts tarball) and fetch them from the same release tag as the binary; keep `main` only as fallback for old releases. Pairs naturally with parked review step 4 (signed SHA256SUMS per release).

### 33. Release 1.0.6: Security & Stability Review, Toon 1 Touch/Clock, Meterstanden
- **Status**: Released (tag 1.0.6, 2026-11-10).
- **Contents**:
  - Security: no takeover of the web login via /set-password; all settings/remote values shell-quoted (no root command injection); marketplace catalog/plugins verified; plugin ABI gate (Sonos 1.0.8 / Stocks 1.0.5 / Teletekst 1.0.6).
  - Installer: verified downloads (no truncated binaries/scripts), synced copies, inittab order that always keeps a UI row, uninstall releases the boiler UART and stops FreeToon before stock qt-gui; Toon 1 stock UI back at 32bpp.
  - Stability: timer use-after-free fixes (stats tooltip, family map, package banner), per-open screens freed (no memory growth), OTGW advanced screen off the UI thread, thread-safe calendar/HA data, crash-safe settings and schedule saves.
  - Toon 1: clock no longer 1 h behind (TZ), factory touch calibration (whole X button responds), no pkill dependency.
  - Features/fixes: "Meterstanden" pop-up, energy usage on the right day, boiler flow/return correct (incl. HA MQTT), calendar retries a timed-out feed, pinned HA tiles follow their entity, forecast radar refreshes.

### 34. Release 1.0.7: Telegram Bot, Hue Rooms & Zones, Lighter Toon 1, Weather Location Fixes
- **Status**: Released (tag 1.0.7 on 9012fb7, 2026-11-11).
- **Contents**:
  - Built-in Telegram bot (ToonBot replacement, approved chats, optional alerts); Philips Hue rooms & zones and a Hue home tile.
  - Performance: idle home/dim screens no longer redraw the whole screen (Toon 1 toonui ~32% -> ~14% CPU on home, ~18% -> ~4% on dim); no button drop shadows on Toon 1 (Settings scroll frame ~45% cheaper).
  - Weather: Dutch place names preferred over namesakes abroad (Ede/Epe resolved to Nigeria); saving Weer settings applies the location at once (was: only after a UI restart); a place that can't be found keeps the entered Buienradar-id and resolves the coordinates from it.
  - Fixes: Freetoon default UI on Toon 1 after install, waste icon flicker on dim.
  - Marketplace: Sonos Player 1.0.9 (TuneIn favourites start again).

### 35. Daily Check for Marketplace App Updates
- **Status**: Implemented (2026-11-12): app_updates.c checks the catalog once a day alongside the FreeToon check (and on "Controleer"). The home banner shows "N app-updates" when there's no FreeToon update (tap = Marketplace; stays hidden until the set of updates changes), the Settings Marketplace tile shows the count, and the Update modal has an opt-in "Apps ook automatisch" at the auto-update hour (UI restart afterwards). Optional catalog field `min_toonui` gates updates/installs for the plugin ABI. The Marketplace screen has an "Alles updaten (N)" button (one UI restart at the end).
- **Description**:
  - FreeToon checks for its own updates (update_check.c: GitHub releases every 6 h, update banner, optional nightly auto-update), but marketplace apps are only compared with the catalog when the Marketplace screen is opened. A user never hears that e.g. Sonos 1.0.9 is out unless they go looking.
- **Approach**:
  - Once a day (alongside the FreeToon check), fetch the catalog (`catalog/index.json`, with the `?t=` cache-buster) and compare each installed integration's manifest version with the catalog version.
  - Show it the same way as FreeToon updates (banner / badge on the Marketplace tile, "N app updates"); optionally an "update all" and an opt-in automatic update at the nightly auto-update hour.
  - Reuse the existing compare/sort logic of the Marketplace screen; respect the plugin ABI gate (an update that needs a newer toonui must not be offered).

### 36. Settings That Only Take Effect After a Restart
- **Status**: Implemented (2026-11-12): audit, then fixes. Builds and ABI checks pass on all targets; not yet tested on a Toon.
  - Waste: the fetcher starts as soon as an address/ICS source or dim-waste is set (`waste_ensure_started()`). It used to start only with a postcode at boot, so ICS-id/street/ICS-URL-only setups never fetched.
  - Home Assistant: Save reconnects the running client (it used to start an extra WebSocket thread on every Save). It runs after the fields are stored, so a newly entered doorbell starts its worker. HA MQTT discovery starts too.
  - Calendar: switching it on or adding a source starts the periodic thread (30-min refresh + pre-event notifications), not just a one-shot fetch.
  - OTGW: Apply starts the telemetry poller. Domoticz: the enable switch starts it, and "Save + connect" no longer adds a poller each time.
  - MQTT: Apply reconnects the Itho status subscriber and the HA discovery push with the new `mqtt.cfg`. Both now wait for the file instead of giving up when it was missing at boot. Switching MQTT off drops the session at once.
  - Video (Toon 1): the switch starts/stops the warm pipeline and frees the VPU.
  - Auto-update switches turn update checks back on when they were off. Hue: a quick off/on no longer leaves two pollers running.
  - Web settings API: applies waste/news/HomeWizard/OTGW/Domoticz/HA/MQTT changes live (MQTT only when it actually changed). It now reads `mqtt_pass` and `auto_home_*` (they were sent but ignored), and a changed Buienradar id relocates the weather.
  - Still restart-only (with the hint): Itho ventilation on/off, HA on/off, energy source, ESPHome P1; client mode (prompts). Not done: `home_theme` and `ot_bridge_mode` from the web API (theme needs the LVGL thread; the OT mode switch must run the script).
- **Description**:
  - The weather location was resolved only at start-up, so saving Settings -> Weer did nothing until a UI restart (fixed in 97a42e2). Audit the other settings screens for the same pattern: a value read once at start-up or cached in a thread, while the screen suggests it is applied immediately.
- **Approach**:
  - Grep for settings fields read only in thread start-up / init code; per hit either apply live (like `weather_relocate_async()`) or show the existing "restart needed" hint.

### 37. HomeWizard: IP Fields on the Toon, Energy Sockets & kWh Meters, v2 API
- **Status**: Implemented (2026-11-12), sim-tested against fake devices; not yet on a real HomeWizard device.
  - Settings > Integraties: IP fields for the P1 and the water meter (filling one in switches it on), a kWh meter (solar) field and 4 Energy Sockets (name + IP), plus a "Lokale API" hint. All apply without a restart (`homewizard_ensure_started()`; the poller reads hosts live).
  - kWh meter: `abs(active_power_w)` feeds the Solar tile (`energy_solar_w()`), ahead of the HA solar sensor.
  - Energy Sockets: a "Stekkers (HomeWizard)" section on the Devices screen (the HA lights page, also shown when HA is off) with on/off (PUT `/api/v1/state`) and watts. Every device is polled once per ~10 s, on separate ticks.
  - v2 API: not needed. HomeWizard's docs (2026-11) list v1 for P1, Socket, water meter and kWh meters; v2 is "planned" for the Socket and water meter, and no end date for v1 is given. v2 only matters for v2-only devices (Plug-In Battery HWE-BAT).
  - Follow-up (2026-11-12): the hw_* fields are in the web settings API (GET + POST); Hue and Domoticz screens get a "Stekkers" button to the Devices screen; a client Toon mirrors the master's sockets (state "hws") and switches them via POST /api/hw/socket. Entering a P1 IP offers to switch Energiebron to "HomeWizard P1".
- **Description**:
  - A user reports that the HomeWizard water meter, socket plugs, P1 meter and kWh meters don't work. P1 (HWE-P1) and water meter (HWE-WTR) are supported, but their IP addresses (`p1_elec_host` / `p1_water_host`) can only be set in `/mnt/data/toonui.cfg` over SSH: there is no field on the Toon. Energy Sockets (HWE-SKT) and kWh meters (HWE-KWH1/3, SDM230/630) are not supported at all.
  - Workaround until then: "Lokale API" on per meter in the HomeWizard Energy app + a fixed IP in the router; the two hosts in toonui.cfg; Settings > Integraties: energy source "HomeWizard P1", switch on HomeWizard P1 and Watermeter, restart the UI.
- **Approach**:
  - Settings > Integraties: IP fields for the P1 and the water meter, like the ToonWater field (commit-on-defocus, host characters only), plus a short hint about "Lokale API".
  - Energy Sockets: power on a tile, on/off via the local API (`/api/v1/state`, PUT `{"power_on":…}`); kWh meters: power like the P1. Mind the ESP socket-table limit (poll slowly, see hw_thread).
  - HomeWizard v2 API: newer firmware has a local API v2 (HTTPS + bearer token, obtained by pressing the device button; README already mentions a v2 `P1_TOKEN` for p1bridge). Check whether v1 keeps working on current firmware or whether v2 should become the default, and how a user pairs on the Toon.

### 38. Meterstanden Pop-up Shows Readings That Match Neither the Old Nor the New Meter (User Report)
- **Status**: Resolved (2026-11-13). Cause of confusion: the readings were actually correct (17 kWh, 14 kWh, 13 m3 on the new meter), but were formatted with three decimal places (e.g. `17,000` / `14,000`), which looked like seventeen thousand / fourteen thousand. `fmt_reading()` now displays whole kWh and m3 with thousands-grouping and no decimals (e.g. `17 kWh`, `47.123 kWh`), matching the stock UI and physical meters.
- **Investigation on a Toon with a meter adapter (2026-11-13)**:
  - The datasets arrive at `<hostname>:happ_pwrusage`, also when subscribed in the same burst as ssdp:connect, in exactly the format 1.0.8 parses. On that Toon the stock pop-up, the dataset and the hcb_rrd totals all show the same values.
  - The update only restarts the UI (happ_pwrusage keeps running), so an updated, not-rebooted 1.0.8 should show the stock values. Not explained yet; open questions for the reporter: version, whether the note "Geteld door de Toon" shows, energy source, and the toonui.log lines `[bxt-meter]` / `[meter] Meterstanden source`.
  - After a reboot, though, toonui starts before hcb_comm/happ_pwrusage (inittab order), and a subscription sent before happ_pwrusage is up is lost; the same after a happ_pwrusage restart. Fixed (next release): subscribe on happ_pwrusage's ssdp:alive, as the stock BxtDatasetHandler does. Added logging of the first datasets and of the pop-up's source; rrd fallback asks for `elec_quantity_*_produ` (was `_prod`).
  - Stock source (qresource `apps/graph/GraphApp.qml` + `GraphScreen.qml`): BoxTalk dataset subscriptions `powerUsage` / `gasUsage` on `happ_pwrusage`, fields `meterReading` (normal) / `meterReadingLow` / `meterReadingProdu` / `meterReadingLowProdu` in Wh, gas `meterReading` in litres.
  - FreeToon now subscribes to the same datasets (boxtalk.c, `meter_state.reading_*`). The pop-up shows those registers, or with HomeWizard / ESPHome as energy source that device's P1 totals (DSMR: t1 = low, t2 = normal). The hcb_rrd loggers are only a fallback, with a note "Geteld door de Toon - kan afwijken van de meter".
  - Side finding, not changed: homewizard.c `push_to_rrd()` writes `kwh_import_t1` (NL low tariff) into `elec_quantity_nt` (normal) and t2 into `_lt`, so the HomeWizard normal/low split in the charts is probably swapped. Fixing it would mix up existing history; decide separately.
- **Description**:
  - A user's "Meterstanden" pop-up (#29) shows ~17,000 and ~14,000 kWh for electricity and ~13,000 m3 for gas. Their meter was replaced a week ago; the old meter read ~47,000 / ~40,000 kWh and ~41,000 m3, and the new meter is still close to zero. So the pop-up shows neither.
- **Cause (hypothesis)**:
  - `on_meter_open()` in `screen_stats.c` shows the latest sample of the hcb_rrd cumulative loggers (`elec_quantity_nt` / `elec_quantity_lt` / `gas_quantity`). These are hcb_rrd's own running totals, not the meter's register. They probably started counting when the Toon (or the logger) was set up, so they never matched the meter. After a meter swap they keep going instead of dropping to the new meter's value.
  - Supported by a second report (2026-11-12): on the stock QML UI the Meterstanden are correct. So the right values exist on the Toon and FreeToon reads the wrong source.
- **Investigation Points**:
  - Where the stock QML UI gets its Meterstanden: probably the live P1 dataset from meteradapter / `hdrv_p1` (the raw DSMR registers 1.8.1 / 1.8.2 / gas). If so, read that instead of the rrd loggers.
  - Compare on a test Toon: rrd latest sample vs the P1 telegram / meteradapter values vs the physical meter.
  - With HomeWizard / ESPHome P1 as the energy source, use that device's `total_power_import_t1/t2_kwh` / `total_gas_m3` (actual meter registers).
  - If no meter-register source is available, hide the pop-up or label the values as "counted by Toon" rather than showing them as meter readings.

### 39. Suppress the TSC "Resource File Update" Inbox Message
- **Status**: Fixed (2026-11-12): inbox.c drops notifications whose text mentions "resource file" / "TSC menu" (sent by /usr/bin/tsc for the stock TSC menu).
- **Description**:
  - The inbox shows "Er is een TSC resource file update beschikbaar. Update via TSC menu aub". The TSC script sends it for the stock QML UI's TSC menu, which FreeToon doesn't have, so the user can't act on it.
- **Approach**:
  - `inbox.c` already drops the "Service Center" notifications in `inbox_parse_dataset()` (CR2 filter). Add the TSC resource-file message there, matching on its type/subType if it has a stable one, otherwise on "TSC resource file". Also consider sending a DeleteNotification so it doesn't stay in happ_usermsg.

### 40. Empty Inbox Message "telegram / imported" on Both Toons
- **Status**: Fixed (2026-11-12), sim-tested. Cause confirmed in the stock source: `qb/notifications/Notifications.qml` sends CreateNotification with the argument `text`; FreeToon's `notify_show()` sent `plainText`, so every FreeToon notification (telegram, update, calendar, system, ...) arrived without a body. notify.c now sends `<text>`. The inbox falls back to `plainText`, and hides body-less entries of FreeToon's own types; stock types without text are kept.
- **Description**:
  - Both Toons show an inbox message with the header "telegram / imported" and no text. It comes from `telegram.c` (ToonBot settings import, `notify_show("telegram", "imported", ...)`).
- **Cause (hypothesis)**:
  - `notify_show()` (notify.c) sends the message as `<plainText>`, but `inbox_parse_dataset()` reads `<text>`. happ_usermsg probably fills `<text>` only for types it knows, so our own notifications arrive without a body. Check the other `notify_show` users too ("update"/"freetoon", "telegram"/"pending").
- **Approach**:
  - Dump the happ_usermsg dataset for this notification to see which field carries the text; fall back to `plainText` in the inbox parser when `text` is empty.
  - Decide whether a one-time "settings imported" note should go to the inbox at all, or only be a banner/log line.

### 41. Release 1.0.8: HomeWizard Sockets & kWh Meter, Remote Web Page, App Updates, Meter Readings, Live Settings
- **Status**: Released (tag 1.0.8).
- **Contents**:
  - HomeWizard: IP fields on the Toon, Energy Sockets (Devices screen, Hue/Domoticz "Stekkers" button, client mirror), kWh meter as solar source, offer to use the P1 as energy source (#37).
  - Remote-control web page on :10081; client mode signs in on a master with a web password; ToonWater sensor as water meter.
  - Daily marketplace app-update check with home banner, "Alles updaten" and optional nightly app updates (#35).
  - Weather from Buienradar only; radar buttons off the timestamp.
  - Settings that apply without a restart (#36); Meterstanden from the meter's registers (#38); FreeToon notifications with text, TSC notice hidden (#39, #40).

### 42. Release 1.0.9: HA Lights on Tile and Web Page, Gas Today, Water Statistics, Meter Readings After Restart
- **Status**: Released (tag 1.0.9).
- **Contents**:
  - Home Assistant lights/switches on the Verlichting tile (T1/T2 in Apparaten beheren) and switchable on the :10081 web page.
  - "Gas vandaag" on the Energie tile; gas today since 00:00 for HomeWizard/ESPHome P1 and Home Assistant.
  - Water statistics from hcb_rrd's 10yrhours archive (was 0.01 m3 instead of 1.84 m3).
  - HomeWizard P1 gas from the "external" list; Meterstanden subscribe on happ_pwrusage's ssdp:alive (#38).
  - Apparaten beheren in front of the HA modal; tracker picker lists person entities; waste tile on a low tile; Toon 1 bold dim font.

### 43. Energy Use per HomeWizard Socket (History in Statistics)
- **Status**: Done (2026-11-15): covered by #48 (HomeWizard socket loggers in Stekkers), confirmed working by the user.
- **Description**:
  - A user wants to see the energy use per HomeWizard Energy Socket. Today the Devices screen ("Stekkers (HomeWizard)") shows only the live watts; no history is kept.
- **Options**:
  - Small: kWh today + total per socket on the Devices screen, from the socket's `total_power_import_kwh` (today = now minus the 00:00 value, as for gas).
  - Larger: a Stekkers tab in Statistics. hcb_rrd has no API to create a logger (only get/set data, ChangeRrdProperties, RemoveRrd, SetRrdDeviceUuid, RevertToFactoryDefaults), but loggers are `<rrdLogger>` blocks in `/qmf/config/config_hcb_rrd.xml`; adding blocks + restarting hcb_rrd creates the .rra files, after which `setRrdData` works as for the P1/water. Risks: a malformed file stops hcb_rrd (all graphs); Eneco config updates / factory reset can drop the extra loggers. Needs backup, atomic replace, check after restart with automatic rollback. First step: one test logger on the test Toon 1 (.52), only with the user's go-ahead. Alternative: FreeToon keeps its own history file (as airhist does).

### 44. Dynamic Energy Tariffs for Costs in Statistics
- **Status**: Done: released in 1.0.14 (dynamic contract per commodity in 1.0.15), tested on a device by the user (2026-11-19).
- **Implementation**: Statistics rate modal has a "Dynamisch contract (uurprijzen)" switch; with it on, the number entered is the surcharge per kWh / m3 incl. VAT (mark-up + energy tax; defaults 0.15 / 0.75) and all-in = EnergyZero market price x 1.21 + surcharge. Euro view: hourly bars use each hour's price; day/week/month/year bars use per-UTC-day usage-weighted prices (hourly `5yrhours` deltas x hourly price) computed by a worker thread and cached in `toonui_price_days.dat` (finished days only); hourly prices (~6 months) in `toonui_prices.dat`. Solar valued at the all-in price (net metering), smart plugs at the plain day average. "Energieprijzen" screen (from Statistics): today + tomorrow hourly bars, cheap/expensive per day by quartile, current hour outlined, cheapest 2-hour block. Energie tile: current all-in price bottom-right (green/orange vs today's quartiles, plus "laag HHu" on wide tiles). Prices refresh every 30 min (tomorrow after ~13:00); while prices load the fixed rate is used and the caption says so. No new libc imports (ABI baselines unchanged).
- **Description**:
  - The Euro view in Statistics uses one fixed price per kWh and per m3 (Settings: Stroom / Gas tariff). With a dynamic contract the price changes every hour (electricity) or every day (gas), so the costs shown are not real.
- **Approach**:
  - Fetch hourly day-ahead prices (EPEX NL) from a free source (e.g. EnergyZero or Frank Energie public API: all-in or market price), plus the user's own surcharge (energy tax, supplier mark-up, VAT) as settings.
  - Keep the prices per hour for as far back as the statistics go (own file on /mnt/data, or hcb_rrd logger as in #43), and compute costs per hour bar as usage x that hour's price; days/weeks/months as the sum of their hours.
  - Gas: daily price (gas day 06:00-06:00) the same way.
  - Show the current price (and cheapest hours ahead) on the Energie tile or dim screen as an option; fixed tariffs remain the default.
  - Feed-in (solar export) at the same hourly price, minus the supplier's feed-in fee.

### 45. Room-Temperature Correction Not Shared With the Stock GUI and Not Used by the Boiler (User Report)
- **Status**: Resolved (2026-11-14), tested on the test Toon 1 (.52).
- **Description**:
  - FreeToon had its own correction (`temp_offset_centi` in toonui.cfg, slider under Heating). It was only added to the displayed temperature: the boiler, the web page and the stock GUI ignored it. On top of that, happ_thermstat already applies the stock GUI's correction, so with both set the screen was corrected twice (production: qt-gui -1.0 plus FreeToon -2.0 showed 19.7 while the room was 21.6).
  - The slider worked in 0.01 C steps, so it rarely landed on exactly 0 ("-0.0" / "+0.0" still shifted the value 0.1).
- **Findings**:
  - The correction lives in happ_thermstat (config key `i2cTempError` in config_happ_thermstat.xml). It is added to every sensor reading, so `currentTemp` (HTTP, thermostatInfo dataset, ThermostatInfo notify) is already corrected and the thermostat regulates on it.
  - Stock qt-gui (`apps/thermostatSettings/ThermostatSettingsApp.qml`, `TemperatureCorrectionScreen.qml`): BoxTalk service `Thermostat` on happ_thermstat, `GetTempOffset` -> `<offset>-1.0</offset><measuredTemp>2265</measuredTemp>` (offset in C, measuredTemp raw in centi-C), `AdjustTempOffset(<offset>)` sets the absolute offset (no response). Range +-6.0 in 0.1 steps. There is no `SetTempOffset` (toonsim's model is wrong).
  - Binary detail: Get reports `i2cTempError - E`, Adjust stores `new - E`, where E is the factory `i2cTempError` from `/HCBv2/etc/qmf_release.xml` (0 on the checked Toons), so Adjust is a clean absolute set.
- **Fix**:
  - FreeToon reads the offset with GetTempOffset at startup and when the Heating modal opens, and writes it with AdjustTempOffset when the slider is released (0.1 steps, +-6.0), then reads it back. No local offset is added anymore; `temp_offset_centi` is gone from toonui.cfg. The PWA settings API keeps the `temp_offset_centi` key, now backed by happ_thermstat, and only writes on an actual change.
  - Client mode: slider stays disabled ("Beheerd door Master"); the slave shows the master's corrected temperature.

### 46. Verlichting Tile Ignores the Domoticz/HA Switches on a Slave Toon
- **Status**: Works as designed (2026-11-14), left as is.
- **Description**: On a slave Toon (client mode) switching Domoticz or Home Assistant off in Settings does not change the Verlichting tile; it keeps showing the Domoticz lights. Earlier report: after enabling Domoticz the tile showed nothing until a GUI restart.
- **Explanation**: In client mode the slave takes its Domoticz devices from the master (`/api/state` "dz" list) and the tile uses them whenever that list is non-empty (`client_mode && domoticz_state.count > 0`); the slave's own `enable_domoticz` / `enable_ha` are not consulted. The two tile switches are still the slave's own choice (`domoticz_tile_idx1/2`) from the master's list. Switching Domoticz off on the master removes them on the slave too.
- **Possible later change**: show "Beheerd door Master" on the slave's Domoticz/HA switches, or add a per-slave "show master lights" setting.

### 47. Energy History of Z-Wave Smart Plugs (Already Logged by the Toon)
- **Status**: Done (2026-11-15): Statistics tab "Stekkers" (plugs.c + screen_stats.c), confirmed working on a Toon by the user. Related: #43, #48.
- **Implementation**: plugs.c reads `config_hcb_rrd.xml` (/mnt/data/qmf/config, fallback /qmf/config) for `<rrdLogger>` blocks with deviceSvc ElectricityQuantityMeter + deviceVar CurrentElectricityQuantity + an rra `1monthhour`; logger `<name>` is used with getRrdData loggerName=, names and live W from `hdrv_zwave?action=getDevices.json`. Tab only shown when plugs exist (not on a slave). Nu = last 12 h hourly, Uren from 1monthhour, Dagen/Weken/Maanden/Jaren from 1yearday; small values in Wh; Euro with the electricity tariff. hcb_rrd also accepts `logger=<logger-uuid>` (the .rra/.dat file name), used by the sim hook TOONUI_PLUG_LOGGERS.
- **Finding (production Toon 2, read-only, 2026-11-14)**: when a Z-Wave smart plug is added in the stock UI, hcb_rrd creates a logger for it by itself, although the stock UI never shows that data. On the production Toon two NAS_WR01Z plugs ("Tuin", node 10, uuid `482195c7-f27e-4958-8c43-600996d7be5f`; "Kerstboom", node 11, uuid `6f78136e-c3f9-429f-bb8d-834bec7dfba9`) each have a logger in `/mnt/data/qmf/var/hcb_rrd/<logger-uuid>.*`:
  - variable `CurrentElectricityQuantity` (cumulative, Wh) of service `ElectricityQuantityMeter`, integer
  - archives `1monthhour` (3600 s steps, one month) and `1yearday` (86400 s steps, one year), consolidation "last"
  - the `.dat` header names the device uuid; the archive files are `<logger-uuid>-1monthhour.rra` / `-1yearday.rra`.
  - live values in `hdrv_zwave?action=getDevices.json`: `CurrentElectricityFlow` (W) and `CurrentElectricityQuantity` (Wh).
- **Approach**:
  - Z-Wave plugs need no config editing: read the history with the existing `boxtalk_get_rra_data(<device uuid>, "1monthhour" / "1yearday")` (hcb_rrd GetRraData, as the meter history), turn the cumulative counter into usage per hour/day.
  - Show it per plug: an "Stekkers" tab in Statistics (hours for the last month, days for the year), and/or a history view on the Devices screen.
  - Only HomeWizard sockets (#43) would need extra `<rrdLogger>` blocks in `config_hcb_rrd.xml`; the Z-Wave loggers above show the exact block shape to copy.

### 48. New Z-Wave Plugs and HomeWizard Sockets in "Stekkers" (Loggers via hcb_config)
- **Status**: Done (2026-11-15): pairing + EnableDevice, HomeWizard socket loggers and the plug fixes up to 20094eb, confirmed working by the user. Follows #47.
- **Facts (static RE of happ_smartplug / hcb_rrd, Toon 1 binaries)**:
  - Stock "add plug" = Z-Wave IncludeDevice, then `EnableDevice` (arg `ZwUuid` only) to happ_smartplug (handler table `default`, i.e. the specific1 route). happ_smartplug then creates the plug, matches the device's services against its templates and sends one ConfigProvider `SetObjectConfig` to `<host>:hcb_config` with `<Config>` holding one `<rrdLogger>` per match. A plug paired through FreeToon without EnableDevice gets no loggers.
  - Logger block (no `<uuid>`, internalAddress = name; futureBins/sampleOffset only when > 0; no saveInterval):
    `<rrdLogger><package>hcb_rrd</package><name>N</name><internalAddress>N</internalAddress><visibility>0</visibility><deviceVar>CurrentElectricityQuantity</deviceVar><deviceSvc>ElectricityQuantityMeter</deviceSvc><deviceUuid>U</deviceUuid><sampleType>integer</sampleType><rra><binLength>3600</binLength><bins>744</bins><consolidator>last</consolidator><name>1monthhour</name><minSamplesPerBin>1</minSamplesPerBin></rra><rra><binLength>86400</binLength><bins>365</bins><consolidator>last</consolidator><name>1yearday</name><minSamplesPerBin>1</minSamplesPerBin></rra></rrdLogger>`
    plus `plug_elec_flow_%` (CurrentElectricityFlow / ElectricityFlowMeter, double, rra 5min 300 s x 2016, avg). Name = template with `%` replaced by the uuid minus its first character (happ_smartplug bug).
  - Delete: specific1 `RemoveRrd` to hcb_rrd with `<intAddr>`(logger name) and/or `<uuid>`; files removed ~5 s later.
  - Feeding: HTTP `hcb_rrd?action=setRrdData` (or BoxTalk `SetRraData`), `logger=<uuid>` or `loggerName=`, `rra=`, `samples={"<ts>":value,...}`; works with an empty deviceUuid (no live sampling). Write only bins that have already closed: at slot close hcb_rrd pushes its own consolidated (possibly NaN) value into the bin.
- **Plan**: (1) after pairing a plug in FreeToon send EnableDevice; (2) per HomeWizard Energy Socket create a `hw_socket_qtty_<n>` logger (empty deviceSvc) via SetObjectConfig and push the socket's `total_power_import_kwh` (Wh) for each closed hour/day; show them in Stekkers next to the Z-Wave plugs. Existing sockets get a logger too; history starts at creation (the socket API has none).

### 49. Simpler Forecast Strip on the Dim Screen
- **Status**: Done (2026-11-15, e2104c6): dim_fc_build / dim_fc_paint shared by the classic and grid dim; weekday + max temp in 24/28 px, icon tinted halfway to white; checked in the sim on Toon 1 and Toon 2.
- **Description**: The 5 forecasts at the bottom of the dim screen show hour/day, temperature and wind (e.g. "16:00 / 26°C ZO2") in small print and are hard to read from a distance.
- **Wanted**:
  - Per forecast only: day of the week (e.g. "zo", "ma") and that day's maximum temperature, both in a bigger font, plus the weather icon.
  - So daily forecasts (5 days), not the 3-hourly slots; no wind.
  - Weather icons in a lighter colour (they now use the full colour icon tint; lighter/softer for the dark dim screen).
- **Scope**: both the classic dim layout (screen_dim.c) and the custom grid (screen_dim_grid.c, forecast block); check Toon 1 (800x480) as well.

### 50. Dead-Code Check After 250+ Commits
- **Status**: Done 2026-11-17 (1cfafc2, 4eca8df, 94af246, 4acd392): the 9 statics + 46 globals below and the orphans they exposed removed, `screen_dim_stock.c` deleted; `boxtalk_lock`, `boxtalk_zwave_basic_set` and `fbdev_set/clear_cutout` (Toon 1 video) turned out to be used and stay. Verified: all 4 targets + ABI, sim renders, soak. Open follow-ups: `icon_ha`/`icon_domoticz` data, `sonos_port`/`vnc_pass` load/save, ~25 functions that could become static.
- **Description**: After many features, rewrites and removed paths, find code that is no longer used and remove it.
- **Approach**:
  - Compiler/linker: build with `-Wunused-function -Wunused-variable` (+ `-ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--print-gc-sections`) for both targets and the sim; list functions the linker drops.
  - Symbols: exported functions with no callers (`nm` / grep across src), static helpers never called, `#if 0` blocks, settings keys that are loaded/saved but never read, screens/integrations no longer reachable from any menu.
  - Known candidates seen recently: the old fire-and-forget `boxtalk_zwave_include/exclude` (Z-Wave screen now uses ma_admin), the unused `boxtalk_get_rra_data` / `rrd_response_buf`, `screen_dim_stock` (not wired into screen_dim_create), the generic `display_indoor_temp` users removed in #45.
  - Remove in small commits per area; build + sim render of the affected screens after each.
- **Review result (2026-11-17)**: method = sim build with `-Wunused-function/-variable` (the Makefile passes `-Wno-unused-function`; `-rdynamic` keeps every global, so `--gc-sections` can't show dead globals) + `nm` cross-reference of the Toon 2 and sim objects + text grep over freetoon-lvgl (incl. `web/` WASM client, scripts) and the plugin repos (freetoon-integrations, sonos, …). No `#if 0` blocks.
  - **Unused statics (9)**: `pwa_server.c mime_for`, `screen_home.c fc_compact / on_program_tap / on_vent_bump / on_home_gesture_to_lights` (last one left over from #53), `ventilation.c fetch_status / fetch_lastcmd`, `wastecollection.c json_int / ics_slot`.
  - **Unused globals (46, no caller anywhere)**: airhist `airhist_series`, `airhist_pres_series`; backlight `backlight_get`; boxtalk `boxtalk_get_object_config`, `boxtalk_get_rra_data` (+ `rrd_response_buf/ready`), `boxtalk_lock`, `boxtalk_temp_override_active`, `boxtalk_zwave_include/exclude`; canonical_state `canonical_state_subscribe/unsubscribe`; client_link `client_link_curtain`; domoticz `domoticz_set_level_async`; fbdev `fbdev_clear_cutout/get_sizes/set_cutout/set_offset`; homeassistant `ha_cover_stop_async`, `ha_devices_lock/unlock/save`, `ha_get_snapshot`, `ha_light_toggle_async`, `ha_lights_all_on/off_async`, `ha_lock/unlock`; homewizard `homewizard_get_snapshot`; inbox `inbox_load`; layout `dim_block_default`, `layout_load/save`; mqtt `mqtt_subscribe_async`; `screen_dim_layout_editor_close`, `screen_layout_editor_close`, `screen_weather_hourly_close`; screen_zwave `zwave_switch_count` (#53 now uses the cached count); stats `stats_elec/gas/water_flow_5min`; ui_stack `clock_colon_pulse`; update_modal `update_modal_is_open`; ventilation `vent_bump_speed`, `vent_refresh_settings`, `vent_settings_json`; weather `weather_refresh_async`.
  - **Only used in their own file (~25)**: could become `static` (no size win; keeps the exported surface plugins can bind to small).
  - **Unreachable on device**: `screen_dim_stock.c` (465 lines, only the sim opens it) — decide: wire into the stock theme or delete. `icon_ha` / `icon_domoticz` image data: last user was the #53 lights handle.
  - **Settings**: `sonos_port`, `vnc_pass` are loaded/saved but never read (keep the struct fields — removing shifts plugin offsets; can drop load/save + export entry). `_reserved_temp_offset` is a deliberate placeholder.
  - Not dead: monocypher's unused API (vendored library), icons reached via `weather_icon_for` / `waste_icon_for_label` / `moon_phase_icon`, `client_link_apply_state` / `video_*` (used by `web/` WASM client).

### 51. Fetch Log Files Without SSH/SCP
- **Status**: Done (2026-11-15, 728fb17): /logs HTML index + /log raw tail on :10081, 256 KB rotation in healthcheck.c + ui_launcher.sh.
- **Description**: Retrieving `/tmp/toonui.log` (and e.g. ui_launcher.log, integration logs) now needs SSH/SCP into the Toon. Wanted: download them from a browser.
- **Implementation**: FreeToon serves them on port 10081: `/logs` provides a browsable HTML list of all `/var/volatile/tmp/*.log` files with file sizes and download/view links, and `/log?file=<name>` streams the log directly. A direct icon link was added to the header of the remote web app.
- **Log rotation & capping**: `healthcheck.c` checks every 60 s and rotates `toonui.log` at 256 KB, and `scripts/ui_launcher.sh` watchdog checks all `/var/volatile/tmp/*.log` every 60 s, truncating in place with a `.1` copy kept.

### 52. Direct Solar Inverter Integration (Porting QML `solarPanel`)
- **Status**: Merged into #60 (2026-11-22): the plan of approach for porting the QML `solarPanel` app lives there.
- **Description**: The community QML app `solarPanel` (ToonSoftwareCollective / oepi-loepi) allowed rooted Toons to read solar power and cumulative yield directly from solar inverters over the local network (LAN / Wi-Fi) without needing a hardware meter adapter (ZonOpToon) or physical kWh submeter in the fusebox. Wanted: bring this capability natively into FreeToon.
- **Background & QML Architecture**:
  - The QML `solarPanel` app uses `inverters.json` and brand-specific plugin definitions (`solarpanel-plugins`) to query inverters (Enphase Envoy, SolarEdge, SMA Sunny Boy, Fronius, Growatt, GoodWe, APSystems, Hoymiles/OpenDTU, etc.) or local platforms (Home Assistant).
  - It retrieves two core metrics: current production (W) and total cumulative production (kWh).
  - To integrate with Toon's native energy graphs, it pushed data into `hcb_rrd` (`solar_flow` and `solar_quantity`) using `setRrdData`, emulating a hardware solar meter adapter.
- **FreeToon Status Today**:
  - `energy.c` already provides `energy_have_solar()` and `energy_solar_w()`, currently fed by hardware meter adapters, HomeWizard kWh meters (single or dual array), or Home Assistant.
  - FreeToon already has a fully functional Solar home tile and a dedicated "Zon" tab in Statistics (`screen_stats.c`), which queries `solar_flow` and `solar_quantity` from `hcb_rrd`.
- **Implementation Approach**:
  - **Inverter Driver / Poller (`solar_inverter.c`)**:
    - Settings under Instellingen → Integraties → **Zonnepanelen**: select inverter brand / type, IP address / hostname, optional API token/port.
    - Support the most common local LAN inverter endpoints natively (e.g. Enphase Envoy `/production.json`, SolarEdge local/Modbus, SMA WebConnect `/dyn/getDashValues.json`, Fronius Solar API, OpenDTU/Hoymiles `/api/livedata/status`, and a configurable generic JSON endpoint with custom JSON paths for W and kWh).
    - Polling interval ~10–30 s while daylight / connected.
  - **Feed `energy.c` & `hcb_rrd`**:
    - Feed live wattage into `energy_solar_w()` so the home Solar tile and remote web interface show real-time production.
    - Feed hourly and daily cumulative kWh into `hcb_rrd` via `rrd_push()` (or BoxTalk `SetRraData`), ensuring the "Zon" tab in Statistics populates full historical graphs.
  - **Conflict check**:
    - As with the original QML app, ensure direct inverter polling is disabled if a hardware meter adapter or HomeWizard solar meter is already bound, preventing double counting.

### 53. Unified Devices hub — one entry point for lights & plugs
- **Status**: Stage 1 released in 1.0.12 (hub by room, swipe / status-bar entry, handle removed) and in use since. Stages 2-3 (favourites, merged Apparaten tile, one-time migration notice) not started.
- **Decisions (2026-11-17)**: handle removed entirely from the LVGL home (the stock theme keeps its handle — no other entry there — but it now opens the hub); hub grouped **by room** (Hue rooms + HA areas merged by name, case-insensitive; devices without a room grouped per source); tile migration in stage 2 = silent conversion **plus** a one-time notice.
- **Stage 1 notes**: `screen_devices.c` (owned screen, max 96 rows); HA areas via one `POST /api/template` (`area_name()`) on list change / every 30 min (`ha_devices[].area`, not persisted); Hue room membership from `/groups` `lights` (`hue_group_t.light_ids`, `hue_light_room()`); backend screens via chips at the top; Verlichting + Slimme stekker tile taps open the hub; devices button left of the gear, envelopes shift one slot. Client (slave) Toons: HA devices are not mirrored, so they do not appear in the hub there (same as the old HA screen).
- **Problem**: Five entry points reach different things:
  - left-edge handle (Domoticz / HA logo or ⚡) and swipe-right on the thermostat half open **one** backend by fixed priority Domoticz → Hue → HA → Z-Wave (`open_lights_backend()` in `screen_home.c`) — a second system (e.g. HA next to Hue) is unreachable from home;
  - the handle floats left-middle and overlaps tiles;
  - **Verlichting** tile (`LT_LIGHTS`): 2 quick switches from Domoticz *or* HA pins *or* Z-Wave;
  - **Slimme stekker** tile (`LT_SMARTPLUG`): Z-Wave switch #0 only, tap opens the Z-Wave *management* screen;
  - sockets appear in the Hue, Domoticz and HA screens (HomeWizard section); favourites are set three ways (`domoticz_tile_idx1/2`, HA `pin_home`, Z-Wave index 0).
- **Proposal**:
  1. **"Apparaten" hub screen** listing devices of all enabled backends (Hue, HA, Domoticz, Z-Wave, HomeWizard sockets, efanlamp) with a small source badge, toggle, wattage where known, and ★ favourite. Filter chips Alles / Lampen / Stekkers / Overig. Backend screens remain as "Beheer" links (scenes, groups, pairing); Z-Wave include/exclude only under Settings.
  2. **One "Apparaten" tile** replacing Verlichting + Slimme stekker: shows the ★ favourites from any backend (2 at 2×2, 4 at 3×2, 6 at 3×3), title tap → hub. Migrate layouts (`LT_LIGHTS`/`LT_SMARTPLUG` → new type) and existing pins → ★.
  3. **No floating handle**: keep swipe-right (→ hub) and add a devices icon in the status bar next to the inbox envelope; hide it when the Apparaten tile is placed.
- **Order**: (1) hub + swipe/status-bar entry, drop handle — fixes overlap and the second-backend problem; watch Toon 1 memory (cap/paginate rows). (2) ★ favourites + merged tile + layout migration. (3) remove per-backend socket sections and old tile types after one release.
- **Open decisions**: remove the handle entirely vs. keep as off-by-default option; hub grouped by type (filter chips) or by room (only HA/Hue have rooms); silent migration of old tiles vs. one-time notice.

### 54. Meter adapter: analog solar (and analog elec/gas) pulse divider + solar display
- **Status**: Implemented 2026-11-17 (8147621 stats loggers, e03471b pulse step, 66da2fb solar source); sim-verified only — needs feedback from a tester with an analog (S0) solar or analog elec/gas meter after release. Open questions for testers: does hdrv_p1 answer SetMeterConfiguration (else the UI says "Niet bevestigd"), does the card show the new pulse value, do Zon tab history + Zonnepanelen tile show production.
- **Findings**:
  1. **No pulse setting.** Stock QML (`apps/eMetersSettings`) sets the pulse rate per resource with hdrv_p1 `specific1` action `SetMeterConfiguration`: `<devices><device uuid=…><solar><meterType/><divider/><dividerType/></solar></device></devices>`, and sends the default the moment a meter type is chosen (`setDividerMeterType`). FreeToon's Adapters → Beheren only sends `SetSensorConfiguration` (which input type), never a divider — an adapter switched to analog solar/elec/gas in FreeToon runs on hdrv_p1's own default; a divider set earlier in the stock UI is kept. Units/defaults (`Constants.js` `cValueUnits`): solar imp/kWh (def 10000) or Wh/imp; elec imp/kWh (def 1000) or Wh/imp, Ferraris rev/kWh (def 300); gas rev/m³ (def 100) or L/pulse. Current values come back per sensor in `usageDeviceInfo` (`divider`, `dividerType`, `meterType`).
  2. **Solar tile ignores the adapter.** `energy_have_solar()/energy_solar_w()` only know a HomeWizard kWh meter and the HA sensor; happ_pwrusage's `powerProduction` is only used to compute the net grid power.
  3. **Statistics → Zon reads loggers that don't exist.** `screen_stats.c` asks hcb_rrd for `elec_flow_prod`, `elec_quantity_nt_prod/lt_prod`, `solar_flow`, `solar_quantity` — "unknown logger" on .18. Stock/community names are `elec_solar_flow` (5min) + `elec_solar_quantity` (5yrhours/10yrdays) for a solar meter, `elec_produ_flow` + `elec_quantity_nt_produ/lt_produ` for P1 return delivery (seen in a user's config_hcb_rrd.xml and on .18).
- **Plan**: (a) divider field in the adapter editor for analog elec/gas/solar (read from usageDeviceInfo, write with SetMeterConfiguration, stock units/defaults); (b) meter adapter as third solar source for the tile; (c) fix the Zon logger names (try elec_solar_*, then *_produ). (c) can be checked on any Toon; (a) on .52 with the spare adapter's analog input; real numbers need a tester.

### 55. "Klok Studio": configurable clock faces as home tile and dim block
- **Status**: Done: released in 1.0.14; confirmed on Toon 2 (.16) and Toon 1 (.24) by the user (2026-11-18).
- **Implementation**: tile type `LT_CLOCKSTUDIO` (18, layout editor, page 1, min 2x2) and dim block `DB_CLOCKSTUDIO` (14, grid dim only, hidden by default). Faces: Automatisch, Klassiek analoog, Minimaal analoog, Vierkant analoog, LED digitaal, Gestapeld, Digitaal, Agenda. Themes Blauw/Paars/Wit/Rood/Groen, per-config colour overrides (background, foreground, accent, hour/minute/second hand), seconds, date, 12 h, scale 65-100 %, corner radius 0-40, title. Two configurations (home tile, dim block) in `toonui_clockstudio.cfg` (`home.*` / `dim.*`, no `settings_t` change). Settings: Instellingen > Scherm > Klok Studio, or the gear in the full-screen view (tap the home tile); changes apply live. Analog faces are custom-drawn in one object; each second only the second-hand area is redrawn, LED digits redraw once a minute. One home tile and one dim block (the layout shows each type once). Sim: `TOONUI_SIM_TIME=HH:MM:SS`, steps `csfull` / `csset:<0|1>`, `SIM_CAL=1` agenda mock.
- **Inspiration**: ReToon app Clock Studio by Nomzi Design (https://github.com/Vibestar/ReToon-apps/tree/clock-studio-duo, `clock-studio/`). That app is GPL-2.0-or-later and FreeToon is MIT, so this is a **native LVGL implementation written from scratch** — use the app only for the feature list, copy no code or assets.
- **Scope** (no world-map face — user decision):
  - Faces: classic analog, minimal analog, square analog (small/large), LED digital (chamfered segments), stacked (HH over MM), calendar (clock + upcoming appointments from FreeToon's existing calendar data: HA calendar / iCal feeds), auto (picks a face that fits the cell; small cells fall back to simpler faces).
  - Themes: blue, purple, white, red, green + per-tile colour overrides (background, foreground, accent, hour / minute / second hand).
  - Per-tile settings: face, theme, colours, seconds (second hand / blinking colon), date on/off, title, 12/24 h, scale (65–100 %), corner radius (0 = square).
  - Available as a home grid tile (layout editor) and as a dim block (dim layout editor), each with its own settings; tap on the home tile opens a full-screen view with the same settings.
- **Implementation notes**: retained objects, update only what changes each second (hands via rotation / line points, LED segments recoloured, not rebuilt) — keep Toon 1 CPU low (see ui_lazy.h). Settings per tile stored with the layout entry. Existing clock tile / dim clock (incl. moon phase) stay; Klok Studio is an additional tile type.
- **Later (separate)**: option B — a ReToon app host on QuickJS (ReToon app API levels 1–10) so ReToon apps run unchanged as separate downloads; large and a moving target, not planned.

### 56. Indoor humidity from the Toon's own sensor on screen
- **Status**: Done (2026-11-18): also a dim block `DB_CLIMATE` (02f76ff, Toon 2 only); option (3), new "Binnenklimaat" layout tile (`LT_CLIMATE`, appended to the enum; min 2x2): humidity big, air-quality word (coloured, "Lucht: Goed" on wider tiles), CO2 + TVOC line; "--" for unreported values. Not offered in the Toon 1 layout editor (no sensor). Sim-verified at 2x2 and 3x2.
- **Situation**: `toon_state.humidity` is already filled from the Toon 2's own sensor (BoxTalk `HumiditySensor` / thermostatInfo `currentHumidity`) and exported over MQTT and the PWA, and the classic theme has a "Luchtvochtigheid" tile — but the FreeToon dashboard shows it nowhere (`lbl_t_humidity` is a leftover of the old thermostat tile and always NULL). Toon 1 has no humidity sensor.
- **Options**: (1) add "RV 45%" to the dim screen's metrics row (TVOC / CO2 / CV bar / air quality) in `screen_dim.c` + `screen_dim_grid.c` — small; (2) small "RV 45%" on the home thermostat tile — small, but that tile is already full on Toon 1; (3) new "Binnenklimaat" layout tile (humidity + CO2 + TVOC + air-quality label) — append the LT_* value at the end of the enum. In all cases hide it when no value is reported (Toon 1).

### 57. Layout editor: pick an integration directly on page 2
- **Status**: Open (2026-11-18, parked after 85ff5df).
- **Situation**: since 85ff5df page 2's "+ Tegel" offers only "Integratie" (and Neerslag); the integration itself (e.g. Teletekst) is chosen afterwards by tapping the empty tile on the home screen. A user expected to find Teletekst in the editor list.
- **Options**: (a) clearer label, e.g. "Integratie (bijv. Teletekst - koppelen op het scherm)" plus one line of explanation; (b) list the installed integrations in "+ Tegel" on page 2 and place the tile already bound. Page-2 slots are bound by position (slot 4+i), so (b) must keep a binding with its tile when tiles are moved or deleted in the editor.

### 58. Release 1.0.14: first non-beta release
- **Status**: Published (2026-11-18), tag 1.0.14 on ed11e1a.
- **Contents**: OT bridge Proxy mode removed (b984bd1, a5e0531, adcb532); Klok Studio (#55); dynamic energy contracts (#44); Binnenklimaat tile and dim block (#56); thin dim-screen font option; brighter Neerslag dim block; Toon 1: half the animation sprites, Wi-Fi keyboard, dim energy-bar gap; release notes without CRLF "r". The "- beta" label next to the version in the update screen is gone; the Beta / dev channel stays for test prereleases.

### 59. Release 1.0.15: dynamic tariff per commodity, fixes, CI
- **Status**: Published (2026-11-19), tag 1.0.15 on 874d306.
- **Contents**: dynamic contract separately for electricity and gas (field request) + fix: 1.0.14 never saved the dynamic setting and surcharges (6aaca37); updates install exactly the release the Toon found, scripts from that tag (#32, 71a65b9); thermostat screen no longer redraws the whole screen twice a second (52ebf96); Toon 1 thermostat setpoint row no longer overlaps Water / Retour (a43ba76). Build side: lvgl submodule back on upstream + patches (a72f6fe), CI on every push (e6ea064 and follow-ups). First release that also ships the browser UI (index.html / index.js / index.wasm) since 1.0.12.

### 60. Solar production: one holistic approach (all sources, all users)
- **Status**: First part ships in 1.0.18 (#66): app "Omvormers" (Enphase, Solis, Omnik) in the catalog with min_toonui 1.0.18; the user has testers for all three brands. Still to do: Zon statistics in hcb_rrd, status on the Toon itself. Supersedes #52 (porting the QML `solarPanel` app is part of this).
- **Progress (2026-11-23)**:
  - Marketplace app `inverters` ("Omvormers", 0.1.0) in freetoon-integrations (832073e): C daemon, drivers Enphase Envoy (firmware < 7 plain HTTP; 7+ HTTPS + owner token, pasted or fetched via Enlighten login.json + entrez `/tokens`, cached per Envoy address), Solis / Ginlong and Omnik WiFi sticks (`/status.html`, `/js/status.js`), SolisCloud API (HMAC-SHA1). Omnik portal and the old Solarman portal left out (dead). In the catalog with `"min_toonui": "1.0.18"` (ships with the 1.0.18 core; testers for all three brands are available, so no separate test round first).
  - Contract: the app POSTs `/api/v1/solar/source` on 127.0.0.1:10081 (`power_w`, `today_wh`, `total_wh`, `ok`, `msg`; loopback only) instead of a BoxTalk service: same channel the other apps use for `/state`.
  - Core: `solar_app.c` keeps the reading; `energy_solar_w()` / `energy_have_solar()` take the app first while it is active (posts < 10 min, a reading in the last 24 h, so a sleeping stick at night keeps the source), then HomeWizard, HA, meter adapter. One source at a time.
  - Settings via the manifest (decision 2026-11-23: web page + manifest, not a UI plugin): `app_settings.c` builds a generic form from the manifest's `settings` block (select / text / secret, `show` per TYPE, `guards` = secrets that must be typed again when the address changes; secrets write-only, file 0600). Page `:10081/solar` shows the active source, the app's status and its form; linked from the /logs page. `minijson.c` (path lookup, shared with the app) parses manifests and answers.
  - Tested: app `make test` (44 checks: parsers, MD5/SHA-1/HMAC vectors, SolisCloud signature vs openssl); e2e with the sim and synthetic mocks (stick, Envoy old/fw7 with 301 + Bearer, SolisCloud with real signature check, guards, cross-site 403, non-loopback 403, no secrets in page/logs); daemon binaries ran on .16 and .24 (curl 7.50 on Toon 1, TLS to the real SolisCloud). Not tested: the Enlighten token fetch (needs a real account), real inverters.
  - Version gate: catalog `min_toonui` 1.0.18 (Toons 1.0.8+ show "Vereist 1.0.18"); on top of that the app's tile says "Vereist FreeToon 1.0.18" when the core answers the source post with 404 / 401, which covers `/api/install` and Toons older than 1.0.8. New core imports `getpeername` and `fdopen`: resolved by the device linker on .24 and .16 (`ld.so --verify`), ABI baselines refreshed.
  - Zon statistics (built 2026-11-23, not on a device yet). Found: HomeWizard kWh meters only fed the live W (tile, dim, "Huidig"), their counters were never read and nothing reached hcb_rrd, so the Zon tab fell back to P1 net export (or stayed empty with a HomeWizard P1). **Decision (user): all reported solar production adds up**, live and in the statistics (two sources on the same panels count twice: set up one).
    - `solar_acc.c` (host test `make test-solar`, 25 checks): one running total in Wh; each source adds only its own increase (lifetime counter, daily counter with the new day detected, or power integrated with gaps > 3 min not bridged), so sources appearing, going offline or resetting never make it jump. Persisted in `/mnt/data/freetoon_solar_acc.dat`.
    - `solar_rrd.c` (master): every 10 s feeds HomeWizard kWh 1+2 (`total_power_import/export_kwh`, the larger counter), the inverter app (`total_wh`, else `today_wh`), the HA sensor (W) and the meter adapter's solar meter (W); writes the total to `elec_solar_quantity` 5yrhours / 10yrdays at the end of each hour / UTC day (closed bins, END label, re-sent after 10 min) and the 5-minute average W to `elec_solar_flow` 5min. Creates the loggers when missing (stock layout; .24 has none, .16 has `elec_solar_quantity` bound to a device and no `elec_solar_flow`). Loggers that sample a device are pointed at placeholder while FreeToon has its own sources (device saved in `/mnt/data/freetoon_rrd_solardev.cfg`, given back when none is left); the total continues from the logger's last value.
    - `energy_solar_w()` / `energy_have_solar()` sum all sources (was: one at a time); /solar lists every source that counts.
    - Tested on .16 (2026-11-23/24, master; two fake kWh meters of 3.6 kW served by the Toon's own lighttpd on 127.0.0.1 and its LAN IP): `elec_solar_quantity` taken over (device saved), `elec_solar_flow` created, 5-minute values 7200 W (both meters summed), every hour 00:00-14:00 written (+7160..7180 Wh per hour), the day at 01:00 CET (UTC midnight), shown on the Zon tab. Restart at 09:58:30: the missed 10:00 hour was written at start (a63da45). Sources removed + restart: the saved device was given back. Then the test archive was restored from the backup; `elec_solar_flow` (created) keeps 7200 W test values for its 31 days.
    - Not tested yet: the inverter-app and HA paths, and continuing from real Eneco history (.16's old solar logger only held NaN).
  - Next: a status line on the Toon itself.
- **Problem**: solar today has three unrelated entry points, none of them usable for a non-technical user without extra hardware: HomeWizard kWh meter (`hw_kwh_host` / `hw_kwh2_host`, needs a meter in the fuse box), meter adapter S0/analog solar (#54, rare, untested on real hardware), Home Assistant (`ha_solar_entity`, live W only). `energy_solar_w()` (energy.c) picks the first one that's set. The P1 meter only sees net export, not production.
- **Gaps**:
  - No route without Home Assistant or extra hardware (the old QML `solarPanel` app read inverters directly).
  - Home Assistant: only live W from one hand-picked entity; the kWh total isn't read, so the Zon statistics (`elec_solar_flow` / `elec_solar_quantity` in hcb_rrd) stay empty. HA's Energy dashboard already knows the solar kWh sensor (websocket `energy/get_prefs`); the W sensor is usually on the same device (`device_class: power`).
  - No rule against counting the same panels twice when several sources are configured.
- **Inverter landscape** (from general knowledge, verify before building): readable on the home network: Enphase Envoy, SMA, Fronius, OpenDTU / AhoyDTU (Hoymiles); often cloud-only (account + API key) or Modbus: Growatt, Huawei, newer SolarEdge, GoodWe, Solax, Sungrow.
- **To decide in the design**:
  - One "Zonnepanelen" settings entry that lists every source (inverter, HomeWizard kWh, meter adapter, HA) with one active source, instead of separate settings per integration.
  - Which inverter brands first; auto-discovery on the LAN so users needn't know the IP.
  - Whether cloud-only brands are in scope at all (API keys are what these users want to avoid).
  - HA: auto-pick the solar sensor from the Energy dashboard, and store the kWh total so statistics fill.
  - Every source writes live W + hourly/daily kWh into hcb_rrd the same way (only closed bins, as for the HomeWizard sockets in #48).
  - Before starting: ask users (forum) which inverter brands they actually have.
- **Decisions (2026-11-22)**:
  - Split: the core owns the solar pipeline (source choice, `energy_solar_w()`, hcb_rrd writing, tiles / dim / stats / remote page, no double counting); inverter drivers are a marketplace app. Matches STRATEGY pillar B ("solar as marketplace apps") and "no built-in integrations the maintainer cannot test".
  - oepi-loepi (author of the QML `solarPanel` app and `solarpanel-plugins`) is in the team, so porting his driver logic is fine (both repos carry no license file; get his OK on the license of the port, MIT like the rest).
- **The QML app (studied 2026-11-22, ToonSoftwareCollective/solarPanel 1.4.13 + solarpanel-plugins)**:
  - 38 plugins in `inverters.json` (JS downloaded at run time). Local (~13): Enphase (old Envoy), Fronius, SMA, OpenDTU, Zeversolar, Omnik, Kostal, SAJ, FP4ALL, Domoticz, Home Assistant (2x), HomeWizard. Cloud (~25): Growatt, GoodWe SEMS, SolarEdge, Huawei FusionSolar, Solis / SolisCloud, Hoymiles, APS, Deye, Fox, Solax, Delta, Canadian Solar, Autarco, Solarman, PVOutput, Zonneplan, Enphase Enlighten / token. The "-T" variants get tokens through `solarPanel.sh` (curl).
  - Reads current W, total kWh, today kWh; up to two inverters; own use = production minus P1 export; 6 tile variants; today / day / month graphs with sunrise/sunset (Buienradar); JSON for phones at `/solar.html`.
  - hcb_rrd: merges three loggers into config_hcb_rrd.xml and reboots (`elec_solar_flow` 5 min avg, `elec_solar_quantity` 5yrhours + 10yrdays "last", `elec_produ_flow`), then `setRrdData` per 5 min / hour / day.
- **Architecture**:
  - Contract: one BoxTalk service (e.g. `solarSource`) every source speaks: `power_w`, `total_kwh`, optional `today_kwh`, per-inverter index. Third-party apps only need this contract.
  - Core `solar.c`: exactly one active source (inverter app, HomeWizard kWh, Home Assistant, meter adapter) -> `energy_solar_w()`; creates the loggers via `SetObjectConfig` (no config edit, no reboot, as #48) and writes closed bins only; the app never writes hcb_rrd itself.
  - Migration for QML-app users: their loggers have the same names but other `deviceUuid`s; repoint via `SetObjectConfig` keeps the history.
- **Phases**:
  - 0. Forum poll which brands users have; testers per brand (no driver to stable without a real inverter).
  - 1. Core pipeline, testable in the sim with a synthetic fake source (like `tests/homey_mock.py`): `solar.c`, one "Zonnepanelen" settings entry listing every source, rrd writer, HA kWh sensor from the Energy dashboard (`energy/get_prefs`). Already useful on its own: HomeWizard and HA users get filled Zon statistics.
  - 2. Marketplace app "Omvormers", local brands first (Fronius, SMA, OpenDTU / AhoyDTU, older Enphase, Zeversolar, Omnik, Kostal, SAJ); Domoticz / HA / HomeWizard stay in the core. Settings: a UI plugin `.so` (as Stocks / Sonos) or, preferred, settings fields declared in the manifest (STRATEGY pillar B; helps every app, needs a manifest extension first).
  - 3. Cloud brands, opt-in and clearly labelled (same app or a separate "Omvormers (cloud)" app; open decision). Passwords / tokens never in logs or the diagnose report.
  - 4. Extras from the QML app: own-use tile / dim block, two inverters summed, sunrise/sunset in the day graph, 5-min "today" graph.
- **Risks**: double counting (one active source is a hard rule); Toon 1 CPU (poll ~30 s, slower at night, as the app does); cloud APIs change often (Growatt / Huawei fixes fill the app's changelog), another reason for app ownership per brand.
- **Open decisions**: cloud brands in the same app or a separate one; app settings via UI plugin or manifest extension; start phase 1 now.

### 61. Release 1.0.16: diagnose report, Ketel dim block, OT bridge and boiler-type safety
- **Status**: Published (2026-11-20), tag 1.0.16 on efcab22 (binaries built from 2fa940f, same code). The Toon 1 serial-line fix of the diagnose report (6c39643) came after the tag and ships with 1.0.17.
- **Contents**: diagnose report for support without SSH (56bde25, 990807c: :10081/diag, port 80 /freetoon-diag.txt under both UIs); dim block "Ketel" with flow / return / CH pressure (efcab22); OT bridge "Draadloos" renamed "Via OTGW" and only allowed with an OTGW host, quby_bridge gets the configured host instead of a built-in test address, updates fall back to Direct without one (9266104); no automatic switch of the boiler type to On/Off outside the user's own OpenTherm check (489579a); calendar backs off for a dead feed instead of retrying every 10 s (b5ef15f); classic dim: units under the bar values (2d86a50), NILM label removed from the forecast strip (7e14166); missing glyphs in the big readout fonts fall back to Montserrat 48 (178f1a8); waste: no provider fetch without an address (96dc739).

### 62. "Via OTGW" (wireless OT bridge) never worked on a Toon 1: boiler UART is ttymxc1 there
- **Status**: Open (2026-11-20), low priority (experimental mode, no known Toon 1 user with an OTGW).
- **Finding**: the boiler UART is `/dev/ttymxc0` on a Toon 2 but `/dev/ttymxc1` on a Toon 1 (Z-Wave: ttymxc1 on a Toon 2, ttymxc2 on a Toon 1; checked on .16 and .24 via `/proc/<happ_thermstat>/fd`). quby_bridge, ot_mode_switch.sh, ui_launcher.sh, install.sh (RELEASE_UART) and the settings / adapters screens hard-code ttymxc0 (~35 places). On a Toon 1 "Via OTGW" therefore bind-mounts its PTY over an unused line: happ_thermstat keeps talking to the boiler module on ttymxc1, the bridge never sees OT traffic. Harmless, but the mode does nothing there. The umount / release steps for Direct are no-ops on a Toon 1 for the same reason (nothing was mounted).
- **Fix options**: (a) detect the line at run time (the one happ_thermstat holds, or by model: arch.conf without "nxt" = Toon 1 -> ttymxc1) in one shared place and pass it to quby_bridge (`-d <dev>`, new option) and the scripts; (b) hide "Via OTGW" on a Toon 1 until (a) is done. Test on .24 only with a spare OTGW; Toon 1 .24 has no boiler.
- **Related**: diagnose report already lists every ttymxc holder (6c39643); #61.

### 63. CH pressure warning still shown for the "CV druk te laag 0,6" reporter
- **Status**: Open (2026-11-21), pick up next session.
- **History**: user report (2026-11-17) "CV druk te laag 0,6" with a boiler that reports no pressure. 86228f4 / 65386fc: hcb_rrd seed ages out after 6 h, but only once a live reading was seen. 3f472c4: the seed also ages out when happ_thermstat answers the BoilerChPressure query WITHOUT a value (>= 3 times; first one logged in full, format still unknown). The reporter still sees the warning (2026-11-20).
- **Unknown**: his FreeToon version (3f472c4 is in 1.0.12+); whether his happ answers the query at all, with a value, or valueless in a format the parser misses; where the 0.6 comes from (rrd seed, a live value, OTGW, the master via client_link).
- **Next steps**: ask his version and a 1.0.16 diagnose report (`http://<ip>/freetoon-diag.txt`; its toonui.log section includes the `[bxt]` pressure lines and the first valueless answer). Depending on the answer: fix the valueless-answer detection, or add a setting to hide the pressure / the low-pressure banner for boilers without a sensor (the "suppress switch" idea from 2026-11-17).

### 64. On/Off warm-air heater fires under the Eneco GUI but not under FreeToon: serial capture for 1.0.17
- **Status**: Done. Ketel-log shipped in 1.0.17 (#65). The reporter's case is explained (2026-11-24): the living-room master Toon has no boiler, the boiler is wired to his second Toon in a cold room (Thermostat+ "Local" under Eneco); solved by "Ketel via andere Toon" (#70, 1.0.20).
- **Built** (2026-11-21): `ttysniff/ttysniff.c` + `build.sh` (10 KB, glibc <= 2.21), `scripts/ketellog.sh` (start/stop/status/expire/note; drops the hook and restarts happ clean if it does not answer within 30 s with the tap), `freetoon_diag.sh` (status line per minute, expiry, report section), toonui `ketellog.c`: "Ketel-log" button on the Adapters screen (Keteladapter card; hidden without the tap) and a start/stop block on the :10081 /logs page (POST behind the web login; GET /ketellog public). install.sh installs/uninstalls both.
- **Verified on .16**: stock happ_thermstat sets the boiler UART to **4800 8E1** itself at every start (answers the parity question; quby_bridge's old 8N1 proxy explains "ketelmodule niet gevonden"). Tap reloads across ot_mode_switch / UI switches, header labels ui=qt-gui / ui=freetoon, self-stop at the deadline, hook removed by the diag loop, port 80 serves the log. ~2 KB/min without heat demand (~120 KB/h).
- **Toon 1 (parked, 2026-11-21, Toon 2 only for now)**: the tap builds with the Toon 1 toolchain (armv5 soft-float, only GLIBC_2.4; strip `.note.ABI-tag` like toonui) and already matches ttymxc1. Blocker: the Toon 1 `/etc/init.d/HCBv2` (checked on .24) has no `/tmp/etc-default-HCBv2` hook, it only sources `/etc/default/HCBv2` on flash. Plan if wanted: install.sh appends one guarded line `[ -f /tmp/etc-default-HCBv2 ] && . /tmp/etc-default-HCBv2` there (Toon 1 only, once, atomic + `sh -n` + backup; uninstall removes it); then ketellog.sh works unchanged. Avoid an inittab `ther` wrapper (HCBv2 rewrites its rows). Test on .24 (happ holds ttymxc1, no boiler).
- **Facts** (two diagnose reports, 2026-11-21): very old warm-air unit, genuinely On/Off (boiler_type = 1 is correct; the Eneco GUI says On/Off too). Direct mode, no OTGW, no qbri, nothing mounted over ttymxc0, happ_thermstat holds the port under both UIs. Under FreeToon at setpoint 24 the write is accepted (`HTTP roomSetpoint=2400 rc=0`), happ reports ControlSetpoint 90 and BoilerBurnerMinutes counts up every minute: the heat demand reaches happ_thermstat, so it gets lost below it (happ -> boiler module -> relay). FreeToon never sets termios on the port (no stty / tcsetattr anywhere), so 8N1/8E1 drift from our side is unlikely.
- **Correction**: 489579a blamed this report on an OpenTherm boiler flipped to On/Off. Wrong for this user; that fix is still right but does not solve his problem.
- **Suspect**: every switch to the Eneco GUI restarts happ_thermstat (`ot_mode_switch.sh off`), so "Eneco works" may really be "a freshly started happ_thermstat works". happ_thermstat was also restarted during the FreeToon session (pid newer than toonui) without a FreeToon log line explaining it. His FreeToon test with demand lasted only ~3 minutes.
- **Plan**:
  - `boxtalk_re/ttysniff.c` (LD_PRELOAD, passive, inside happ_thermstat only): one 7-byte frame per line instead of one line per byte; hook `tcsetattr` / `ioctl(TCSETS*)` to log baud/parity/stop bits; header per process start with the active UI (`/mnt/data/ui_choice`); self-stop at `TTYSNIFF_UNTIL` (epoch) and at a ~20 MB cap. Match any ttymxc happ opens (Toon 1 = ttymxc1, #62), but release for Toon 2 only.
  - Loading: stock `/etc/init.d/HCBv2` sources `/tmp/etc-default-HCBv2` (line 59, after `XTRAARG=$2`, before the exec) - verified on .16. Write `[ "$XTRAARG" = happ_thermstat ] && export LD_PRELOAD=/mnt/data/libttysniff.so TTYSNIFF_UNTIL=<epoch>` there, then restart happ_thermstat. RAM only, gone after a reboot; the restart by a UI switch reloads the tap, so one capture spans both UIs. (On .16 /tmp -> /var/tmp/; mind the sometimes-broken /tmp symlink.)
  - Start: button "Ketel-log starten (60 min)" in FreeToon (Settings -> Support and/or :10081/diag). Removes a stale hook past its deadline.
  - freetoon_diag.sh loop (runs under both UIs): every 60 s append time, UI, setpoint, room temp, burnerInfo, currentInternalBoilerSetpoint to the capture; report section with status + summary; serve the full log as `http://<toon>/freetoon-ketellog.txt` (symlink in /qmf/www like the report).
  - Test on .16 first, including the switch to Eneco and back (brief heating-control gap from the happ restart).
- **Test script for the reporter**: in FreeToon, Instellingen -> Adapters -> Keteladapter card -> "Ketel-log" (or the :10081/logs page); setpoint 2 °C above room, wait 10 min, note whether it fires; set back, switch to the Eneco GUI, repeat; send `freetoon-ketellog.txt` + `freetoon-diag.txt`. Compare the frames to the boiler module during demand.

### 65. Release 1.0.17: Homey, Verlichting tile chooser, Ketel-log
- **Status**: Published (2026-11-22, non-prerelease, tag on a9fc32c). Prepared in release_v1.0.17/ (built from the "chore(release): 1.0.17" commit, BUILD_VERSION=1.0.17, helpers copied from 1.0.16 as their sources are unchanged, index.* from web/build.sh, libttysniff.so from ttysniff/build.sh). To publish: push, tag 1.0.17 on the release commit, publish non-prerelease with all files incl. `libttysniff.so`.
- **Contents**: Homey Pro (2023) support (#28: Settings -> Homey and :10081/homey, devices hub + Homey screen, remote page Lampen card; locks and garage doors read-only); Verlichting tile chooser (long press; Domoticz, HA, Homey, Z-Wave; old choices kept until the first change) with a tip on the Apparaten screen; Ketel-log boiler serial capture for support (#64, Toon 2; 118918a, 763f989); client mode: boiler flow/return temperatures from the master (5e91605); classic home: accent border on the selected programme, programme switch in sync, Toon 1 outlines (e00f046); diagnose report lists every serial-line holder (Toon 1 boiler = ttymxc1, 6c39643).

### 66. Release 1.0.18: solar via the inverter, meter adapter solar, dim fixes
- **Status**: Published (2026-11-23, non-prerelease, tag 1.0.18 on 46751cf). Prepared in release_v1.0.18/ (built from the "chore(release): 1.0.18" commit, BUILD_VERSION=1.0.18, helpers and libttysniff.so copied from 1.0.17 as their sources are unchanged, index.* from web/build.sh). To publish: push freetoon-lvgl, tag 1.0.18 on the release commit, publish non-prerelease with all files; then push freetoon-integrations (a5378bc: the `inverters` catalog entry needs `min_toonui` 1.0.18, so the tag must be exactly 1.0.18).
- **Contents**: solar from the inverter (#60: core source `/api/v1/solar/source`, `/solar` web page with the form from the app's manifest; app "Omvormers" 0.1.0 for Enphase Envoy, Solis stick / SolisCloud, Omnik); meter adapter analog solar: "Zon nu" / "Verbruik nu" from the powerUsage dataset like the stock tiles (63a9318, not yet verified on a real analog sensor); dim water flow under the setpoint instead of through the room temperature (76ebf01); moon phase ~3.5 days late fixed, weather icon in a low dim block no longer cut in half (212ac20); clock tile moon icon and its setting removed (79b2014).

### 67. Verlichting tile make-over: up to 8 lights, sliders, Hue
- **Status**: Ships in 1.0.19 (#69); seen working on .16 by the user (2026-11-24).
- **Request**: more lights on the Verlichting tile (at least 4, configurable), sliders where possible. Decisions: max 8, sliders where they fit, Hue as a source.
- **Built**:
  - `tile_lights.c`: 8 places (`tile_light1..8`; 3..8 appended to `settings_t`, so the plugin ABI is unchanged; "auto" stays for 1 and 2 only). New sources `hu:<lamp>` and `hg:<room/zone>` (Hue). `tl_level()` / `tl_set_level()`: Domoticz dimmers (new `domoticz_set_level_async`), HA lights (new `dimmable` flag from `supported_color_modes`; poll buffer 768 -> 2560 bytes so it fits), Homey `dim` (switches on first), Hue lamps and groups. Z-Wave stays on/off. Pins from the Domoticz / HA / Homey screens fill the places the tile shows (`tl_set_capacity`).
  - Tile (`screen_home.c`): rows follow the tile size (2x2 = 2 as before, 4x4 = 5, a wide tile two columns, max 8); empty places leave no gap. A slider in the row when the name keeps ~120 px (not on Toon 1); otherwise the row shows the level ("70%") and a long press opens a dim popup (slider, on/off, "Lampen kiezen"). Geometry is only re-applied when it changes; idle redraw 0.00 screens/s with 5 and 8 lights (Toon 1 and 2).
  - Chooser (`screen_tile_lights.c`): 8 place chips, pick a place then tap a light (tap again clears it, a light moves when it was elsewhere); places the tile is too small for are greyed with a hint; "dimbaar" marks; Hue rooms/zones and lamps listed.
  - `ui_label_set_text_cached()` (ui_lazy.h): LONG_DOT labels hold the cut text with "...", so `ui_label_set_text()` re-set a cut name every tick (a redraw per second per long name; the old 2x2 tile had it too). 36 other LONG_DOT labels in the tree may have the same issue where they are refreshed periodically.
  - Remote page "pin", Homey home button and HA T-labels (T1..T8) know all 8 places. Sim: HA starts with `HA_SIM_TOKEN` + `enable_ha`/`ha_host` in the cfg.
- **Tested in the sim**: Hue mock (lamps, room, zone), synthetic HA mock (dimmable colour light, onoff light, switch: `brightness_pct` sent from the slider), `tests/homey_mock.py` (an off light dimmed: `onoff=true` then `dim`), chooser taps, dim popup on Toon 1, ASan/UBSan runs, device builds + ABI checks. Not tested: Domoticz Set Level against a real Domoticz, real Hue bridge, real devices.

### 68. Schakelschema: plugs and lights on/off at set times (successor of the QML app "Slimme stekkers")
- **Status**: Ships in 1.0.19 (#69). Tested on .16 by the user (2026-11-24): a lamp switched by a rule. Follow-ups in the same release: "Willekeurig +/-20 min" per rule (a8cdd8e, holidays; a fixed shift per rule and day), the QML import takes only Z-Wave uuids (f5a2626; .16 had "domoticzLamp TV" entries).
- **Request**: a function like (or better than) oepi-loepi's ToonStore app "Slimme stekkers" (plugSchedule): Z-Wave plugs (and Hue-driver switches) on/off at a time on chosen weekdays; it ran inside qt-gui, missed every moment while the UI was down, and sent each command twice. Decisions (user): every device FreeToon can switch, clock + sun times, Toon screen + web page, import the QML rules once.
- **Built**:
  - `switch_sched_core.c` (host test `make test-schedule`, 32 checks incl. Amsterdam sunrise/sunset, DST day, weekdays, catch-up, save/load, QML import): rule = device ref (tile_lights.h) + on/off + clock time or sunrise/sunset +-180 min + weekdays + active. Due = most recent occurrence not handled yet and at most 3 h old (a restart or power cut still switches afterwards); per device only the latest due occurrence is executed (restart at 09:00 after "on 07:00" + "off 08:00" gives "off"); a saved rule starts handled up to now. Stored in `/mnt/data/freetoon_schedule.cfg`.
  - `switch_schedule.c`: LVGL timer every 15 s on the master (client Toons show a note; the headless WASM host runs no schedule), every command sent again on the next tick; sun times for the weather Lat/Lon, else mid-NL; imports `/mnt/data/tsc/plugSchedule_userSettings.json` once (as `zw:<uuid>`; Hue-driver uuids from the QML app have no FreeToon equivalent and simply match nothing).
  - Explicit on/off per source: `ha_device_set_on_async`, `hue_light_set_on_async`, `zwave_set_switch_uuid` (new), Domoticz On/Off, Homey onoff, Hue groups, HomeWizard sockets; `tl_set_on_ref()`. `tl_enumerate()` lists every switchable device once for the tile chooser, the schedule picker and the web page; HomeWizard Energy Sockets (`hs:<ip>`) are a source now, also on the Verlichting tile.
  - Toon: Apparaten -> "Schakelschema" chip -> `screen_switch_schedule.c` (list with next action and active switches, editor with device picker, Aan/Uit, Tijd/Zonsopkomst/Zonsondergang, -/+ steps, weekday buttons). Idle redraw 0.00 screens/s.
  - Web: `:10081/schema` (behind the web login, POSTs same-origin only), linked from /logs.
  - New import `sincos` (sun calc): exported by libm on .24 and .16, binaries resolve with the device linker (`ld.so --verify`), ABI baselines refreshed.
- **Tested**: host tests; sim renders Toon 1 + 2; a due rule switched within one tick and was marked handled, an already handled one did not; a rule created through the screen taps; /schema add / edit / toggle / delete, cross-site 403, missing device / day / bad time refused; ASan/UBSan run over the screen. Not tested: switching real devices from a schedule on a Toon.

### 69. Release 1.0.19: Verlichting tile, Schakelschema, Zon statistics
- **Status**: Published (2026-11-24, non-prerelease, tag 1.0.19 on 4b412bc). Prepared in release_v1.0.19/ (built from the "chore(release): 1.0.19" commit, BUILD_VERSION=1.0.19, helpers and libttysniff.so copied from 1.0.18 as their sources are unchanged, index.* from web/build.sh). To publish: push, tag 1.0.19 on the release commit, publish non-prerelease with all files. Also push freetoon-integrations (4f8d649: Omvormers 0.1.1, Envoy over HTTPS when plain HTTP is closed; no core change needed).
- **Contents**: Verlichting tile up to 8 lights with sliders and Hue (#67); Schakelschema with sun times and "Willekeurig +/-20 min", on the Toon and on :10081/schema, one-time import of the "Slimme stekkers" app (#68); HomeWizard Energy Sockets as a light source; Zon statistics from every solar source, summed, and live solar power summed (#60); classic home: programme border also with the programme off; dim forecast: optional night temperature (Instellingen -> Scherm); Marketplace rows fit long descriptions; LONG_DOT labels no longer redrawn every second.

### 70. Ketel via andere Toon (Thermostat+ "Local" style)
- **Status**: Ships in 1.0.20 (#73). Tested on .16 (sender) -> .24 (boiler side) 2026-11-24: on 30.0, off 6.0, safety net 6.0 after 900 s; heartbeat peer check only in client mode (15ccc8e).
- **Why**: a user has the master Toon in the living room but the boiler is wired to a second Toon in a cold room. Under Eneco, Thermostat+ "Local" let the living-room Toon decide and the boiler Toon execute.
- **How** (boiler_remote.c): Instellingen -> "Ketel via Toon" on the living-room Toon: IP of the boiler Toon, on / off setpoint (default 30 / 6 °C). Every 10 s the own happ_thermstat burnerInfo (1 or 3 = heat demand) is read; on a change, and every 60 s, the boiler Toon's happ gets setSetpoint (on or off value) and its programme is switched off (changeSchemeState 0). Works with stock Eneco or FreeToon on the boiler Toon. Own happ silent 5 min: off setpoint sent.
- **Safety net**: when FreeToon runs on the boiler Toon, the sender also POSTs a heartbeat to its /api/v1/boiler/remote (private / loopback peers only; with a master_host set, only that peer). After 15 min without one, the boiler Toon sets its own happ to the off setpoint, once. Without FreeToon there the boiler stays on the last setpoint if the living-room Toon dies (the modal says so).
- **Tested**: e2e in the sim with two FreeToon instances and two mock happs (demand on / off / on, sender killed: boiler Toon fell back to 6.0 after the silence window); modal renders Toon 1 + 2 incl. live status. inet_ntop verified on .24 and .16 (ld.so --verify), baselines refreshed.

### 71. Home Assistant: lifetime solar total sensor for the Zon statistics
- **Status**: Ships in 1.0.20 (#73); sim-tested only.
- **What**: Instellingen -> Home Assistant -> "Zon totaal (kWh)" (settings.ha_solar_total_entity, appended at the end of settings_t; ha_state.solar_total_wh / have_solar_total appended to ha_state_t). Unit from unit_of_measurement: kWh / Wh / MWh, anything else refused with one log line; unknown / unavailable ignored. When set, solar_rrd counts its increase (SACC_LIFETIME, id "ha:total:<entity>") instead of integrating the W sensor, so gaps (restarts, HA sending no update for a steady value) lose no energy; the W sensor stays for the live tiles only (never counted twice). /solar page source shows "Home Assistant (totaal)".
- **Tested**: sim against a synthetic HA mock: kWh and MWh converted, W refused, unavailable ignored; modal renders Toon 1 + 2. The accumulator path itself is covered by tests/solar_acc_test.c; not tested on a Toon with a real HA.

### 72. Homey flows tile
- **Status**: Ships in 1.0.20 (#73); sim-tested against the Homey mock only.
- **What**: new home tile type LT_HOMEY_FLOWS ("Homey flows", min 2x1, Toon 1 + 2, layout editor palette). One button per chosen flow (homey_start_flow: flows with the "This flow is started" card); green check / red cross for 1.5 s after a tap. As many buttons as fit (min ~130 px wide, max 8); a one-row tile hides the title. Long press on the tile opens the chooser (tile_flows.c, sim screen "tileflows"): 8 places as chips, tap a flow to put it on the selected place, again to remove. settings.tile_flows (appended to settings_t): "auto" = the first flows Homey lists, else comma-separated flow ids.
- **Tested**: sim vs tests/homey_mock.py: 6x3 / 4x2 / 2x1 on Toon 1 + 2, tap triggered "Movie Evening" on the mock (POST .../trigger -> 204), long press opened the chooser. Toon 2 binary built, abi-check OK.

### 73. Release 1.0.20: Ketel via andere Toon, Homey flows tile, HA solar total
- **Status**: Published (2026-11-25, non-prerelease, tag 1.0.20 on af25a98). Prepared in release_v1.0.20/ (built from the "chore(release): 1.0.20" commit, BUILD_VERSION=1.0.20, helpers and libttysniff.so copied from 1.0.19 as their sources are unchanged, index.* from web/build.sh).
- **Contents**: Ketel via andere Toon (#70, Thermostat+ "Local" style, with safety net when the boiler Toon runs FreeToon); Homey flows home tile (#72); Home Assistant lifetime solar total sensor for the Zon statistics (#71).

### 74. Marketplace apps on the home page and the dim screen
- **Status**: Implemented (2026-11-25, f1f5f8e), tested on .16 by the user with the Verkeer app (home tile + dim block); ships in the next release.
- **Why**: a marketplace app (e.g. Verkeer) could only take the place of one of the four fixed tiles (Energie / Familie / Ventilatie / Water, long press) or go on page 2; the layout editor's "Vervang type" never listed apps, and the dim screen had no app block.
- **Home**: the "Integratie" tile type (LT_SLOT) is now allowed on page 1 of a custom layout, up to 4 (TILE_SLOT_HOME_0..3, settings.tile_slot_home[4], keys tile_slot_home_N); the k-th in layout order uses slot HOME_0 + k. Rendering shared with the page-2 slots (render_assignable_slot); tap opens the app, an unbound tile or a long press opens the chooser.
- **Dim**: new dim block DB_INTEG "Integratie" (default hidden) showing the app's title / value / subtitle; the app is TILE_SLOT_DIM (settings.tile_slot_dim), chosen in the dim layout editor: long press on the block -> "App kiezen" (apps only).
- **Sim**: screen modes now call tile_slots_init() (apps from /mnt/data/integrations); SIM_INTEG_VALUE="<id>|<value>|<subtitle>" stands in for an app's tile post.

### 75. Hourly forecast: wind arrow too close to the wind direction text
- **Status**: Fixed (2026-11-25), sim-checked on the forecast cards (Toon 1 + 2); the per-day hourly table has no hourly data in the sim, check on .16. The same overlap was in the forecast screen's hour cards (label at 20 / 26 px, arrow 32 px): both now start the text after the arrow's width plus a gap.
- **What**: on the hourly forecast of a day (screen_weather_hourly.c) the rotated wind arrow sits at `col_wind_x` and the "ZW 4 Bft" label at `col_wind_x + SX(28)` (line ~237): the arrow (rotated around its centre, so wider than its 24 px when diagonal) touches the text. Give the label more room (e.g. arrow width + SX(10), measured from the image) on Toon 1 and Toon 2.

### 76. Dim forecast strip: night temperature drops out per day when the strip is narrow
- **Status**: Fixed (2026-11-25): dim_fc_paint_strip() decides for the whole strip (big font, else smaller font, else day temperature only on all days), using the column's real room (the label width has a floor). Sim-checked with the photo's temperatures at 9 / 7 / 5 columns, grid + classic dim, Toon 1 + 2. SIM_FC_WINTER=1 gives those temperatures.
- **What**: with Instellingen > Scherm "Ook nachttemperatuur" each day of the 5-day strip shows "max°/min°" only if that text fits its column (dim_fc_paint in screen_dim.c, `both = sz.x <= width`); otherwise just "max°". In a narrow strip some days show the night temperature and others not (photo: "2°/-1°  1°/-3°  0°  2°  5°/-1°"), which looks like missing data. Make it one decision for the whole strip: if any day doesn't fit, first step the temperature font down for all days, and only if it still doesn't fit drop the night temperature on all days.

### 77. ToonWater (oepi-loepi) water meter needed the HomeWizard water switch
- **Status**: Fixed (2026-11-25), sim-tested (watertest against a fake sensor), all targets build.
- **Report**: water tile empty with the oepi-loepi water meter, its IP filled in under Integraties; works under the stock GUI.
- **Cause**: the ToonWater row has only an address field; reading it (and the water tile) also required `enable_p1_water`, the switch on the "Watermeter (HomeWizard)" row. Filling in the address switched it on only when the address changed, so a switch turned off later (no HomeWizard) left the water meter off with no obvious way back. Workaround on 1.0.20: switch on "Watermeter (HomeWizard)" (its IP may stay empty).
- **Fix**: `water_enabled()` = the HomeWizard switch or a ToonWater address; used by the poller start, the ToonWater poll, the water tiles (home, classic home) and the tile-slot check. The address field strips "http://" / "https://" and any path ("/water.html") and shows what it kept. ToonWater problems are logged once per kind with the reason (no answer / reply without "waterquantity" + its start). Sim watertest no longer switches the meter on itself.

### 78. /logs: the Marketplace apps' logs
- **Status**: Fixed (2026-11-25), sim-tested (webserve with fake logs).
- **What**: /logs listed only toonui.log(.1), ui_launcher.log and x11vnc.log, so `integration-<id>.log` (each app daemon's output, e.g. integration-inverters.log) and integration_install.log could only be fetched over SSH - support texts had told users to get them from /logs. Now a section "Apps uit de Marketplace" lists every /var/volatile/tmp/integration*.log, and /log?file= serves them (name checked: integration*, .log, [a-z0-9-_.], no ".."). The app daemons were checked not to log secrets (tokens / passwords only go into request buffers); /logs is public like the other logs.

### 79. A date and time on every log line (toonui.log and the app logs)
- **Status**: Implemented (2026-11-25), on .16 (core + script, apps rewired); host-tested logstamp; Toon 1 binary checked with ld.so --verify only.
- **toonui.log**: logstamp.c, started first thing in main(): fd 1 and 2 go to a pipe, a thread writes each line to the original log with "YYYY-MM-DD HH:MM:SS " in front (children such as sh / curl included; lines that already start with a date kept). Write end non-blocking (a stalled thread drops lines instead of hanging toonui); at exit it waits up to 0.5 s for queued lines; on SIGSEGV/BUS/ILL/FPE/ABRT the handler copies what is queued (unstamped) before the crash goes on. FT_NO_LOGSTAMP=1 switches it off. Not in the sim (tests grep its output).
- **App logs**: the inittab row of an app is now `/bin/sh /mnt/data/integrations-install.sh run <id>`, which runs the app's binary and stamps each line (date per line; own dates kept). Short on purpose: sysvinit refuses a process field over 127 characters ("process field too long") - a first version with the binary and log path in the row left every app stopped on .16 until fixed. Rows are found for drop / kill by their own dir (old form) or "integrations-install.sh run <id>" (new form). `rewire <id>` turns an installed app's row into the new form (no-op when it already is); install.sh runs it for every installed app on a FreeToon update.
- **ABI**: dup, signal, __cxa_atexit (glibc 2.4) added to both baselines after ld.so --verify on .24 and .16.

### 80. User feedback: network message, teruglevering, Energie tile, inverter "Geen verbinding"
- **Status**: Implemented (2026-11-25); sim-checked (stats Toon 1 + 2 with a fake hcb_rrd, home tile with and without dynamic prices), core build running on .16; Omvormers 0.2.4 e2e-tested with a synthetic Envoy.
- **"Toon is niet verbonden met internet, bekijk hier de oplossingen"**: not FreeToon's own text - it is hcb_netcon's notice (stock network service, "notification-no-connection-to-internet / -network"), posted to happ_usermsg after a few failed pings and deleted when they work again; FreeToon only mirrored it in the Inbox. inbox.c now keeps the full list and holds hcb_netcon's internet / router notices (all firmware languages) back until they have been there 10 min (INBOX_NETCON_HOLD_S) and FreeToon's own internet check fails too; re-checked every healthcheck round. The healthcheck's own "X niet bereikbaar" messages (incl. "Geen internet (DNS niet bereikbaar)") now need 3 failed probes in a row (~3 min) instead of one.
- **Teruglevering**: the Elektriciteit tab shows the P1 return delivery (elec_quantity_nt/lt_produ; "Nu" elec_produ_flow) as green bars below the zero line, the total under the rates, and "terug" in the bar tooltip; not in the euro view. lv_chart draws bars from its bottom edge, so the draw hook moves them to the zero line and draws the export under each bar. The Zon tab says where its numbers come from: "opwek (zonnemeter of omvormer)" or "teruglevering P1 (geen zonnemeter of omvormer)" (the old silent fallback). Bucketing moved into bucket_series(); export matched to the import bars in order (a 12-month view has the same month twice).
- **Energie tile**: the NILM step label ("~120 W AAN", 10 s after a jump) is off by default (Instellingen > Scherm "Energie-tegel: sprong in verbruik tonen", settings.energy_nilm_label, appended to settings_t) and sits next to the title when on; the dynamic price reads "€ 0,23 per kWh" right under the watts (was a bare "€ 0,23" bottom right next to "Gas 1,23 m3", read as money spent); gas always "Gas vandaag".
- **Omvormer tile "Geen verbinding"** (integrations, Omvormers 0.2.4): when the last good reading is < 20 h old and was low (< 150 W) or it is 17:00-08:00, a failed read shows "0 W / Slaapt, vandaag x kWh" (inverters and their sticks switch off without sun); other failures show the short reason (time-out, TLS-fout, ...). The core still gets "no reading".
- **Water tile**: no change; it shows "--" until the first reading, and the spinning circle means water is flowing (not loading).
- **Meterstanden + solar total**: not done (not a meter reading); the Zon tab source line covers the confusion.

### 81. Release 1.0.21: home/dim app tiles, boiler modulation, teruglevering, log timestamps
- **Status**: Prepared (2026-11-25) in release_v1.0.21/ (built from the "chore(release): 1.0.21" commit, BUILD_VERSION=1.0.21, helpers and libttysniff.so copied from 1.0.20 as their sources are unchanged, index.* from web/build.sh). To publish: push, tag 1.0.21 on the release commit, publish non-prerelease with all files.
- **Contents**: Marketplace apps as a home tile (page 1) and dim block (#74); boiler modulation on the thermostat screen and in the dim Ketel block; hourly forecast wind arrow spacing (#75); dim forecast night temperature decided per strip (#76); ToonWater works without the HomeWizard water switch (#77); /logs lists the Marketplace apps' logs (#78); date and time on every log line, toonui.log and app logs (#79); user feedback (#80): stock "niet verbonden met internet" notice held back unless the outage is real, healthcheck messages after 3 failed probes, teruglevering below the zero line in the Elektriciteit chart, Zon tab names its source, clearer Energie tile (price "per kWh", NILM label opt-in).

### 82. SolarEdge via the cloud (Monitoring API v2, OAuth "Site Access")
- **Status**: Implemented (2026-11-26); e2e-tested with a synthetic SolarEdge API (sim webserve + app host build); core on .16. Not yet tried against a real SolarEdge account. Needs core 1.0.22 (catalog min_toonui for Omvormers 0.3.0).
- **Why**: a user got a client ID + secret from SolarEdge. Since November 2026 the old v1 API (one site API key) is off; v2 for homeowners is OAuth (authorization code) with a Site Access app on developer.solaredge.com. Local Modbus TCP (SunSpec, port 1502) stays the advice when possible: live and without credits.
- **Core**: generic, for any app's settings form (app_settings.c): field type "link" (a button to an https:// URL built from the stored values; disabled until they are filled in) and templates `{REDIRECT}` / `{KEY}` in defaults, help and link URLs. `{REDIRECT}` = "http://<Host header>/solar/callback". GET /solar/callback hands its query (read from the request line, not the 255-char path) to the solar app as file `oauth_callback` in its directory and redirects to /solar?linked; the core keeps no secrets. abi: strpbrk (glibc 2.4) added after ld.so --verify on .24 / .16.
- **App** (Omvormers 0.3.0, type solaredge_cloud): fields client ID, secret, Redirect URI (default {REDIRECT}), "Koppelen met SolarEdge" (connect.solaredge.com/authorize with client_id, scope SITE_DATA DEVICE_DATA, access_duration 24, external_id, redirect_uri), and a paste field for the address after approval when the redirect to the Toon does not work. Token exchange / refresh: JSON POST to monitoringapi.solaredge.com/v2/oauth2/token with client id + secret in the body (retry with redirect_uri on a 4xx); the refresh token rotates and is stored each time (solaredge.state); a code is used once. Data: /sites/{id}/power (QUARTER_HOUR, W, last non-null value) every 30 min and /sites/{id}/lifetime-energy (unit-scaled) every 2 h, 06:00-22:00 only (~1500 calls/month of the ~2000 free); today from the lifetime counter. HTTP 429 with "credit" pauses until the 1st, other 429 5 min; 401 drops the access token; refresh 400/401 asks to link again.
- **Sources** (official docs are JS-rendered and could not be read): Home Assistant solaredge_v2 (aarondrabeck/ha-solaredge-api-connector), Domoticz PR #7028, ecoflow PR #4. Unverified: whether SolarEdge accepts a plain http LAN Redirect URI (HA reports a local http address working), exact token lifetime.
