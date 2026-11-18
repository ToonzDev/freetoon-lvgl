/*
 * thermo_quick.c — two quick thermostat actions on top of the programme.
 *
 * Boost: +1 °C for one hour. Sets a temporary setpoint (the normal +/-
 * override); after an hour, if the setpoint is still the boost target, the
 * thermostat goes back to what it was doing: the weekly programme, or the
 * previous manual setpoint.
 *
 * Holiday: Away, held (LOCKEDBASE: the same as tapping "Weg" twice) until a
 * return date and time, then the weekly programme resumes. Changing the
 * preset by hand during the holiday ends it.
 *
 * "I'm leaving" already exists: tap Weg (eco until the next programme block)
 * or tap it twice (eco until you change it), so it is not duplicated here.
 *
 * End times live in settings (boost_until, holiday_until) so a UI restart
 * keeps them. thermo_quick_tick() runs from the UI idle tick.
 */
#include "thermo_quick.h"
#include "boxtalk.h"
#include "settings.h"
#include <stdio.h>
#include <time.h>

#define BOOST_SECONDS 3600

int thermo_boost_active(void) {
    return settings.boost_until > 0 && time(NULL) < (time_t)settings.boost_until;
}

int thermo_boost_start(void) {
    if (thermo_boost_active()) return 0;
    float cur = toon_state.setpoint;
    if (cur < 5.0f) return -1;
    settings.boost_prev_centi  = (int)(cur * 100.0f + 0.5f);
    settings.boost_prev_manual = (toon_state.program_state == PROG_MANUAL);
    settings.boost_target_centi = settings.boost_prev_centi + 100;
    if (boxtalk_set_setpoint(settings.boost_target_centi / 100.0f) != 0) return -1;
    settings.boost_until = (int)(time(NULL) + BOOST_SECONDS);
    settings_save();
    fprintf(stderr, "[quick] boost to %.1f until +1h\n", settings.boost_target_centi / 100.0f);
    return 0;
}

static void boost_restore(void) {
    if (settings.boost_prev_manual) boxtalk_set_setpoint(settings.boost_prev_centi / 100.0f);
    else                            boxtalk_resume_schedule();
}

void thermo_boost_stop(void) {
    if (!settings.boost_until) return;
    settings.boost_until = 0;
    settings_save();
    boost_restore();
}

int thermo_holiday_active(void) {
    return settings.holiday_until > 0;
}

int thermo_holiday_start(time_t until) {
    if (until <= time(NULL)) return -1;
    if (thermo_boost_active()) { settings.boost_until = 0; }
    /* Away, then Away again = held until changed (LOCKEDBASE). */
    if (boxtalk_set_program(3) != 0) return -1;
    if (toon_state.program_state != PROG_LOCKEDBASE) boxtalk_set_program(3);
    settings.holiday_until = (int)until;
    settings_save();
    fprintf(stderr, "[quick] holiday until %ld\n", (long)until);
    return 0;
}

void thermo_holiday_stop(void) {
    if (!settings.holiday_until) return;
    settings.holiday_until = 0;
    settings_save();
    boxtalk_resume_schedule();
}

void thermo_quick_tick(void) {
    static time_t last = 0;
    time_t now = time(NULL);
    if (now - last < 10) return;          /* every 10 s is plenty */
    last = now;

    if (settings.boost_until > 0 && now >= (time_t)settings.boost_until) {
        int still = (int)(toon_state.setpoint * 100.0f + 0.5f) == settings.boost_target_centi;
        settings.boost_until = 0;
        settings_save();
        if (still) boost_restore();       /* the user changed it meanwhile: leave it */
        fprintf(stderr, "[quick] boost over (%s)\n", still ? "restored" : "left as is");
    }

    if (settings.holiday_until > 0) {
        if (now >= (time_t)settings.holiday_until) {
            fprintf(stderr, "[quick] holiday over -> programme\n");
            thermo_holiday_stop();
        } else if (toon_state.connected && toon_state.active_state >= 0 &&
                   toon_state.active_state != 3) {
            /* preset changed by hand (here, the app, the web page): holiday off */
            fprintf(stderr, "[quick] preset changed during holiday -> holiday off\n");
            settings.holiday_until = 0;
            settings_save();
        }
    }
}
