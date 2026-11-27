#ifndef TOON_TILE_EXT_H
#define TOON_TILE_EXT_H
/* tile_ext — a Home Assistant entity or a Domoticz device on an Integratie
 * tile (home page, pages 2 and 3, the dim Integratie block).
 *
 * The tile's slot binding is "local:ext"; what it shows is in
 * settings.tile_slot_ext[slot]: "ha:<entity_id>" or "dz:<idx>" (a separate
 * field: entity ids do not fit the 48-char binding). The values are filled by
 * the HA and Domoticz threads (seed + live events / pushes) into a per-slot
 * cache guarded by a mutex; the UI reads copies. */
#include <time.h>
#include <stddef.h>

#define TILE_EXT_SLOTS 33          /* = TILE_SLOT_COUNT; settings.tile_slot_ext */
enum { TEXT_NONE = 0, TEXT_HA = 1, TEXT_DZ = 2 };

typedef struct {
    int    kind;                   /* TEXT_* */
    char   id[96];                 /* entity_id, or the Domoticz idx */
    char   name[64];               /* friendly name / device name */
    char   value[64];              /* display value incl. unit ("58 %", "Aan") */
    time_t changed;                /* last change (0 = unknown) */
    int    switchable;             /* tap toggles it */
    int    on;                     /* switchable: current state */
    int    have;                   /* a value arrived */
    int    error;                  /* last fetch failed / not found */
} tile_ext_t;

/* Binding (UI thread). kind TEXT_NONE clears. Saves settings, asks the
 * backend for the value at once. */
void tile_ext_bind(int slot, int kind, const char * id, const char * name);
int  tile_ext_kind(int slot);                      /* TEXT_* of a slot (from settings) */
int  tile_ext_get(int slot, tile_ext_t * out);     /* 1 = bound (out filled) */
/* "Home Assistant: sensor.x" / "Domoticz: apparaat 12" for the assign dialog. */
void tile_ext_describe(int slot, char * out, size_t n);
/* Tap on the tile: toggles a switchable entity/device. 1 = handled. */
int  tile_ext_tap(int slot);
/* Title / value / age line for a tile. */
void tile_ext_age(time_t changed, char * out, size_t n);

/* Backends (their threads). Iterate the bound ids of `kind`: returns the
 * next slot >= *it with that kind and copies its id; -1 at the end.
 * only_dirty: only those not fetched since they were bound. */
int  tile_ext_next(int kind, int * it, char * id, size_t n, int only_dirty);
int  tile_ext_wants_ha(const char * entity_id);    /* any tile shows it */
/* HA: a /api/states/<id> body or a state_changed new_state slice. ok=0: failed. */
void tile_ext_apply_ha(const char * entity_id, const char * json, int ok);
/* Domoticz: one device object of getdevices (p..end). ok=0: failed. */
void tile_ext_apply_dz(int idx, const char * obj, const char * end, int ok);

#endif
