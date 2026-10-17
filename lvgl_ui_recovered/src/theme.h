#ifndef TOON_THEME_H
#define TOON_THEME_H

#include "settings.h"

/* Standard UI theme colors adapting dynamically to settings.home_light_mode */
#define THEME_BG          (settings.home_light_mode ? 0xe4eaf2 : 0x0f1a2a)
#define THEME_CARD_BG     (settings.home_light_mode ? 0xffffff : 0x1a2a44)
#define THEME_ROW_BG      (settings.home_light_mode ? 0xf0f4f8 : 0x1f3050)
#define THEME_ROW_BG_ALT  (settings.home_light_mode ? 0xe8eef5 : 0x152033)
#define THEME_BTN_BG      (settings.home_light_mode ? 0xd0d8e2 : 0x223344)
#define THEME_BTN_ACCENT  (settings.home_light_mode ? 0xccdaea : 0x335577)
#define THEME_TEXT_HI     (settings.home_light_mode ? 0x1e2838 : 0xffffff)
#define THEME_TEXT_DIM    (settings.home_light_mode ? 0x687c94 : 0x88aabb)
#define THEME_TEXT_MUTED  (settings.home_light_mode ? 0x8898aa : 0x556677)
#define THEME_BORDER      (settings.home_light_mode ? 0xc8d4e2 : 0x335577)
#define THEME_WARN_ORANGE (settings.home_light_mode ? 0xd97706 : 0xffcc44)
#define THEME_ACCENT_BLUE (settings.home_light_mode ? 0x1d4ed8 : 0x44aaff)
#define THEME_OFF         (settings.home_light_mode ? 0xd0d8e2 : 0x3a4658)

#endif /* TOON_THEME_H */

