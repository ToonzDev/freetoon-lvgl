#ifndef TOON_ENERGY_PRICES_H
#define TOON_ENERGY_PRICES_H

/* Dynamic energy contracts (#44): hourly day-ahead market prices from the
 * EnergyZero public API (electricity per hour, gas per gas day), cached on
 * /mnt/data, and turned into all-in prices:
 *
 *     all-in = market (excl. VAT) x 1.21 + surcharge (incl. VAT)
 *
 * where the surcharge is the user's supplier mark-up + energy tax
 * (settings.dyn_elec_surcharge / dyn_gas_surcharge).
 *
 * Costs over a day use the USAGE-WEIGHTED market price of that day: hourly
 * meter deltas (hcb_rrd "5yrhours") x that hour's price, divided by the
 * day's usage. A worker thread computes those per UTC day (the day buckets
 * of the Statistics screen) and caches finished days, so month and year
 * views stay cheap after the first time.
 *
 * All functions below are for the UI thread. Lookups never block: a missing
 * price or day is queued for the worker and the caller gets 0 back; when the
 * worker has news, the listener is called from ep_tick() (UI thread). */

enum { EP_ELEC = 0, EP_GAS = 1 };

/* Usage-weighted day prices are cached per kind. */
enum { EP_K_AVG_E = 0, EP_K_AVG_G, EP_K_ELEC, EP_K_GAS, EP_K_SOLAR, EP_K_COUNT };

int    ep_dynamic(void);                       /* settings.dyn_tariff */
double ep_allin(int com, double market);       /* market x 1.21 + surcharge */

/* Market price (EUR excl. VAT per kWh / m3) of the hour containing t.
 * 1 = known; 0 = unknown (fetch queued). */
int    ep_market_at(int com, long t, double * market);

/* Price factor for UTC day `day` (= epoch / 86400) for kind EP_K_ELEC /
 * EP_K_GAS / EP_K_SOLAR: the usage-weighted market price. Returns 1 exact,
 * 2 = plain day average (weighting not done yet / no hourly usage),
 * 0 = unknown. Call ep_request_days() for the range first. */
int    ep_day_market(int kind, long day, double * market);

/* Queue the work for days [day0, day1] (UTC day numbers): prices, day
 * averages and - for kind ELEC/GAS/SOLAR - the usage weighting from the
 * given hourly logger(s) (l2 may be NULL). Cheap when all is cached. */
void   ep_request_days(int kind, const char * l1, const char * l2, long day0, long day1);

/* Electricity, from the current hour on, through the last known hour:
 * the `hours`-long block with the lowest average all-in price. */
int    ep_cheapest(int hours, long * start, double * avg_allin);

/* Today's elec all-in prices: quartile thresholds (for cheap / expensive
 * colouring). 0 when today's prices are not known yet. */
int    ep_today_quartiles(double * q1, double * q3);

int    ep_busy(void);                          /* worker has queued work */
/* Listeners are called from ep_tick (UI thread) when new data arrived;
 * each checks its own visibility. Up to 4. */
void   ep_add_listener(void (*cb)(void));
void   ep_remove_listener(void (*cb)(void));
void   ep_tick(void);                          /* UI thread, ~5x/s */
void   ep_start(void);                         /* idempotent; lookups call it */

#endif
