#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PLAYLIST_MAX 5

typedef struct {
    char     name[64];
    char     url[256];
    char     stationuuid[40];
    char     codec[8];
    uint16_t bitrate_kbps;
    bool     is_custom;
} playlist_entry_t;

typedef struct {
    playlist_entry_t entries[PLAYLIST_MAX];
    int count;
    int current;
} playlist_t;

void playlist_load_from_nvs(playlist_t *out);         /* falls back to empty if not found */
void playlist_save_to_nvs(const playlist_t *p);

const playlist_entry_t *playlist_current(const playlist_t *p);  /* NULL if empty */
void playlist_next(playlist_t *p);   /* wrap-around, persists index */
void playlist_prev(playlist_t *p);   /* wrap-around, persists index */
