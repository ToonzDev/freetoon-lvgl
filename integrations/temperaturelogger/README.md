# Temperature Logger

Marketplace integratie en 24-uurs grafiek voor FreeToon.

## Functionaliteit
- Logt continu elke 10 minuten 3 temperatuurwaarden (144 datapunten over 24 uur):
  1. **Kamertemperatuur** (`toon_state.indoor_temp`)
  2. **Doeltemperatuur / Setpoint** (`toon_state.setpoint`)
  3. **Buitentemperatuur** (via actuele weerbron / Buienradar)
- Volledige compatibiliteit met de historische Toon 2 QML temperatuurlogger bestanden in `/HCBv2/var/` (`actualTempReading24H.txt`, `actualTempSetpoint24H.txt`, `actualTempBuienradar24H.txt`).
- Toont een interactieve 24-uurs grafiek via FreeToon's generieke `chart_24h` engine:
  - Tijd-as van 00:00 tot 24:00 (om de 2 uur gelabeld).
  - Y-as in °C met automatische schaling en handmatige ▲ / ▼ / Auto knoppen.
  - Interactieve serie-knoppen (chips) om afzonderlijke curven aan/uit te zetten.
  - Verticale tijdscursor ("Nu") met datummarkeringen (vandaag / gisteren).
- Werkt zowel als starttegel (Home tegel) als via Instellingen → Temperatuurlog.

