#include "hwmon_backend.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HWMON_MAX_CHIPS 64

typedef struct {
    char dirname[64];    /* "hwmon3" */
    char chip_name[128]; /* contents of <dirname>/name, ASCII-sanitized */
} hwmon_chip;

static const char* hwmon_root(void){
    const char* override = getenv("HWSENSOR_HWMON_ROOT");
    return (override && *override) ? override : "/sys/class/hwmon";
}

static void read_trimmed_line(const char* path, char* out, size_t out_size){
    out[0] = '\0';
    FILE* f = fopen(path, "r");
    if(!f)
        return;
    if(fgets(out, (int)out_size, f)){
        size_t len = strlen(out);
        while(len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' '))
            out[--len] = '\0';
    }
    fclose(f);
}

/* Dropdown labels go through printurl(), whose signed-char escaping
 * corrupts bytes >= 0x80 -- keep every label/chip-name ASCII. */
static void sanitize_ascii(char* s){
    for(; *s; s++){
        unsigned char c = (unsigned char)*s;
        if(c < 0x20 || c >= 0x7F)
            *s = '_';
    }
}

static size_t list_chips(hwmon_chip* chips, size_t max_chips){
    size_t n = 0;
    DIR* d = opendir(hwmon_root());
    if(!d)
        return 0;
    struct dirent* ent;
    while(n < max_chips && (ent = readdir(d)) != NULL){
        if(ent->d_name[0] == '.')
            continue;
        char name_path[512];
        snprintf(name_path, sizeof(name_path), "%s/%s/name", hwmon_root(), ent->d_name);
        char chip_name[128];
        read_trimmed_line(name_path, chip_name, sizeof(chip_name));
        if(chip_name[0] == '\0')
            continue;
        sanitize_ascii(chip_name);
        snprintf(chips[n].dirname, sizeof(chips[n].dirname), "%.63s", ent->d_name);
        snprintf(chips[n].chip_name, sizeof(chips[n].chip_name), "%.127s", chip_name);
        n++;
    }
    closedir(d);
    return n;
}

static int name_is_unique(const hwmon_chip* chips, size_t count, size_t index){
    for(size_t i = 0; i < count; i++){
        if(i != index && !strcmp(chips[i].chip_name, chips[index].chip_name))
            return 0;
    }
    return 1;
}

static void discover_leaves(sensor_list* list, const hwmon_chip* chip, int unique){
    char chip_dir[512];
    snprintf(chip_dir, sizeof(chip_dir), "%s/%s", hwmon_root(), chip->dirname);
    DIR* d = opendir(chip_dir);
    if(!d)
        return;
    struct dirent* ent;
    while((ent = readdir(d)) != NULL){
        const char* fname = ent->d_name;
        size_t flen = strlen(fname);
        static const char suffix[] = "_input";
        size_t suffix_len = sizeof(suffix) - 1;
        if(flen <= suffix_len || strcmp(fname + flen - suffix_len, suffix) != 0)
            continue;
        int is_temp = !strncmp(fname, "temp", 4);
        int is_fan = !strncmp(fname, "fan", 3);
        if(!is_temp && !is_fan)
            continue;

        char leaf[32];
        size_t leaf_len = flen - suffix_len;
        if(leaf_len >= sizeof(leaf))
            continue;
        memcpy(leaf, fname, leaf_len);
        leaf[leaf_len] = '\0';

        char label_path[560];
        snprintf(label_path, sizeof(label_path), "%s/%s_label", chip_dir, leaf);
        char custom_label[128];
        read_trimmed_line(label_path, custom_label, sizeof(custom_label));
        sanitize_ascii(custom_label);

        char id[SENSOR_ID_MAX];
        const char* id_chip = unique ? chip->chip_name : chip->dirname;
        snprintf(id, sizeof(id), "%s%s/%s", HWMON_PREFIX, id_chip, leaf);

        char label[SENSOR_LABEL_MAX];
        const char* unit = is_temp ? "C" : "RPM";
        if(custom_label[0])
            snprintf(label, sizeof(label), "%.48s (%.48s) - %s", custom_label, chip->chip_name, unit);
        else
            snprintf(label, sizeof(label), "%.48s %.16s - %s", chip->chip_name, leaf, unit);

        sensor_list_add(list, id, label);
    }
    closedir(d);
}

size_t hwmon_discover(sensor_list* list){
    hwmon_chip chips[HWMON_MAX_CHIPS];
    size_t count = list_chips(chips, HWMON_MAX_CHIPS);
    for(size_t i = 0; i < count; i++)
        discover_leaves(list, &chips[i], name_is_unique(chips, count, i));
    return list->count;
}

static int resolve_chip_dir(const char* chip_id, char* out_dir, size_t out_size){
    if(!strncmp(chip_id, "hwmon", 5) && isdigit((unsigned char)chip_id[5])){
        snprintf(out_dir, out_size, "%s/%s", hwmon_root(), chip_id);
        return 1;
    }
    hwmon_chip chips[HWMON_MAX_CHIPS];
    size_t count = list_chips(chips, HWMON_MAX_CHIPS);
    for(size_t i = 0; i < count; i++){
        if(!strcmp(chips[i].chip_name, chip_id)){
            snprintf(out_dir, out_size, "%s/%s", hwmon_root(), chips[i].dirname);
            return 1;
        }
    }
    return 0;
}

int hwmon_read(const char* id_after_prefix, double* out_value){
    const char* slash = strchr(id_after_prefix, '/');
    if(!slash)
        return 0;
    char chip_id[128];
    size_t chip_len = (size_t)(slash - id_after_prefix);
    if(chip_len == 0 || chip_len >= sizeof(chip_id))
        return 0;
    memcpy(chip_id, id_after_prefix, chip_len);
    chip_id[chip_len] = '\0';
    const char* leaf = slash + 1;
    if(!*leaf)
        return 0;

    char chip_dir[512];
    if(!resolve_chip_dir(chip_id, chip_dir, sizeof(chip_dir)))
        return 0;

    char input_path[560];
    snprintf(input_path, sizeof(input_path), "%s/%s_input", chip_dir, leaf);
    FILE* f = fopen(input_path, "r");
    if(!f)
        return 0;
    long raw;
    int ok = (fscanf(f, "%ld", &raw) == 1);
    fclose(f);
    if(!ok)
        return 0;

    *out_value = !strncmp(leaf, "temp", 4) ? raw / 1000.0 : (double)raw;
    return 1;
}
