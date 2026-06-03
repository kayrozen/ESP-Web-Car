#include "station_list.h"

/*
 * Test stations covering the cases from the prototype plan (§5).
 * URLs verified as of 2026-06 — replace if a stream goes offline.
 *
 * Phase B validation order:
 *   1. MP3 Icecast  (STATION_IDX_MP3_ICECAST)
 *   2. AAC Icecast  (STATION_IDX_AAC_ICECAST)
 *   3. HLS AAC      (STATION_IDX_HLS_AAC)   ← the decisive test
 */

const station_t g_stations[STATION_COUNT] = {
    [STATION_IDX_MP3_ICECAST] = {
        .name    = "CISM 89.3 Montreal (MP3 Icecast)",
        .url     = "http://streaming.cism.ca:8000/cism_mp3",
        .type    = STATION_TYPE_ICECAST,
        .codec   = STATION_CODEC_MP3,
    },
    [STATION_IDX_AAC_ICECAST] = {
        .name    = "ICI Musique Radio-Canada (AAC Icecast)",
        .url     = "https://ici-MusiquePlusOuest.akacast.akamaistream.net/7/284/493634/v1/rc.akacast.akamaistream.net/ici_musique_plus_ouest",
        .type    = STATION_TYPE_ICECAST,
        .codec   = STATION_CODEC_AAC,
    },
    [STATION_IDX_HLS_AAC] = {
        .name    = "BBC Radio 1 (HLS AAC)",
        .url     = "https://as-hls-ww-live.akamaized.net/pool_904/live/ww/bbc_radio_one/bbc_radio_one.isml/bbc_radio_one-audio=96000.norewind.m3u8",
        .type    = STATION_TYPE_HLS,
        .codec   = STATION_CODEC_AAC,
    },
    [STATION_IDX_HLS_MASTER] = {
        .name    = "BBC Radio 4 (HLS master playlist)",
        .url     = "https://as-hls-ww-live.akamaized.net/pool_904/live/ww/bbc_radio_fourfm/bbc_radio_fourfm.isml/bbc_radio_fourfm.m3u8",
        .type    = STATION_TYPE_HLS,
        .codec   = STATION_CODEC_AAC,
    },
    [STATION_IDX_OPUS] = {
        .name    = "FIP Radio France (Opus)",
        .url     = "https://icecast.radiofrance.fr/fip-hifi.aac",  /* fallback AAC if Opus unavailable */
        .type    = STATION_TYPE_ICECAST,
        .codec   = STATION_CODEC_AAC,
    },
    [STATION_IDX_MONO] = {
        .name    = "CKAC TalkRadio Montreal (mono MP3)",
        .url     = "http://22243.live.streamtheworld.com/CKACAM_SC",
        .type    = STATION_TYPE_ICECAST,
        .codec   = STATION_CODEC_MP3,
    },
    [STATION_IDX_48KHZ] = {
        .name    = "FIP 48 kHz AAC (resampling test)",
        .url     = "https://icecast.radiofrance.fr/fip-midfi.aac",
        .type    = STATION_TYPE_ICECAST,
        .codec   = STATION_CODEC_AAC,
    },
};
