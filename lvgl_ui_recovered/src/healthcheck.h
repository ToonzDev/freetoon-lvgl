#ifndef TOON_HEALTHCHECK_H
#define TOON_HEALTHCHECK_H

/* Background poller. Every HC_POLL_S seconds it pings each watched
 * service (HA, OTGW, MQTT, Buienradar, Internet/DNS, Itho, P1 electricity,
 * P1 water, OPNsense) and on transition pushes a notification via
 * notify.h. Each (type,subType) is unique per service so DeleteNotification
 * clears the right one on recovery.
 *
 * Start once from main.c after boxtalk_start() — the notifier needs the
 * BoxTalk socket up. */

int healthcheck_start(void);

/* 1 when the internet check (TCP to 1.1.1.1:53) has failed HC_FAILS_DOWN
 * times in a row, i.e. a real outage rather than one lost probe. 0 while it
 * works or before the first round. Used by inbox.c to decide whether the
 * stock hcb_netcon "not connected" message is worth showing. */
int healthcheck_internet_down(void);

#endif
