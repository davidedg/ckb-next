/*
 * Copyright (C) 2026  ckb-next Development Team
 * hwsensor is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * hwsensor is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with hwsensor.  If not, see <http://www.gnu.org/licenses/>.
 */

#define CKB_ENABLE_QUERY
#include <ckb-next/animation.h>
#include <pthread.h>
#include <time.h>

#include "sensor.h"

#define POLL_INTERVAL_SEC 1.0
#define STALE_MULTIPLIER  3.0

static ckb_gradient   g_gradient = {0};
static double         g_value_min = 0.0;
static double         g_value_max = 100.0;
static unsigned char  g_fallback_a = 0, g_fallback_r = 0, g_fallback_g = 0, g_fallback_b = 0;

/* Written by ckb_parameter() on the main thread, read by the poll thread. */
static pthread_mutex_t g_selection_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_selection_cond  = PTHREAD_COND_INITIALIZER;
static char            g_current_sensor_id[SENSOR_ID_MAX] = "";

/* Written by the poll thread, read by ckb_frame() on the main thread. */
static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static double          g_cache_value = 0.0;
static int             g_cache_valid = 0;
static struct timespec g_cache_updated_at;

static pthread_t g_poll_thread;

void ckb_info(){
    CKB_NAME("Hardware Sensor");
    CKB_VERSION("0.1");
    CKB_COPYRIGHT("2026", "ckb-next-ddg");
    CKB_LICENSE("GPLv2+");
    CKB_GUID("{5B1E7F0A-6C2D-4B8E-9A3F-1D2C3B4A5E6F}");
    CKB_DESCRIPTION("Maps a live hardware sensor reading onto a color gradient");

    CKB_PARAM_LIST("sensor", "Sensor:", "", "");
    sensor_list discovered;
    sensor_list_init(&discovered);
    sensor_discover_all(&discovered);
    for(size_t i = 0; i < discovered.count; i++)
        CKB_LISTITEM("sensor", discovered.items[i].id, discovered.items[i].label, discovered.items[i].group);
    sensor_list_free(&discovered);

    CKB_PARAM_GRADIENT("color", "Color:", "", "0:ff00ff00 50:ffffff00 100:ffff0000");
    CKB_PARAM_DOUBLE("value_min", "Minimum value:", "", 0.0, -100000.0, 100000.0);
    CKB_PARAM_DOUBLE("value_max", "Maximum value:", "", 100.0, -100000.0, 100000.0);
    CKB_PARAM_ARGB("fallback", "Fallback color:", "", 0, 0, 0, 0);

    CKB_KPMODE(CKB_KP_NONE);
    CKB_TIMEMODE(CKB_TIME_ABSOLUTE);
    CKB_REPEAT(FALSE);
    CKB_LIVEPARAMS(TRUE);

    CKB_PRESET_START("Default");
    CKB_PRESET_PARAM("sensor", "");
    CKB_PRESET_PARAM("color", "0:ff00ff00 50:ffffff00 100:ffff0000");
    CKB_PRESET_PARAM("value_min", "0");
    CKB_PRESET_PARAM("value_max", "100");
    CKB_PRESET_PARAM("fallback", "00000000");
    CKB_PRESET_END;
}

/* Runs on its own thread for the process's whole lifetime. Never touches
 * stdout (that's the protocol channel, owned by the main thread) and never
 * blocks ckb_frame()/ckb_time() -- it only ever holds g_cache_mutex for the
 * few instructions needed to publish one reading. */
static void* poll_thread_main(void* arg){
    (void)arg;
    for(;;){
        char id[SENSOR_ID_MAX];
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)POLL_INTERVAL_SEC;

        pthread_mutex_lock(&g_selection_mutex);
        pthread_cond_timedwait(&g_selection_cond, &g_selection_mutex, &deadline);
        snprintf(id, sizeof(id), "%s", g_current_sensor_id);
        pthread_mutex_unlock(&g_selection_mutex);

        if(id[0] == '\0'){
            pthread_mutex_lock(&g_cache_mutex);
            g_cache_valid = 0;
            pthread_mutex_unlock(&g_cache_mutex);
            continue;
        }

        double value = 0.0;
        int ok = sensor_read(id, &value);

        pthread_mutex_lock(&g_cache_mutex);
        if(ok){
            g_cache_value = value;
            g_cache_valid = 1;
            clock_gettime(CLOCK_MONOTONIC, &g_cache_updated_at);
        } else {
            g_cache_valid = 0;
        }
        pthread_mutex_unlock(&g_cache_mutex);
    }
    return NULL;
}

void ckb_init(ckb_runctx* context){
    pthread_create(&g_poll_thread, NULL, poll_thread_main, NULL);
}

void ckb_parameter(ckb_runctx* context, const char* name, const char* value){
    CKB_PARSE_LIST("sensor"){
        pthread_mutex_lock(&g_selection_mutex);
        strncpy(g_current_sensor_id, value, SENSOR_ID_MAX - 1);
        g_current_sensor_id[SENSOR_ID_MAX - 1] = '\0';
        pthread_cond_signal(&g_selection_cond);
        pthread_mutex_unlock(&g_selection_mutex);
    }
    CKB_PARSE_GRADIENT("color", &g_gradient){}
    CKB_PARSE_DOUBLE("value_min", &g_value_min){}
    CKB_PARSE_DOUBLE("value_max", &g_value_max){}
    CKB_PARSE_ARGB("fallback", &g_fallback_a, &g_fallback_r, &g_fallback_g, &g_fallback_b){}
}

void ckb_query_value(const char* name, const char* value, char* out, size_t out_size){
    if(strcmp(name, "sensor") != 0){
        snprintf(out, out_size, "error");
        return;
    }
    double v;
    if(sensor_read(value, &v))
        snprintf(out, out_size, "value %.1f", v);
    else
        snprintf(out, out_size, "error");
}

void ckb_start(ckb_runctx* context, int state){
    return;
}

void ckb_keypress(ckb_runctx* context, ckb_key* key, int x, int y, int state){
    return;
}

void ckb_time(ckb_runctx* context, double delta){
    return;
}

static int cache_is_stale(const struct timespec* updated_at){
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - updated_at->tv_sec) + (now.tv_nsec - updated_at->tv_nsec) / 1e9;
    return elapsed > STALE_MULTIPLIER * POLL_INTERVAL_SEC;
}

int ckb_frame(ckb_runctx* context){
    double value = 0.0;
    int valid = 0;

    pthread_mutex_lock(&g_cache_mutex);
    if(g_cache_valid && !cache_is_stale(&g_cache_updated_at)){
        value = g_cache_value;
        valid = 1;
    }
    pthread_mutex_unlock(&g_cache_mutex);

    unsigned char a, r, g, b;
    if(valid){
        double span = g_value_max - g_value_min;
        double pos;
        if(span <= 0.0){
            pos = (value >= g_value_max) ? 100.0 : 0.0;
        } else {
            pos = (value - g_value_min) / span * 100.0;
            if(pos < 0.0) pos = 0.0;
            if(pos > 100.0) pos = 100.0;
        }
        float fa, fr, fg, fb;
        ckb_grad_color(&fa, &fr, &fg, &fb, &g_gradient, (float)pos);
        a = (unsigned char)round(fa);
        r = (unsigned char)round(fr);
        g = (unsigned char)round(fg);
        b = (unsigned char)round(fb);
    } else {
        a = g_fallback_a;
        r = g_fallback_r;
        g = g_fallback_g;
        b = g_fallback_b;
    }

    unsigned count = context->keycount;
    ckb_key* keys = context->keys;
    for(unsigned i = 0; i < count; i++){
        keys[i].a = a;
        keys[i].r = r;
        keys[i].g = g;
        keys[i].b = b;
    }
    return 0;
}
