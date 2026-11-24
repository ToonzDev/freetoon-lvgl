#ifndef TOON_BOILER_REMOTE_H
#define TOON_BOILER_REMOTE_H
/*
 * Boiler on another Toon (user report 2026-11-24): the Toon in the living
 * room decides when to heat (its own room temperature, setpoint and
 * programme in its own happ_thermstat), but the boiler is wired to a second
 * Toon in a cold room. As the ToonStore app Thermostat+ does in its "Local"
 * mode, this Toon (the sender) reads its own heat demand (happ burnerInfo
 * 1 or 3) and sets the boiler Toon's happ_thermstat to boiler_remote_on
 * (30.0) or boiler_remote_off (6.0) and its programme off, over that Toon's
 * plain happ HTTP: works whatever UI the boiler Toon runs. Sent at once on a
 * change and refreshed every minute.
 *
 * Safety net when the boiler Toon runs FreeToon: the sender also posts a
 * heartbeat to its :10081/api/v1/boiler/remote. A boiler Toon that got
 * heartbeats and then hears nothing for 15 minutes puts its own happ on the
 * safe (off) setpoint, so a sender that dies while "on" does not leave the
 * boiler heating the cold room towards 30 degrees.
 */
#include <time.h>

typedef struct {
    /* sender side */
    int    configured;       /* boiler_remote_host set */
    int    demand;           /* own happ asks for heat (-1 unknown) */
    int    reachable;        /* last command answered */
    time_t last_sent;
    int    sent_setpoint;    /* centi-degrees */
    double remote_temp;      /* boiler Toon's room temp (deg C, < -90 = unknown) */
    int    remote_burner;    /* its burnerInfo (-1 unknown) */
    int    remote_freetoon;  /* it answered the heartbeat (FreeToon, safety net on) */
    char   error[96];
    /* boiler side (safety net) */
    time_t hb_last;          /* last heartbeat received */
    char   hb_from[48];
    int    hb_tripped;       /* safe setpoint applied after silence */
} boiler_remote_status_t;

int  boiler_remote_start(void);
void boiler_remote_status(boiler_remote_status_t * out);
/* Heartbeat from a sender (pwa_server, already checked for the peer):
 * body {"on":0|1,"safe":600}. 0 = accepted. */
int  boiler_remote_heartbeat(const char * peer_ip, const char * body);
/* 1 when host is a plain IPv4 address or host name (it goes into a URL). */
int  boiler_remote_host_ok(const char * host);

#endif
