#include "olh_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "http_client.h"

#define DEFAULT_OLH_URL "http://127.0.0.1:27003/api/devices/"

/* Dropdown labels go through printurl(), whose signed-char escaping
 * corrupts bytes >= 0x80 (OpenLinkHub's own temperatureString field, e.g.
 * "38.9 C", contains a real one -- never build a label from it). */
static void sanitize_ascii(char* s){
    for(; *s; s++){
        unsigned char c = (unsigned char)*s;
        if(c < 0x20 || c >= 0x7F)
            *s = '_';
    }
}

static const char* olh_url(void){
    const char* override = getenv("HWSENSOR_OLH_URL");
    return (override && *override) ? override : DEFAULT_OLH_URL;
}

static int parse_http_url(const char* url, char* host, size_t host_size, unsigned short* port, char* path, size_t path_size){
    if(strncmp(url, "http://", 7) != 0)
        return 0;
    const char* p = url + 7;
    const char* host_start = p;
    while(*p && *p != ':' && *p != '/')
        p++;
    size_t host_len = (size_t)(p - host_start);
    if(host_len == 0 || host_len >= host_size)
        return 0;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    unsigned short parsed_port = 80;
    if(*p == ':'){
        p++;
        char* end = NULL;
        long v = strtol(p, &end, 10);
        if(end == p || v <= 0 || v > 65535)
            return 0;
        p = end;
        parsed_port = (unsigned short)v;
    }
    *port = parsed_port;

    snprintf(path, path_size, "%s", (*p == '/') ? p : "/");
    return 1;
}

/* Caller owns the returned buffer (free() it). NULL on any failure. */
static char* fetch_payload(void){
    const char* url = olh_url();

    if(!strncmp(url, "file://", 7)){
        const char* path = url + 7;
        FILE* f = fopen(path, "rb");
        if(!f)
            return NULL;
        if(fseek(f, 0, SEEK_END) != 0){
            fclose(f);
            return NULL;
        }
        long size = ftell(f);
        if(size < 0 || fseek(f, 0, SEEK_SET) != 0){
            fclose(f);
            return NULL;
        }
        char* buf = malloc((size_t)size + 1);
        if(!buf){
            fclose(f);
            return NULL;
        }
        size_t got = fread(buf, 1, (size_t)size, f);
        fclose(f);
        buf[got] = '\0';
        return buf;
    }

    char host[128];
    unsigned short port;
    char path[512];
    if(!parse_http_url(url, host, sizeof(host), &port, path, sizeof(path)))
        return NULL;
    if(strcmp(host, "127.0.0.1") != 0 && strcmp(host, "localhost") != 0)
        return NULL; /* loopback-only by design */

    http_response resp;
    if(http_get_loopback(port, path, OLH_CONNECT_TIMEOUT_MS, OLH_RECV_TIMEOUT_MS, &resp) != 0)
        return NULL;
    return resp.body; /* ownership transferred to caller */
}

static void discover_from_payload(sensor_list* list, const cJSON* root){
    const cJSON* devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    const cJSON* device;
    cJSON_ArrayForEach(device, devices){
        const char* serial = device->string;
        if(!serial)
            continue;
        const cJSON* get_device = cJSON_GetObjectItemCaseSensitive(device, "GetDevice");
        const cJSON* channels = cJSON_GetObjectItemCaseSensitive(get_device, "devices");
        const cJSON* channel;
        cJSON_ArrayForEach(channel, channels){
            const char* channel_id = channel->string;
            if(!channel_id)
                continue;
            const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(channel, "name");
            char name[96];
            snprintf(name, sizeof(name), "%.64s", cJSON_IsString(name_item) ? name_item->valuestring : channel_id);
            sanitize_ascii(name);

            const cJSON* has_temps = cJSON_GetObjectItemCaseSensitive(channel, "HasTemps");
            const cJSON* has_speed = cJSON_GetObjectItemCaseSensitive(channel, "HasSpeed");

            if(cJSON_IsTrue(has_temps)){
                char id[SENSOR_ID_MAX];
                snprintf(id, sizeof(id), "%s%s/%s/temperature", OLH_PREFIX, serial, channel_id);
                char label[SENSOR_LABEL_MAX];
                snprintf(label, sizeof(label), "%s - C", name);
                sensor_list_add(list, id, label, "OpenLinkHub");
            }
            if(cJSON_IsTrue(has_speed)){
                char id[SENSOR_ID_MAX];
                snprintf(id, sizeof(id), "%s%s/%s/rpm", OLH_PREFIX, serial, channel_id);
                char label[SENSOR_LABEL_MAX];
                snprintf(label, sizeof(label), "%s - RPM", name);
                sensor_list_add(list, id, label, "OpenLinkHub");
            }
        }
    }
}

size_t olh_discover(sensor_list* list){
    char* payload = fetch_payload();
    if(!payload)
        return list->count;
    cJSON* root = cJSON_Parse(payload);
    free(payload);
    if(!root)
        return list->count;
    discover_from_payload(list, root);
    cJSON_Delete(root);
    return list->count;
}

int olh_read(const char* id_after_prefix, double* out_value){
    const char* slash1 = strchr(id_after_prefix, '/');
    if(!slash1)
        return 0;
    const char* slash2 = strchr(slash1 + 1, '/');
    if(!slash2)
        return 0;

    char serial[128], channel_id[32], field[32];
    size_t serial_len = (size_t)(slash1 - id_after_prefix);
    size_t channel_len = (size_t)(slash2 - (slash1 + 1));
    if(serial_len == 0 || serial_len >= sizeof(serial) || channel_len == 0 || channel_len >= sizeof(channel_id))
        return 0;
    memcpy(serial, id_after_prefix, serial_len);
    serial[serial_len] = '\0';
    memcpy(channel_id, slash1 + 1, channel_len);
    channel_id[channel_len] = '\0';
    snprintf(field, sizeof(field), "%s", slash2 + 1);

    char* payload = fetch_payload();
    if(!payload)
        return 0;
    cJSON* root = cJSON_Parse(payload);
    free(payload);
    if(!root)
        return 0;

    int ok = 0;
    const cJSON* devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    const cJSON* device = cJSON_GetObjectItemCaseSensitive(devices, serial);
    const cJSON* get_device = cJSON_GetObjectItemCaseSensitive(device, "GetDevice");
    const cJSON* channels = cJSON_GetObjectItemCaseSensitive(get_device, "devices");
    const cJSON* channel = cJSON_GetObjectItemCaseSensitive(channels, channel_id);
    const cJSON* value_item = cJSON_GetObjectItemCaseSensitive(channel, field);
    if(cJSON_IsNumber(value_item)){
        *out_value = value_item->valuedouble;
        ok = 1;
    }

    cJSON_Delete(root);
    return ok;
}
