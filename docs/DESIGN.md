# FreeToon design guide

How FreeToon looks since 1.1, and how to make a screen or a marketplace integration look the same.
Everything here is a token in `lvgl_ui_recovered/src/theme.h`; the four palettes live in `theme.c`.

## The idea: quiet luxury

A thermostat hangs in a hallway and is read from two metres, mostly at a glance.
The screen should feel like a good piece of hardware (think Nest, Bang & Olufsen), not like an app.

- **Air before ornament.** Generous spacing on an 8 px grid; few elements per tile.
- **Depth from surfaces, not lines.** Canvas, card and row are close shades of one colour.
  No borders around tiles or buttons, no drop shadows (they also cost frame time on the Toon 1).
- **One accent per theme.** Interactive state (selected preset, active mode, switches, sliders)
  uses the theme accent. Everything else is neutral. Colour that *means* something stays:
  weather icons, gas/electricity/solar series, waste bins, people on the family tile.
- **Big numbers, small labels.** Readouts in Open Sans Light, labels small with a little tracking.
  Sentence case, no all-caps labels.
- **Same control everywhere.** A button is a borderless pill; the active one is the accent pill.

## Themes

| id | name | character |
|---|---|---|
| 0 | Nacht | refined dark navy (the original FreeToon look) |
| 1 | Licht | white cards on cool grey; the classic Toon look gets the original Eneco colours |
| 2 | Warm | anthracite with an orange accent, a nod to the original Toon |
| 3 | Zwart | true black for dark rooms, dimmed text |

Picked in Settings > Display (or `POST /api/settings {"ui_theme":N}`); stored as `ui_theme` in `toonui.cfg`.
`settings.home_light_mode` stays 1 exactly when Licht is active, for older plugins.

## Colour tokens

Always `lv_color_hex(THEME_…)`, never a literal for UI chrome.

| token | use |
|---|---|
| `THEME_BG` | screen background |
| `THEME_CARD_BG` | tiles, cards, modal panels |
| `THEME_ROW_BG` / `THEME_ROW_BG_ALT` | rows inside a card / recessed rows |
| `THEME_BTN_BG` | neutral button (the default pill) |
| `THEME_BTN_ACCENT` | raised neutral button |
| `THEME_OFF` | off state of a toggle or lamp |
| `THEME_BORDER` / `THEME_DIVIDER` | chart grid, rare 1 px lines / hairlines inside cards |
| `THEME_TEXT_HI` / `THEME_TEXT_DIM` / `THEME_TEXT_MUTED` | primary / secondary / hint text (all pass WCAG AA on a card) |
| `THEME_ACCENT` | accent for text, icons, thin marks |
| `THEME_ACCENT_FILL` + `THEME_ON_ACCENT` | the active pill and its label |
| `THEME_ACCENT_SOFT` | tinted surface of a selected row, a manual override |
| `THEME_ICON` | icon tint on cards |
| `THEME_SUCCESS` / `THEME_SUCCESS_BG` | ok, connected / confirm button |
| `THEME_DANGER` / `THEME_DANGER_BG` | errors / remove button |
| `THEME_WARNING` | amber warnings |
| `THEME_WARN_ORANGE` | the warm temperature readout |
| `THEME_GLOW` | heat: burner, hot water |
| `THEME_INFO` | cool secondary accent: water, cooling, rain |
| `THEME_INPUT_BG` | text fields and keyboards |
| `THEME_OFFLINE` | unreachable device |
| `THEME_DIM_BG` / `THEME_DIM_TEXT` / `THEME_DIM_TEXT_DIM` | the dim (screensaver) screen |
| `THEME_CLASSIC_*` | the classic Toon look (`screen_home_stock.c`) |
| `THEME_IS_LIGHT` | only for non-colour choices, such as an icon variant |

Scrims behind modals stay `lv_color_black()` with opacity.

## Shape and spacing

| token | value | use |
|---|---|---|
| `THEME_SP(n)` | 8·n px | gaps and margins: 8, 16, 24, 32 |
| `THEME_GAP` | 8 | between home tiles |
| `THEME_PAD` | 10 | inside a tile |
| `THEME_RADIUS_PANEL` | 18 | modal panels, dialogs |
| `THEME_RADIUS_TILE` | 16 | tiles, settings tiles, cards |
| `THEME_RADIUS_BTN` | 10 | buttons, rows |
| `THEME_RADIUS_SMALL` | 8 | controls inside a tile, chips |

Design coordinates are 1024×600; wrap positions and sizes in `SX()` / `SY()` (and fonts in `SF()`)
so the Toon 1 (800×480) scales.

## Type

| token | font | use |
|---|---|---|
| `THEME_FONT_NUM(px)` | Open Sans Light 30/40/50/90 | clock, temperatures, power, big readouts |
| `THEME_FONT_HEADING` | Montserrat 28 | screen titles |
| `THEME_FONT_TITLE` | Montserrat 22 | row titles, button labels |
| `THEME_FONT_BODY` | Montserrat 18 | body text |
| `THEME_FONT_LABEL` + `THEME_LABEL_TRACK` | Montserrat 14, +1 px tracking | tile titles, units, weekdays |
| `THEME_FONT_CAPTION` | Montserrat 14 | captions, hints |

Montserrat has no euro sign: for any label that shows "€" use `SF_EUR(px)` (14/18/22/36 px; single-glyph
fonts that fall back to Montserrat).

The Light fonts are readout subsets (no new font files: flash is scarce). Write big temperatures as
`20.5°` without the C; anything outside the subset falls back to Montserrat instead of a tofu box.

```c
lv_obj_t * t = lv_label_create(tile);
lv_obj_set_style_text_font(t, THEME_FONT_LABEL, 0);
lv_obj_set_style_text_letter_space(t, THEME_LABEL_TRACK, 0);
lv_obj_set_style_text_color(t, lv_color_hex(THEME_TEXT_DIM), 0);
lv_label_set_text(t, "Woonkamer");
```

## Controls

- **Pill button:** `THEME_BTN_BG`, radius `THEME_RADIUS_BTN` (inside a tile `THEME_RADIUS_SMALL`), no border,
  label `THEME_TEXT_HI` (or `THEME_TEXT_DIM` when it is one of a set).
- **Active / selected:** `THEME_ACCENT_FILL` with `THEME_ON_ACCENT`. A manual override of a preset:
  `THEME_ACCENT_SOFT` with `THEME_ACCENT` text. Never a white selection border.
- **Destructive:** `THEME_DANGER_BG` with `THEME_ON_ACCENT`. **Confirm:** `THEME_SUCCESS_BG`.
- Switches, sliders and checkboxes take the accent automatically (LVGL's default theme is re-initialised
  with the palette).
- Motion: short (≤ 200 ms) and only in answer to a touch; the Toon 1 cannot redraw full-screen fades smoothly.

## For marketplace integrations (.so plugins)

```c
#include "theme.h"
#include "fonts_opensans.h"   /* only if you use THEME_FONT_NUM */
FT_PLUGIN_DECLARE_ABI(FT_SETTINGS_FIELD(language),
                      FT_SETTINGS_FIELD(home_light_mode));   /* needed by the THEME_* fallback */
```

- The palette is read through the weak symbol `ft_theme_cur`, so your plugin follows the user's theme
  live, and still loads on an older toonui (it then falls back to the old light/dark pair).
- `ft_palette_t` is append-only; a token newer than the host's palette also falls back.
- Use the tokens above for every colour, the radius/spacing tokens for shapes, and the type tokens for text.
  A plugin screen should be indistinguishable from a built-in one.
- Need the theme id itself? Read `settings.ui_theme` and declare `FT_SETTINGS_FIELD(ui_theme)`
  (requires toonui 1.1 or later).
