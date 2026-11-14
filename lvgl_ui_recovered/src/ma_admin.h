/*
 * Meter-adapter management (add / configure / remove), ported from the stock
 * qt-gui eMetersSettings app + qb/utils/ZWaveUtils.qml.
 *
 *  - Status: hdrv_p1 dataset "usageDevicesInfo" (one entry per adapter:
 *    uuid, name, driver status string, deviceStatus 0/1/2, and the usages
 *    elec/gas/solar/... it measures).
 *  - Add / remove: hdrv_zwave, serviceId "Administration", IncludeDevice /
 *    ExcludeDevice with event + eventTimeout. The reply arrives when the
 *    inclusion/exclusion window ends and carries a JSON string <pb> with
 *    status (added / deleted / canceled / failed / keyGrant / ssaAuth),
 *    devType and uuid. S2 keyGrant is accepted automatically (as stock);
 *    ssaAuth (PIN entry) is declined.
 *  - Force remove: hdrv_zwave specific1 RemoveDevice <uuid>.
 *  - Meter selection: hdrv_p1 specific1 GetMeasureCapabilities,
 *    GetSensorConfiguration, SetSensorConfiguration (all adapters at once).
 *
 * All calls are asynchronous; results land in state read with the ma_get_*
 * copies, each with a sequence number the UI polls for changes.
 */
#ifndef MA_ADMIN_H
#define MA_ADMIN_H

#define MA_MAX_DEV   4
#define MA_MAX_USAGE 6
#define MA_MAX_SENS  8
#define MA_MAX_CAPS  12

typedef struct {
    char type[8];      /* elec / gas / solar / heat / water */
    int  measure;      /* 0 analog, 1 p1, 2 laser */
    int  status;
} ma_usage_t;

typedef struct {
    char uuid[48];
    char ident[48];        /* deviceIdentifier, e.g. "Meetmodule 1" */
    char status_str[64];   /* driver-supplied, already localised */
    int  dev_status;       /* 0 not configured, 1 not connected, 2 OK */
    int  n_usage;
    ma_usage_t usage[MA_MAX_USAGE];
} ma_dev_t;

typedef struct {
    int  seq;              /* bumps on every dataset update; 0 = none yet */
    int  n;
    char status_str[64];
    ma_dev_t dev[MA_MAX_DEV];
} ma_info_t;

enum { MA_OP_NONE = 0, MA_OP_INCLUDE, MA_OP_EXCLUDE };

typedef struct {
    int  op;               /* MA_OP_* of the running / last operation */
    int  busy;             /* waiting for the driver's reply */
    int  seq;              /* bumps when a final result arrives */
    int  ssa_declined;     /* device asked for an S2 PIN; we declined */
    char status[24];       /* added / deleted / canceled / failed / timeout / ... */
    char dev_type[48];
    char uuid[48];
} ma_admin_t;

typedef struct {
    char uuid[48];
    int  n;
    char sensor[MA_MAX_SENS][16];  /* p1Elec, analogGas, analogSolar, ... */
} ma_sens_t;

typedef struct {
    int  seq;              /* bumps when a GetSensorConfiguration reply lands */
    int  n;
    ma_sens_t dev[MA_MAX_DEV];
} ma_sensconf_t;

typedef struct {
    int  seq;              /* bumps when a GetMeasureCapabilities reply lands */
    char uuid[48];
    int  n;
    char cap[MA_MAX_CAPS][16];     /* p1Elec, analogElec, laserElec, p1Gas, ... */
} ma_caps_t;

/* boxtalk.c hooks */
int  ma_is_p1_uuid(const char * uuid);
void ma_subscribe(void);
void ma_handle_action(const char * xml);   /* every <action> message */

/* status */
void ma_get_info(ma_info_t * out);

/* add / remove */
int  ma_include_start(void);
int  ma_include_stop(void);
int  ma_exclude_start(void);
int  ma_exclude_stop(void);
int  ma_force_remove(const char * dev_uuid);
void ma_admin_timeout(void);               /* UI gave up waiting */
void ma_get_admin(ma_admin_t * out);
int  ma_is_meter_type(const char * dev_type);

/* meter selection */
int  ma_request_sensor_config(void);
int  ma_request_caps(const char * dev_uuid);
void ma_get_sensor_config(ma_sensconf_t * out);
void ma_get_caps(ma_caps_t * out);
/* Send the full configuration (all adapters). Result via ma_get_set_result. */
int  ma_set_sensor_config(const ma_sensconf_t * all);
void ma_get_set_result(int * seq, int * ok);

#endif
