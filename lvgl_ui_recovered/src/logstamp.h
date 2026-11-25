#ifndef TOON_LOGSTAMP_H
#define TOON_LOGSTAMP_H
/* Put "YYYY-MM-DD HH:MM:SS " in front of every line written to stdout /
 * stderr (and by children). Call once, early in main(). See logstamp.c. */
void logstamp_start(void);
#endif
