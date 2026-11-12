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

---

## Open / Backlog

### 12. Doorbell Live Snapshot/Feed Investigation ('feed unavailable')
- **Status**: Open / Investigation needed.
- **Description**:
  - After pressing the physical doorbell, the Toon correctly opens the doorbell popup via WebSocket trigger, but displays *"Doorbell: snapshot unavailable"*.
- **Investigation Points**:
  - **Camera Proxy vs. Event Image**: Battery-powered Eufy doorbells remain in sleep mode; check if `camera.doorbell` provides a snapshot immediately or if `image.doorbell_event_image` holds the captured doorbell press frame.
  - **HTTP Response Verification**: Check what `/usr/bin/curl` actually downloads from `http://<ha>:8123/api/camera_proxy/camera.doorbell` to `/tmp/doorbell.jpg` (HTTP status code, 401/404/500 JSON vs valid JPEG header `FF D8 FF`).
  - **Stream/Snapshot Wake-up Latency**: Determine if a short delay or wake-up call (or MJPEG stream via go2rtc/RTSP) is needed for battery-powered cameras.

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

### 28. Homey Integration
- **Status**: Requester has a Homey Pro (2023) -> local API key route confirmed; waiting for sample API data (no Homey available to the developer; 2026-11-10).
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
- **Status**: Open (2026-11-10).
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
- **Status**: Implemented (2026-11-12): app_updates.c checks the catalog once a day alongside the FreeToon check (and on "Controleer"). The home banner shows "N app-updates" when there's no FreeToon update (tap = Marketplace; stays hidden until the set of updates changes), the Settings Marketplace tile shows the count, and the Update modal has an opt-in "Apps ook automatisch" at the auto-update hour (UI restart afterwards). Optional catalog field `min_toonui` gates updates/installs for the plugin ABI. Not built: an "update all" button in the Marketplace screen.
- **Description**:
  - FreeToon checks for its own updates (update_check.c: GitHub releases every 6 h, update banner, optional nightly auto-update), but marketplace apps are only compared with the catalog when the Marketplace screen is opened. A user never hears that e.g. Sonos 1.0.9 is out unless they go looking.
- **Approach**:
  - Once a day (alongside the FreeToon check), fetch the catalog (`catalog/index.json`, with the `?t=` cache-buster) and compare each installed integration's manifest version with the catalog version.
  - Show it the same way as FreeToon updates (banner / badge on the Marketplace tile, "N app updates"); optionally an "update all" and an opt-in automatic update at the nightly auto-update hour.
  - Reuse the existing compare/sort logic of the Marketplace screen; respect the plugin ABI gate (an update that needs a newer toonui must not be offered).

### 36. Settings That Only Take Effect After a Restart
- **Status**: Open (2026-11-11), after 1.0.7.
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
  - Not done: the hw_* fields in the web settings API (pwa_server.c); sockets on the Domoticz/Hue lights screens (those users reach the HA Devices screen only when neither is configured); a client Toon doesn't mirror sockets.
- **Description**:
  - A user reports that the HomeWizard water meter, socket plugs, P1 meter and kWh meters don't work. P1 (HWE-P1) and water meter (HWE-WTR) are supported, but their IP addresses (`p1_elec_host` / `p1_water_host`) can only be set in `/mnt/data/toonui.cfg` over SSH: there is no field on the Toon. Energy Sockets (HWE-SKT) and kWh meters (HWE-KWH1/3, SDM230/630) are not supported at all.
  - Workaround until then: "Lokale API" on per meter in the HomeWizard Energy app + a fixed IP in the router; the two hosts in toonui.cfg; Settings > Integraties: energy source "HomeWizard P1", switch on HomeWizard P1 and Watermeter, restart the UI.
- **Approach**:
  - Settings > Integraties: IP fields for the P1 and the water meter, like the ToonWater field (commit-on-defocus, host characters only), plus a short hint about "Lokale API".
  - Energy Sockets: power on a tile, on/off via the local API (`/api/v1/state`, PUT `{"power_on":…}`); kWh meters: power like the P1. Mind the ESP socket-table limit (poll slowly, see hw_thread).
  - HomeWizard v2 API: newer firmware has a local API v2 (HTTPS + bearer token, obtained by pressing the device button; README already mentions a v2 `P1_TOKEN` for p1bridge). Check whether v1 keeps working on current firmware or whether v2 should become the default, and how a user pairs on the Toon.

### 38. Meterstanden Pop-up Shows Readings That Match Neither the Old Nor the New Meter (User Report)
- **Status**: Open (user report, 2026-11-12).
- **Description**:
  - A user's "Meterstanden" pop-up (#29) shows ~17,000 and ~14,000 kWh for electricity and ~13,000 m3 for gas. Their meter was replaced a week ago; the old meter read ~47,000 / ~40,000 kWh and ~41,000 m3, and the new meter is still close to zero. So the pop-up shows neither.
- **Cause (hypothesis)**:
  - `on_meter_open()` in `screen_stats.c` shows the latest sample of the hcb_rrd cumulative loggers (`elec_quantity_nt` / `elec_quantity_lt` / `gas_quantity`). These are hcb_rrd's own running totals, not the meter's register. They probably started counting when the Toon (or the logger) was set up, so they never matched the meter. After a meter swap they keep going instead of dropping to the new meter's value.
- **Investigation Points**:
  - Where the stock QML UI gets its Meterstanden: probably the live P1 dataset from meteradapter / `hdrv_p1` (the raw DSMR registers 1.8.1 / 1.8.2 / gas). If so, read that instead of the rrd loggers.
  - Compare on a test Toon: rrd latest sample vs the P1 telegram / meteradapter values vs the physical meter.
  - With HomeWizard / ESPHome P1 as the energy source, use that device's `total_power_import_t1/t2_kwh` / `total_gas_m3` (actual meter registers).
  - If no meter-register source is available, hide the pop-up or label the values as "counted by Toon" rather than showing them as meter readings.

### 39. Suppress the TSC "Resource File Update" Inbox Message
- **Status**: Open (user request, 2026-11-12).
- **Description**:
  - The inbox shows "Er is een TSC resource file update beschikbaar. Update via TSC menu aub". The TSC script sends it for the stock QML UI's TSC menu, which FreeToon doesn't have, so the user can't act on it.
- **Approach**:
  - `inbox.c` already drops the "Service Center" notifications in `inbox_parse_dataset()` (CR2 filter). Add the TSC resource-file message there, matching on its type/subType if it has a stable one, otherwise on "TSC resource file". Also consider sending a DeleteNotification so it doesn't stay in happ_usermsg.

### 40. Empty Inbox Message "telegram / imported" on Both Toons
- **Status**: Open (user report, 2026-11-12).
- **Description**:
  - Both Toons show an inbox message with the header "telegram / imported" and no text. It comes from `telegram.c` (ToonBot settings import, `notify_show("telegram", "imported", ...)`).
- **Cause (hypothesis)**:
  - `notify_show()` (notify.c) sends the message as `<plainText>`, but `inbox_parse_dataset()` reads `<text>`. happ_usermsg probably fills `<text>` only for types it knows, so our own notifications arrive without a body. Check the other `notify_show` users too ("update"/"freetoon", "telegram"/"pending").
- **Approach**:
  - Dump the happ_usermsg dataset for this notification to see which field carries the text; fall back to `plainText` in the inbox parser when `text` is empty.
  - Decide whether a one-time "settings imported" note should go to the inbox at all, or only be a banner/log line.
