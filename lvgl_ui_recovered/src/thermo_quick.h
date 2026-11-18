#ifndef TOON_THERMO_QUICK_H
#define TOON_THERMO_QUICK_H
#include <time.h>
/* Quick thermostat actions (thermo_quick.c): Boost +1 °C for an hour and a
 * holiday (Away, held) until a return time. */
int  thermo_boost_active(void);
int  thermo_boost_start(void);
void thermo_boost_stop(void);
int  thermo_holiday_active(void);
int  thermo_holiday_start(time_t until);
void thermo_holiday_stop(void);
void thermo_quick_tick(void);     /* UI idle tick: ends boost / holiday */
#endif
