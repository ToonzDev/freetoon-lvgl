/*
 * Ventilation tile via Homey: a plain fan with three positions (off / low /
 * high) switched by two relays that Homey knows, e.g. a Shelly Plus 2PM in
 * switch mode (settings.vent_homey, vent_hy_r1 / r2, vent_hy_low / high).
 *
 * It fills vent_state like the Itho source does, so the ventilation tile and
 * the dim screen show it without knowing the difference: fan_info / last_cmd
 * "off" / "low" / "high" / "timer", speed_pct 0 / 50 / 100, remaining_min
 * while a timer runs. The position is read back from the relays' onoff in
 * Homey, so a change made by Homey itself shows too.
 *
 * Commands (the tile's buttons, through vent_send_vremote_async): "low",
 * "high", "off" (both relays off), "timer1" / "timer2" / "timer3" (high for
 * 10 / 20 / 30 minutes, then back to the position before), "auto" (the Timer
 * button's cancel: back to the position before the timer). Relays
 * that must go off are switched first, then the ones that go on, so the
 * motor never gets two windings at once. The timer runs in FreeToon (lost on
 * a restart: the fan then stays where it was).
 */
#ifndef VENT_HOMEY_H
#define VENT_HOMEY_H

/* 1 when the ventilation tile is driven through Homey (switch on, Homey on,
 * a relay chosen). */
int vent_homey_active(void);

/* Run the 1 s update (an LVGL timer, independent of the screen shown). */
void vent_homey_start(void);

/* A tile command (see above). */
void vent_homey_cmd(const char * cmd);

/* Power of the relays together (measure_power), < 0 if Homey reports none. */
double vent_homey_watts(void);

#endif
