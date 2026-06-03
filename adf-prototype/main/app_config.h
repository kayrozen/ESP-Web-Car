#pragma once

// ------------------------------------------------------------
//  WiFi credentials — override via sdkconfig or edit here
// ------------------------------------------------------------
#ifndef WIFI_SSID
#define WIFI_SSID   "YOUR_SSID"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS   "YOUR_PASSWORD"
#endif

// ------------------------------------------------------------
//  Bluetooth target name (the car speaker / head unit)
// ------------------------------------------------------------
#define BT_SINK_NAME    "YOUR_BT_SPEAKER"

// ------------------------------------------------------------
//  Station list (index 0 is the startup station)
//  See station_list.c for the full table.
// ------------------------------------------------------------
#define STATION_COUNT   7

// ------------------------------------------------------------
//  Resampler output — A2DP source requires 44100 Hz stereo
// ------------------------------------------------------------
#define RSP_OUT_SAMPLE_RATE     44100
#define RSP_OUT_CHANNELS        2

// ------------------------------------------------------------
//  Stats monitor interval
// ------------------------------------------------------------
#define STATS_INTERVAL_MS       30000   /* 30 s */

// ------------------------------------------------------------
//  Phase C — endurance test duration (0 = run forever)
// ------------------------------------------------------------
#define ENDURANCE_DURATION_MS   0
