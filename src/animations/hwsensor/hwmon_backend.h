#ifndef HWSENSOR_HWMON_BACKEND_H
#define HWSENSOR_HWMON_BACKEND_H

#include "sensor.h"

#define HWMON_PREFIX "hwmon:"

/* Walks /sys/class/hwmon (or $HWSENSOR_HWMON_ROOT, for tests) and appends
 * one entry per temp*_input/fan*_input leaf found. Never fails loudly: a
 * missing root, an unreadable chip, or a chip with no matching leaves just
 * contributes nothing to the list. */
size_t hwmon_discover(sensor_list* list);

/* id_after_prefix is "<chip-name-or-hwmonN>/<leaf>", e.g. "k10temp/temp1"
 * or "hwmon3/temp1". Returns 1 + *out_value on success, 0 on any failure
 * (unknown chip, missing/unreadable file, unparseable content). */
int hwmon_read(const char* id_after_prefix, double* out_value);

#endif /* HWSENSOR_HWMON_BACKEND_H */
