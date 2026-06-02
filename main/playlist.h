#pragma once

#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <stdint.h>
#include <stdbool.h>

#define PLAYLIST_MAX 5

typedef struct {
    char name[64];
    char url[256];
    char stationuuid[40];
    char codec[8];
    uint16_t bitrate_kbps;
    bool is_custom;
} playlist_entry_t;

typedef struct {
    playlist_entry_t entries[PLAYLIST_MAX];
    int count;
    int current;
} playlist_t;

/**
 * @brief Load playlist from NVS. Falls back to empty playlist if not found.
 */
void playlist_load_from_nvs(playlist_t *out);

/**
 * @brief Serialize and save playlist to NVS.
 */
void playlist_save_to_nvs(const playlist_t *p);

/**
 * @brief Return pointer to current entry, or NULL if playlist is empty.
 */
const playlist_entry_t *playlist_current(const playlist_t *p);

/**
 * @brief Advance to next station (wraps), persist index.
 */
void playlist_next(playlist_t *p);

/**
 * @brief Go back to previous station (wraps), persist index.
 */
void playlist_prev(playlist_t *p);

#endif /* PLAYLIST_H */
