#ifndef TOON_PWA_SERVER_H
#define TOON_PWA_SERVER_H

/* Tiny HTTP server bolted onto toonui (port 10081, next to the stock
 * happ_thermstat HTTP on 10080). Serves the remote-control page
 * (remote/index.html, compiled in) behind the web login, and exposes the
 * in-process state as JSON: GET /api/state, live via SSE on
 * /api/state/stream, plus action endpoints (/api/setpoint, /api/program,
 * /api/vent, /api/curtain, ...). A client-mode Toon mirrors a master
 * through the same API. */

/* Start the server thread. Returns 0 on success. */
int pwa_start(void);

/* End every web session (password changed or reset on the Toon). */
void pwa_sessions_clear(void);

#endif
