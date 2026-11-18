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
- **Status**: Prepared (2026-11-12); tag 1.0.8 to be created on GitHub.
- **Contents**:
  - HomeWizard: IP fields on the Toon, Energy Sockets (Devices screen, Hue/Domoticz "Stekkers" button, client mirror), kWh meter as solar source, offer to use the P1 as energy source (#37).
  - Remote-control web page on :10081; client mode signs in on a master with a web password; ToonWater sensor as water meter.
  - Daily marketplace app-update check with home banner, "Alles updaten" and optional nightly app updates (#35).
  - Weather from Buienradar only; radar buttons off the timestamp.
  - Settings that apply without a restart (#36); Meterstanden from the meter's registers (#38); FreeToon notifications with text, TSC notice hidden (#39, #40).

### 42. Release 1.0.9: HA Lights on Tile and Web Page, Gas Today, Water Statistics, Meter Readings After Restart
- **Status**: Prepared (2026-11-13); tag 1.0.9 to be created on GitHub.
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
- **Status**: Open (user request, 2026-11-13).
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
- **Status**: Open (user request, 2026-11-15).
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
- **Status**: Stage 1 implemented 2026-11-17 (hub + swipe/status-bar entry, handle removed), sim-tested, awaiting on-device test. Stages 2-3 open.
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
- **Status**: Open (user request, 2026-11-17).
- **Inspiration**: ReToon app Clock Studio by Nomzi Design (https://github.com/Vibestar/ReToon-apps/tree/clock-studio-duo, `clock-studio/`). That app is GPL-2.0-or-later and FreeToon is MIT, so this is a **native LVGL implementation written from scratch** — use the app only for the feature list, copy no code or assets.
- **Scope** (no world-map face — user decision):
  - Faces: classic analog, minimal analog, square analog (small/large), LED digital (chamfered segments), stacked (HH over MM), calendar (clock + upcoming appointments from FreeToon's existing calendar data: HA calendar / iCal feeds), auto (picks a face that fits the cell; small cells fall back to simpler faces).
  - Themes: blue, purple, white, red, green + per-tile colour overrides (background, foreground, accent, hour / minute / second hand).
  - Per-tile settings: face, theme, colours, seconds (second hand / blinking colon), date on/off, title, 12/24 h, scale (65–100 %), corner radius (0 = square).
  - Available as a home grid tile (layout editor) and as a dim block (dim layout editor), each with its own settings; tap on the home tile opens a full-screen view with the same settings.
- **Implementation notes**: retained objects, update only what changes each second (hands via rotation / line points, LED segments recoloured, not rebuilt) — keep Toon 1 CPU low (see ui_lazy.h). Settings per tile stored with the layout entry. Existing clock tile / dim clock (incl. moon phase) stay; Klok Studio is an additional tile type.
- **Later (separate)**: option B — a ReToon app host on QuickJS (ReToon app API levels 1–10) so ReToon apps run unchanged as separate downloads; large and a moving target, not planned.
### 56. Colour themes (Nacht / Licht / Warm / Zwart) and a quiet-luxury design pass
- **Status**: Implemented 2026-11-18 on branch `feat/themas` (fork), sim-tested on Toon 1 and Toon 2 and on a real Toon 2; not yet upstream.
- **What**: palette table (`theme.h` / `theme.c`) replacing the light/dark ternaries; ~600 fixed colours converted to tokens; theme picker in Settings > Display, `ui_theme` in cfg and web API; dim screen, bootpicker, layout editors and the classic Toon look follow the theme; LVGL widgets take the accent. Design: Open Sans Light numerals, small tracked labels, no accent bars or borders, one pill style, 8 px grid, WCAG AA text contrast. Style guide for screens and integrations: `docs/DESIGN.md`.
- **ABI**: `ui_theme` appended to `settings_t` (offsets unchanged); `home_light_mode` = 1 only for Licht; palette reached by plugins through the weak `ft_theme_cur`, `ft_palette_t` append-only with a `size` field.
- **Fixed on the way**: dim-screen fan over the temperature, bar captions colliding, update banner hidden behind tiles, English date on the thermostat screen, crash when Settings was rebuilt while not on the screen stack.
- **Open**: dim grid (opt-in) and the remaining detail screens only got colours, not the full typography pass; Toon 1 not tested on hardware.
