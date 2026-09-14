#ifndef HWSENSOR_SENSOR_H
#define HWSENSOR_SENSOR_H

#include <stddef.h>

#define SENSOR_ID_MAX     256
#define SENSOR_LABEL_MAX  128
#define SENSOR_GROUP_MAX  48

typedef struct {
    char id[SENSOR_ID_MAX];
    char label[SENSOR_LABEL_MAX];
    /* ASCII display name of the backend that found this sensor (e.g. "System
     * (hwmon)", "OpenLinkHub") -- lets the GUI offer a source filter without
     * knowing anything about individual backends. */
    char group[SENSOR_GROUP_MAX];
} sensor_desc;

typedef struct {
    sensor_desc* items;
    size_t count;
    size_t capacity;
} sensor_list;

void sensor_list_init(sensor_list* list);
void sensor_list_add(sensor_list* list, const char* id, const char* label, const char* group);
void sensor_list_free(sensor_list* list);

/* Aggregates every real backend's discovery. Called only from ckb_info(),
 * under its ~1s budget. Never includes "fake:" entries -- those exist only
 * for the test harness, reachable by setting the sensor id directly. */
size_t sensor_discover_all(sensor_list* list);

/* Dispatches by "<prefix>:" on id to the matching backend. Returns 1 and
 * writes *out_value on success, 0 on any failure (unknown prefix, I/O
 * error, parse error, unresolvable id) -- callers must treat 0 as "no
 * reading available", never as a fatal condition. Called only from the
 * poll thread, never from ckb_frame/ckb_time. */
int sensor_read(const char* id, double* out_value);

#endif /* HWSENSOR_SENSOR_H */
