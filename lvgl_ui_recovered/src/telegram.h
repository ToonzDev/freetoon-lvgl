/*
 * telegram.h — built-in Telegram bot (ToonBot replacement).
 *
 * The Toon long-polls the Telegram Bot API (outbound HTTPS only, no port
 * forwarding) and answers the same commands as the QML ToonBot app
 * (github.com/glsf91/toonbot): /info, /energie, /prog_c|t|s|w, /auto_a|u,
 * /therm_xx. Only chats approved on the Toon itself are answered; a new chat
 * shows up as "pending" in Settings → Telegram. Optional push alerts (boiler
 * error, low water pressure, thermostat offline) go to approved chats that
 * have alerts switched on.
 *
 * On first start an existing ToonBot config (/mnt/data/tsc/toonbot.userSettings.json)
 * is imported: token, allowed users and alarm chat.
 *
 * All functions are safe to call from the LVGL thread; the network work runs
 * on the module's own threads.
 */
#ifndef TOON_TELEGRAM_H
#define TOON_TELEGRAM_H

#include <time.h>

#define TG_MAX_CHATS 16
#define TG_LOG_MAX   10

/* settings.telegram_alerts bits */
#define TG_ALERT_BOILER    (1 << 0)   /* OpenTherm communication error */
#define TG_ALERT_PRESSURE  (1 << 1)   /* low CH water pressure */
#define TG_ALERT_OFFLINE   (1 << 2)   /* thermostat (BoxTalk) link down */
#define TG_ALERT_DEFAULT   (TG_ALERT_BOILER | TG_ALERT_PRESSURE | TG_ALERT_OFFLINE)

typedef enum { TG_CHAT_PENDING = 0, TG_CHAT_ALLOWED = 1, TG_CHAT_DENIED = 2 } tg_chat_state_t;

typedef struct {
    long long id;          /* Telegram chat id (negative for groups) */
    int  state;            /* tg_chat_state_t */
    int  alerts;           /* 1 = receives push alerts (allowed chats only) */
    char name[48];         /* display name / group title */
} tg_chat_t;

typedef struct {
    time_t t;
    char   who[48];
    char   text[64];
    char   result[24];     /* "Ok", "Fout", "Geweigerd", "Wacht op goedkeuring" */
} tg_log_t;

/* Start the poll + alert threads. Idles while disabled / no token, so the
 * Settings switch takes effect without a restart. Call once at startup. */
int  telegram_start(void);

/* Replace the bot token (saves settings, restarts the session). */
void telegram_set_token(const char * token);
/* Enable / disable polling (saves settings). */
void telegram_set_enabled(int on);

/* One-line status for the Settings screen, e.g. "Verbonden als @mijn_toon_bot". */
void telegram_status(char * out, int outsz);

/* Increments whenever chats or the log change — lets the UI refresh cheaply. */
unsigned telegram_version(void);

/* Snapshot of the chat list / log (newest first). Return the count. */
int  telegram_get_chats(tg_chat_t * out, int max);
int  telegram_get_log(tg_log_t * out, int max);

/* Chat management from the Settings screen. */
void telegram_chat_set_state(long long id, int state);
void telegram_chat_set_alerts(long long id, int on);
void telegram_chat_remove(long long id);

/* Send a test alert to every chat with alerts on. Returns chats reached. */
int  telegram_send_test(void);

#endif
