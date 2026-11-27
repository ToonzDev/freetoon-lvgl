#ifndef TOON_SETTINGS_H
#define TOON_SETTINGS_H

typedef struct {
    int auto_dim_enabled;     /* 0/1 — switch to ambient screen after idle */
    int auto_dim_seconds;     /* 5..300 — idle timeout in seconds */
    int auto_home_enabled;    /* 0/1 — return to the home screen after idle */
    int auto_home_seconds;    /* 5..600 — idle timeout before auto-returning home */
    int active_brightness;    /* 0..1000 backlight when active */
    int dim_brightness;       /* 0..1000 backlight while dimmed */
    int dim_sleep_away_black; /* 0/1 — completely black screen when Slapen or Weg */
    int auto_brightness;      /* 0/1 — follow the LTR-303 ambient sensor (Toon 2) */

    /* Toon 1 TSC2007 touch panel alignment. Resistive panels are mounted any-
     * which-way; these three booleans cover all 8 orientations. Tweak in
     * /mnt/data/toonui.cfg and restart toonui. Ignored on Toon 2 (eGalax cap
     * touch is already pixel-aligned). */
    int touch_swap_xy;        /* 0/1 — swap the X and Y axes (90/270° rotation) */
    int touch_invert_x;       /* 0/1 — mirror horizontally */
    int touch_invert_y;       /* 0/1 — mirror vertically */
    int _reserved_temp_offset;/* ABI padding: preserves struct layout for marketplace plugins (was temp_offset_centi) */
    int show_dim_weather;     /* 0/1 — show today's weather icon on the dim screen */
    int show_dim_waste;       /* 0/1 — show next-pickup on the dim screen */
    int show_dim_bars;        /* 0/1 — usage bars (energy now / gas hourly) flanking the dim clock */
    int show_dim_metrics;     /* 0/1 — TVOC/eCO2/CH-pressure row on the dim screen */
    int dim_custom_enabled;   /* 0 = original dim layout (default); 1 = custom grid (dim editor) */
    int language;             /* 0=NL, 1=EN */
    int dim_bars_swap;        /* 0..3: bar placement on dim screen:
                                 0 = Gas left, Electricity right (act + day) [default]
                                 1 = Electricity left (act + day), Gas right
                                 2 = Day totals left (gas + elec day), Electricity act right
                                 3 = Electricity act left, Day totals right (gas + elec day) */
    int dim_bars_vertical;    /* 0 = horizontal (default); 1 = vertical segmented (classic Toon style) */
    /* (dim_elec_max_kwh lives at the END of this struct — see there.) */
    int dim_waste_lead_days;  /* 0..7 — only show if pickup is within this many days
                                 (0 disables; 1 = only on pickup day; 2 = day before + day of) */
    int dim_waste_hour;       /* 0..23 — alert window start/end hour (default 18 = 18:00) */
    char waste_postcode[12];  /* e.g. "1671AD" — overrides the TSC waste config when set */
    char waste_housenr[8];    /* house number, e.g. "14" */
    int  waste_provider;      /* 0 = HVC (postcode), 1 = generic ICS calendar URL */
    char waste_ics_url[256];  /* full .ics calendar URL for provider 1 (prezero/cyclus/dar/…) */
    /* ToonSoftwareCollective provider plugin (run by the wastefetch helper) — the
       full stock-app mimic. "" = none (use ics_url / legacy). */
    char waste_plugin[8];     /* provider id from plugin_index.json, e.g. "6", "33" */
    char waste_icsid[48];     /* calendar number for ICSId-based providers (HVC etc.) */
    char waste_street[48];    /* for street-based providers */
    char waste_city[48];
    int  vnc_enabled;         /* 0/1 — run the x11vnc remote-control server */
    char vnc_pass[16];        /* VNC password (plaintext, max 8 effective chars;
                                 empty = no password). No spaces. */
    char weather_location[32];   /* Free-text location name shown in the UI
                                    (default "Medemblik"). Cosmetic for now. */
    char weather_lat[16];        /* Latitude (e.g. "52.01") */
    char weather_lon[16];        /* Longitude (e.g. "4.55") */
    int  weather_location_id;    /* Buienradar wikiCode for hourly-forecast
                                    fetch — default 6249 ≈ Berkhout (KNMI
                                    station closest to Medemblik). 0 disables
                                    the hourly fetch. */
    int  forecast_mode;          /* 0 = auto (hourly if available, else daily)
                                    1 = force hourly
                                    2 = force daily */
    int  weather_use_feel;       /* 0 = real temperature (default), 1 = feel temperature */
    /* OT bridge / OTGW configuration, matching ot_mode_switch.sh:
         "off"      — "Direct" in the UI: happ_thermstat drives the
                      keteladapter on /dev/ttymxc0 itself, no bridge.
                      Original Toon behaviour. Default.
         "wireless" — quby_bridge in active mode + OTGW GW=2: bridge
                      fakes Quby responses to happ and forwards CS/CH/
                      etc to OTGW via port 25238. Keteladapter not in
                      loop. Use as fallback if keteladapter dies.
       The former "proxy" mode was removed (it broke OpenTherm for some
       boiler modules); "proxy"/"keteladapter" load as "off", legacy
       "otgw" as "wireless". */
    char ot_bridge_mode[16];     /* "off" | "wireless" — default off */
    char otgw_host[64];          /* "192.168.99.21" — IP or hostname (no scheme) */
    char otgw_user[32];          /* HTTP Basic-Auth user, empty = no auth */
    char otgw_pass[64];          /* HTTP Basic-Auth pass */

    /* MQTT broker for the banner subscriber. Empty host disables subscriber.
     * Topics: filter strings the subscriber listens on; each new PUBLISH
     * triggers a banner with topic+payload. Cap of 8 keeps the settings
     * file small and the topic-select UI digestible. */
    int  mqtt_enabled;           /* 0/1 — run the MQTT banner subscriber */
    char mqtt_host[64];
    int  mqtt_port;              /* 1883 default */
    char mqtt_user[32];
    char mqtt_pass[64];
    char mqtt_topics[8][96];
    int  mqtt_topic_count;

    /* Integration toggles — runtime on/off for optional add-ons.
     * Default is 0 ("basic" install). On first boot (no toonui.cfg key),
     * settings_load() auto-enables a flag if its config file is present:
     *   enable_p1_elec   ← /mnt/data/p1bridge.conf exists
     *   enable_p1_water  ← /mnt/data/p1bridge.conf has a .115 line
     *   enable_vent      ← /mnt/data/vent.conf exists (non-empty)
     *   enable_ha        ← /mnt/data/ha.cfg exists (non-empty)
     * After first save the cfg keys are authoritative — user flips in
     * Settings → Integrations and toonui restarts to apply. */
    int enable_p1_elec;
    int enable_p1_water;
    int enable_vent;
    int enable_ha;

    /* Domoticz — alternative to HA for lights + blinds (for users who run
     * Domoticz). Talks to its JSON API at domoticz_host ("ip:port"); optional
     * basic-auth user/pass. */
    int  enable_domoticz;
    char domoticz_host[64];
    char domoticz_user[32];
    char domoticz_pass[48];
    int  domoticz_tile_idx1;   /* device idx pinned to tile switch 1 (-1 = none) */
    int  domoticz_tile_idx2;   /* device idx pinned to tile switch 2 (-1 = none) */
    int  reserved_clock_moon;  /* unused (was clock_moon_phase); kept for the plugin ABI */

    /* Philips Hue Bridge (REST API on LAN, port 80) */
    int  hue_enabled;
    char hue_host[64];
    char hue_user[64];

    /* Sonos HTTP API integration (node-sonos-http-api) */
    int  sonos_enabled;
    char sonos_host[64];
    int  sonos_port;
    char sonos_zone[48];

    /* Z-Wave control — gates the Settings → Z-Wave management screen's
     * write actions (include/exclude/on-off/rename). Default 0. Flipping it
     * on also writes <supportControl>1</supportControl> into the stock
     * config_hdrv_zwave.xml and restarts hdrv_zwave so the built-in
     * controller's advanced control is genuinely enabled. See
     * reference_toon_zwave_api memory for the HTTP API. */
    int enable_zwave;

    /* Energy data source for the Energy tile.
     *   0 = meteradapter — the Toon's own built-in smart-meter reading via
     *       happ_pwrusage (the official/stock path). Default.
     *   1 = p1 — HomeWizard P1 over the LAN (the homewizard.c poller).
     *   2 = p1esp — ESPHome P1 reader, e.g. a Zuidwijk SlimmeLezer (p1esphome.c).
     *   3 = ha — Home Assistant sensor entities (energy_* below).
     * Per-device: a Toon with a working meteradapter uses 0; a setup that
     * reads the meter via a HomeWizard P1 instead picks 1. */
    int energy_source;
    char p1esp_host[64];        /* ESPHome P1 reader — ip, host or host:port */

    /* HA sensor entities for energy_source == 3. Power is watts, gas m³. */
    char ha_power_entity[64];
    char ha_gas_entity[64];
    /* Solar production. This is deliberately NOT part of energy_source: a P1
     * meter can only see what crosses the meter (i.e. net export), never gross
     * panel production — the house consumes some of its own solar directly. So
     * the solar tile always reads an HA inverter/production sensor (watts), and
     * shows "--" when it isn't configured, rather than quietly mislabelling
     * export as yield. */
    char ha_solar_entity[64];

    /* Energy statistics rates and display mode */
    float rate_elec_kwh;         /* Electricity rate in € per kWh (default 0.30) */
    float rate_gas_m3;           /* Gas rate in € per m3 (default 1.35) */
    int   stats_show_euro;       /* 0 = physical UoM (W, kWh, m3), 1 = Euro (€) */

    /* Boot-picker — when 1, the launcher-spawned `toonui --bootpick`
     * shows a 10 s "freetoon vs stock qt-gui" picker before dispatching.
     * When 0 the picker short-circuits and the launcher boots straight
     * into whatever /mnt/data/ui_choice selects. Default ON so a new
     * user can always escape to the stock UI by tapping a button. */
    int boot_picker_enabled;

    /* Update check — poll GitHub Releases every 6 h, banner on the
     * home tile when a newer freetoon-lvgl release is available.
     * Default 1; flip to 0 in toonui.cfg to disable the background
     * fetch entirely (no network call, no banner). */
    int update_check_enabled;
    /* Update channel: 1 = beta/dev (newest release incl. prereleases, default),
     * 0 = stable/official only (/releases/latest, skips prereleases). */
    int update_channel;

    /* Hide offline tiles on the home screen. Default 0 → tiles for
     * disabled / disconnected integrations stay visible with an
     * "offline" placeholder ("P1 offline" / "HA offline" / "Itho
     * offline"). 1 → those tiles get LV_OBJ_FLAG_HIDDEN so the home
     * screen drops the corresponding rectangles entirely. The user
     * picks one or the other in Settings → Integrations. */
    int hide_offline_tiles;

    /* Tile-reassignment slots (Phase 2 of marketplace). Each holds the id
     * of an installed integration whose tile should replace the matching
     * right-column home tile. Empty = built-in behaviour. See
     * tile_slots.h for the dispatch model. 48 chars matches INTEG_ID_MAX. */
    char tile_slot_energy[48];
    char tile_slot_family[48];
    char tile_slot_vent[48];
    char tile_slot_water[48];
    /* Page-2 (swipe) tile slots — 4 independent assignable positions that
     * render whatever marketplace integration you bind to them. Empty = the
     * "tap to assign" placeholder. */
    char tile_slot_page1[12][48];   /* page-2 slot bindings; must match TILE_SLOT_P1_N */

    /* Auto-rotate: one tile position cycles its content through a chosen set
     * of installed integrations every N seconds. rotate_members is a
     * comma-separated list of integration ids. */
    int  tile_rotate_enabled;
    int  tile_rotate_seconds;          /* 3..120 */
    char tile_rotate_members[256];     /* "id1,id2,id3" */

    /* Home Assistant — host:port (no scheme), empty = HA disabled. The two
     * presence entities + display names drive the Family tile (e.g. Life360
     * device_trackers). All empty by default so nothing personal ships. */
    char ha_host[64];
    char life360_a_entity[64];
    char life360_a_name[24];
    char life360_b_entity[64];
    char life360_b_name[24];
    char life360_c_entity[64];
    char life360_c_name[24];
    /* Curtain tile — HA cover entity + two optional battery sensors. Empty
     * curtain_entity disables the curtain tile. All empty by default so no
     * personal entity ids ship in the binary. */
    char curtain_entity[64];
    char curtain_bat_a[64];
    char curtain_bat_b[64];
    /* Blinds — second HA cover entity with optional battery sensors. Same
     * shape as the curtain fields above. Empty = blinds tile hidden. */
    char blinds_entity[64];
    char blinds_bat_a[64];
    char blinds_bat_b[64];
    /* Doorbell snapshot — when the HA trigger entity (binary_sensor / input_
     * boolean / event) goes "on", fetch a snapshot of doorbell_camera and show
     * it fullscreen on the Toon for doorbell_seconds. All empty/0 = disabled. */
    char doorbell_entity[64];   /* HA entity that turns "on" when pressed */
    char doorbell_camera[64];   /* HA camera.* entity to snapshot */
    int  doorbell_seconds;      /* how long the snapshot stays up (default 30) */
    /* Optional MJPEG stream for live footage (server-transcoded, e.g. a
     * Frigate/go2rtc resized MJPEG). When set, the overlay plays this stream
     * frame-by-frame instead of a single snapshot. Empty = still snapshot. */
    char doorbell_stream_url[256];

    /* Live video tile (Toon 1 only): hardware MPEG-4/H.264 decode on the
     * i.MX27 VPU via /root/vpu/vpu_stream, shown either direct-to-fb0 (with
     * an LVGL cutout) or as an fb1 graphic-window hardware overlay. Distinct
     * from the doorbell_* snapshot feature above. Cfg keys are video_* with
     * camera_* accepted as aliases (older configs). */
    int  video_enabled;        /* 0/1 — install the Video tile + warm pipeline */
    int  video_size_pct;       /* 25..125 % of video_src_w x video_src_h */
    int  video_src_w;          /* what the sender pushes (default 640) */
    int  video_src_h;          /* (default 480) */
    int  video_x;              /* panel px, or -1 to centre */
    int  video_y;              /* panel px, or -1 to centre */
    int  video_rtp;            /* UDP RTP port for vpu_stream; 0 = legacy TCP 5000 */
    int  video_overlay;        /* 0 = fb0 + cutout; 1 = fb1 FG hardware overlay */

    /* LAN hosts for the optional integrations + healthcheck probes. All
     * empty by default so no personal network topology ships in the binary;
     * a probe/poller is simply skipped when its host is empty. "ip" or
     * "ip:port" / hostname, no scheme. */
    char p1_elec_host[64];     /* HomeWizard P1 electricity meter */
    char p1_water_host[64];    /* HomeWizard P1 water meter */
    char vent_host[64];        /* Itho NRG-WiFi ventilation unit */
    char efanlamp_host[64];    /* ESPHome BLE fan+lamp (ld2411-ble-fanlamp), native API */
    char efanlamp_psk[64];     /* base64-encoded 32-byte Noise PSK for ESPHome native API */
    char opnsense_host[64];    /* router, for the "router offline" healthcheck */

    /* Auto-update — when on, the update checker installs a newer release
     * automatically around auto_update_hour (local), with a Toon banner. */
    int  auto_update_enabled;
    int  auto_update_hour;     /* 0..23, default 2 (≈02:00) */

    /* Newsreader — built-in RSS ticker above the forecast on the home screen.
     * Tapping a headline shows a QR to open the article on your phone. */
    int  news_enabled;
    char news_rss_url[640];    /* one or more RSS feed URLs, newline-separated */
    int  news_scroll_speed;   /* ticker scroll speed, px/sec (0 → default 30) */

    /* Calendar — upcoming-events agenda. Two optional sources, merged + sorted:
     *  - calendar_ha_entity: a Home Assistant calendar.* entity (REST, Bearer)
     *  - calendar_ics_url:   a public/private .ics URL (CalDAV/Google/etc.)
     * Either or both; empty disables that source. */
    int  calendar_enabled;
    char calendar_ha_entity[64];
    char calendar_ics_url[1024];  /* one or more ICS URLs, newline-separated */
    int  calendar_notify_enabled;    /* 0/1 — pop a notification before a timed event starts */
    int  calendar_notify_lead_min;   /* minutes before the start to notify (default 15) */

    /* Animation Overlay Engine (Oepi-Loepi style animations) */
    int  anim_enabled;               /* 1 = animation engine enabled (default 1) */
    int  anim_birthday_enabled;      /* 1 = celebrate birthdays from Agenda with balloons (default 1) */
    int  anim_seasonal_enabled;      /* 1 = auto seasonal triggers: Sint, New Year, snow (default 1) */
    int  anim_dim_enabled;           /* 1 = allow animations to remain visible in dim mode (default 0) */
    int  anim_duration_sec;          /* animation display duration in seconds (default 25) */
    int  home_theme;                 /* 0 = freetoon dark dashboard, 1 = stock light tile-carousel */
    int  home_light_mode;            /* 0 = dark tiles (default), 1 = light tiles for freetoon dashboard */
    char stock_tiles[200];           /* stock-theme carousel layout: comma-sep tile keys per slot */
    int  stock_big_indoor;           /* stock theme: 1 = big readout is indoor temp, 0 = setpoint */

    /* Custom home-tile layout — when 1, the home screen places/sizes/hides the
     * built-in tiles from the grid layout (layout.c / toonui_layout.cfg) edited
     * in the "Indeling" editor. When 0 (default) the original hardcoded layout
     * is used unchanged. */
    int  custom_layout_enabled;

    /* Active layout preset name (empty = the default toonui_layout.cfg). Named
     * presets live at <data>/toonui_layout_<name>.cfg; switching writes the name
     * here so it's restored on the next boot. */
    char active_layout[24];

    /* Client mode — this Toon is a "slave" that mirrors a master Toon over
     * its PWA HTTP API instead of talking to local HCB daemons. When 1, all
     * local integrations (BoxTalk, P1, meteradapter, weather, waste, vent,
     * HA, Domoticz, MQTT) are skipped; client_link.c streams the master's
     * /api/state/stream into toon_state and routes setpoint/program/curtain
     * control back to the master. The slave connects ONLY to master_host. */
    int  client_mode;
    char master_host[64];   /* master Toon IP/host, no scheme (PWA port 10081) */

    /* PWA web-interface login (browser-facing).
     *
     * Enabled by default — a freshly-installed Toon shouldn't expose the full
     * thermostat + curtain + schedule UI on the LAN with no gate. First visit
     * after enable is redirected to /set-password to mint a password; until
     * that's done the rest of the routes return 302 → /set-password (no
     * silent "anyone in" window).
     *
     * pwa_login_pass is plaintext to match the rest of the cfg (mqtt_pass,
     * vnc_pass). The threat model is "stop the neighbour or kid from poking
     * the UI", not "an attacker reading /mnt/data/toonui.cfg" — root on the
     * Toon already gives them everything. */
    int  pwa_login_enabled;   /* 0/1 — gate the web UI behind login (default 1) */
    char pwa_login_user[32];  /* default "admin" */
    char pwa_login_pass[32];  /* default "" — empty forces /set-password */

    /* On-device PIN — gates anything the user might tap by accident or that
     * a household member shouldn't change unsupervised:
     *  - Opening Settings (any setting change)
     *  - Comfort/Home/Sleep/Away preset taps (home + dim screens)
     *  - Temperature +/- setpoint adjustments
     *  - Schedule mode toggle (auto ↔ manual)
     *
     * Default OFF — opt-in. When pin_enabled but pin_code is empty, the
     * gate behaves as if disabled (no point asking for a PIN nobody set).
     * Stored plaintext (UI PIN, not a security boundary). */
    int  pin_enabled;
    char pin_code[8];         /* 4-6 numeric chars; empty = effectively off */

    /* Custom NILM appliance signatures — user-named step-change patterns.
     * Each entry covers a watt-delta range [lo, hi) and a display name.
     * Stored as nilm_sig_N=Name,lo,hi in toonui.cfg (N = 0-based index). */
#define NILM_CUSTOM_MAX 12
    char  nilm_sig_name[NILM_CUSTOM_MAX][40];
    float nilm_sig_lo  [NILM_CUSTOM_MAX];
    float nilm_sig_hi  [NILM_CUSTOM_MAX];
    int   nilm_sig_count;

    /* Philips Hue room/zone controlled by the home "Hue" tile (bridge group
     * id; "" = all lamps). Appended at the end: plugins read settings_t by
     * declared offsets, so new fields go here (see plugin_abi.h). */
    char  hue_tile_group[16];

    /* Built-in Telegram bot (telegram.c) — ToonBot replacement. Approved
     * chats live in <data>/toonui_telegram_chats.txt, not here. */
    int   telegram_enabled;      /* 0/1 — long-poll the Bot API */
    char  telegram_token[64];    /* BotFather token "123456:ABC-..." */
    int   telegram_alerts;       /* TG_ALERT_* bitmask (default all) */

    /* Client mode: the master's web login (Settings > Webtoegang on the
     * master), used when the master has one. */
    char  master_web_user[32];
    char  master_web_pass[32];

    /* oepi-loepi's ToonWater ESP sensor ("ip" or "ip:port", serves
     * /water.html). When set, the water meter (enable_p1_water) reads it
     * instead of the HomeWizard HWE-WTR. */
    char  toonwater_host[64];

    /* Opt-in: install marketplace app updates at auto_update_hour (needs
     * update_check_enabled; app_updates.c). */
    int   auto_update_apps;

    /* HomeWizard kWh meter (HWE-KWH1/3, SDM230/630-wifi), IPv4 address. Meant
     * for the PV inverter: when set it feeds the Solar tile (energy.c). */
    char  hw_kwh_host[24];
    /* Second kWh meter, for a second PV system (e.g. east + west on separate
     * inverters). Its production is added to the first one. */
    char  hw_kwh2_host[24];
    /* HomeWizard Energy Sockets (HWE-SKT): name + IPv4 each, shown with
     * on/off + watts on the Devices screen. Empty host = slot unused. */
#define HW_SOCKET_MAX 4
    char  hw_socket_name[HW_SOCKET_MAX][24];
    char  hw_socket_host[HW_SOCKET_MAX][24];

    /* NEW FIELDS GO HERE, AT THE END. Marketplace plugins (.so) read this
     * struct directly; a field inserted in the middle moves every field after
     * it and the loader then refuses those plugins ("app is not compatible").
     * 1.0.12 did exactly that with dim_elec_max_kwh (Sonos, Stocks refused).
     * plugin_loader.c checks the exported offsets at compile time. */
    int dim_elec_max_kwh;     /* 5..50 kWh — full scale for electricity day total bar (default 15) */
    int dim_thin_font;        /* 1 = dim clock + indoor/outdoor temp in the original thin
                                 Montserrat Regular (same size); 0 = Bold (default) */
    int   dyn_elec;           /* 1 = dynamic electricity contract: hourly EnergyZero day-ahead prices (#44)
                                 (was dyn_tariff, which covered both; same slot) */
    float dyn_elec_surcharge; /* EUR per kWh incl. VAT on top of market x 1.21 (mark-up + energy tax) */
    float dyn_gas_surcharge;  /* EUR per m3 incl. VAT on top of market x 1.21 */
    int   dyn_gas;            /* 1 = dynamic gas contract: EnergyZero gas-day prices */
    /* Athom Homey Pro (2023) local Web API (BACKLOG #28). The API key lives
     * in /mnt/data/homey.key (0600), not here. */
    int  enable_homey;
    char homey_host[64];      /* "ip[:port]" or http(s)://... */
    /* The two switches on the Verlichting home tile: "dz:<idx>",
     * "ha:<entity_id>", "hy:<homey id>", "zw:<uuid>", "" (empty) or "auto"
     * (default: the old Domoticz T1/T2 + HA Home + Z-Wave rule). See
     * tile_lights.h. */
    char tile_light1[72];
    char tile_light2[72];
    /* Places 3..8 of the Verlichting tile (shown when the tile is big
     * enough): same refs, "" = empty, never "auto". */
    char tile_light3[72];
    char tile_light4[72];
    char tile_light5[72];
    char tile_light6[72];
    char tile_light7[72];
    char tile_light8[72];
    int  dim_fc_min;          /* 1 = dim forecast shows day and night temperature ("14°/6°") */
    /* Boiler on another Toon (boiler_remote.c): this Toon decides when to
     * heat, the Toon at host (wired to the boiler) is set to boiler_remote_on
     * or _off (centi-degrees, 3000 / 600 like Thermostat+'s Local mode). */
    char boiler_remote_host[64];
    int  boiler_remote_on;
    int  boiler_remote_off;
    /* HA lifetime solar production sensor (kWh / Wh / MWh). When set, the
     * Zon statistics count its increase instead of integrating the watts of
     * ha_solar_entity (no energy lost over gaps); the W sensor stays for the
     * live tiles. */
    char ha_solar_total_entity[64];
    /* Homey flows tile (tile_flows.h): "auto" or one flow id per place,
     * comma-separated (8 places x 39 chars + commas). */
    char tile_flows[336];
    /* Integratie tiles on the home page and the dim Integratie block
     * (tile_slots.h TILE_SLOT_HOME_0.. / TILE_SLOT_DIM): integration ids. */
    char tile_slot_home[4][48];
    char tile_slot_dim[48];
    /* Energie tile: show the NILM step label ("~120 W ON") for 10 s after a
     * jump in power. Off by default: unlabelled it reads as a mystery value. */
    int  energy_nilm_label;
    /* Ventilation tile via Homey (vent_homey.c): a plain fan switched by two
     * relays (e.g. a Shelly Plus 2PM in switch mode) that Homey knows. */
    int  vent_homey;          /* 1 = the ventilation tile drives these relays */
    char vent_hy_r1[40];      /* Homey device id of relay 1 ("" = none) */
    char vent_hy_r2[40];      /* Homey device id of relay 2 */
    int  vent_hy_low;         /* relays on for "Laag": bit 0 = relay 1, bit 1 = relay 2 */
    int  vent_hy_high;        /* relays on for "Hoog" */
    /* Page 3 of the home screen (custom layout): integration ids of its
     * slot tiles (tile_slots.h TILE_SLOT_P3_0..); must match TILE_SLOT_P3_N. */
    char tile_slot_page3[12][48];
    /* Integratie tiles bound to "local:ext" (tile_ext.c): per slot
     * "ha:<entity_id>" or "dz:<idx>"; must match TILE_SLOT_COUNT. */
    char tile_slot_ext[33][96];
    /* Ventilation via Homey: the on/off capability of relay 1 / 2 ("" =
     * "onoff"; "onoff.1" etc. for one channel of a multi-channel device). */
    char vent_hy_c1[24];
    char vent_hy_c2[24];
    /* WiFi QR tile (tile_wifiqr.c): mode 0 = the Toon's own network
     * (/qmf/config/config_hcb_netcon.xml), 1 = the network below (a guest
     * network). sec: 0 WPA/WPA2/WPA3, 1 WEP, 2 none. */
    int  wifiqr_mode;
    char wifiqr_ssid[64];
    char wifiqr_pass[64];
    int  wifiqr_sec;
    int  wifiqr_hidden;
} settings_t;

#define FORECAST_AUTO   0
#define FORECAST_HOURLY 1
#define FORECAST_DAILY  2


extern settings_t settings;

void settings_load(void);
void settings_save(void);
void vnc_apply_state(int enabled);

/* Strip control chars (<0x20) from a string in place — keeps user-entered
 * values from corrupting the cfg or injecting newlines. */
void settings_sanitize_str(char * s);
/* Weather Lat/Lon typed the wrong way round (lat ~2-8, lon ~49-56): swaps
 * the two strings back and returns 1, else 0. */
#include <stddef.h>
int settings_coords_unswap(char * lat, size_t latsz, char * lon, size_t lonsz);

#endif
