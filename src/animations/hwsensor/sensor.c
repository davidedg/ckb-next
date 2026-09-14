#include "sensor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hwmon_backend.h"
#include "olh_backend.h"

#define FAKE_PREFIX "fake:"

void sensor_list_init(sensor_list* list){
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void sensor_list_add(sensor_list* list, const char* id, const char* label, const char* group){
    if(list->count == list->capacity){
        size_t new_capacity = list->capacity ? list->capacity * 2 : 8;
        sensor_desc* grown = realloc(list->items, new_capacity * sizeof(sensor_desc));
        if(!grown)
            return;
        list->items = grown;
        list->capacity = new_capacity;
    }
    sensor_desc* d = &list->items[list->count];
    snprintf(d->id, SENSOR_ID_MAX, "%s", id);
    snprintf(d->label, SENSOR_LABEL_MAX, "%s", label);
    snprintf(d->group, SENSOR_GROUP_MAX, "%s", group);
    list->count++;
}

void sensor_list_free(sensor_list* list){
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

size_t sensor_discover_all(sensor_list* list){
    hwmon_discover(list);
    olh_discover(list);
    return list->count;
}

static int fake_read(const char* id_after_prefix, double* out_value){
    char* end = NULL;
    double v = strtod(id_after_prefix, &end);
    if(end == id_after_prefix)
        return 0;
    *out_value = v;
    return 1;
}

int sensor_read(const char* id, double* out_value){
    if(!strncmp(id, FAKE_PREFIX, strlen(FAKE_PREFIX)))
        return fake_read(id + strlen(FAKE_PREFIX), out_value);
    if(!strncmp(id, HWMON_PREFIX, strlen(HWMON_PREFIX)))
        return hwmon_read(id + strlen(HWMON_PREFIX), out_value);
    if(!strncmp(id, OLH_PREFIX, strlen(OLH_PREFIX)))
        return olh_read(id + strlen(OLH_PREFIX), out_value);
    /* Unknown prefix -- no reading available. */
    return 0;
}
