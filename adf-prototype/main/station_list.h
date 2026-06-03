#pragma once
#include "app_config.h"

typedef enum {
    STATION_TYPE_ICECAST,
    STATION_TYPE_HLS,
} station_type_t;

typedef enum {
    STATION_CODEC_MP3,
    STATION_CODEC_AAC,
    STATION_CODEC_OPUS,
} station_codec_t;

typedef struct {
    const char     *name;
    const char     *url;
    station_type_t  type;
    station_codec_t codec;
} station_t;

/* Indices into g_stations[] */
#define STATION_IDX_MP3_ICECAST     0
#define STATION_IDX_AAC_ICECAST     1
#define STATION_IDX_HLS_AAC         2
#define STATION_IDX_HLS_MASTER      3
#define STATION_IDX_OPUS            4
#define STATION_IDX_MONO            5
#define STATION_IDX_48KHZ           6

extern const station_t g_stations[STATION_COUNT];
