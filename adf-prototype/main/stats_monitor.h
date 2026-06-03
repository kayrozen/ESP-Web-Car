#pragma once
#include <stdint.h>

/* Start a FreeRTOS task that logs heap + CPU stats every STATS_INTERVAL_MS. */
void stats_monitor_start(void);

/* Snapshot logged at each interval (also returned on demand). */
typedef struct {
    uint32_t free_internal_bytes;
    uint32_t free_spiram_bytes;
    uint32_t min_free_internal_bytes;   /* low-water mark since boot */
    uint32_t min_free_spiram_bytes;
    /* CPU load per core (0-100 %) — computed over last interval */
    uint32_t cpu_idle_pct[2];
    /* Monotonic counters set by main pipeline event handler */
    uint32_t audio_glitches;            /* MUSIC_STREAM_NEED_DATA events */
    uint32_t reconnects;                /* pipeline restarts */
    uint64_t uptime_ms;
} stats_snapshot_t;

/* Thread-safe read of the latest snapshot. */
void stats_get(stats_snapshot_t *out);

/* Called from main when an audio glitch is detected. */
void stats_record_glitch(void);
void stats_record_reconnect(void);
