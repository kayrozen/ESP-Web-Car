#pragma once

/*
 * LED status indicator
 *
 * Dev board: IO19
 * Production boards: IO17
 * Override at build time: -DLED_STATUS_GPIO=<pin>
 *
 * Patterns (all non-blocking, driven by a dedicated FreeRTOS task):
 *
 *  LED_STATE_BOOT           — solid on  (briefly during startup)
 *  LED_STATE_WIFI_PORTAL    — fast blink 200 ms on/off  (Phase 1 AP active)
 *  LED_STATE_BT_PORTAL      — double-pulse every 2 s    (Phase 2 BT pairing)
 *  LED_STATE_CONNECTING     — slow blink 1 s on/off     (WiFi or BT connecting)
 *  LED_STATE_PLAYING        — solid on                  (streaming)
 *  LED_STATE_SOFT_PAUSED    — slow pulse 2 s period     (soft pause)
 *  LED_STATE_HARD_PAUSED    — off                       (hard pause / standby)
 *  LED_STATE_OTA            — rapid double-flash loop   (firmware update)
 *  LED_STATE_ERROR_LOOP     — SOS pattern               (boot-loop guard tripped)
 *  LED_STATE_FACTORY_RESET  — 10 rapid flashes then off (reset triggered)
 */

typedef enum {
    LED_STATE_BOOT          = 0,
    LED_STATE_WIFI_PORTAL,
    LED_STATE_BT_PORTAL,
    LED_STATE_CONNECTING,
    LED_STATE_PLAYING,
    LED_STATE_SOFT_PAUSED,
    LED_STATE_HARD_PAUSED,
    LED_STATE_OTA,
    LED_STATE_ERROR_LOOP,
    LED_STATE_FACTORY_RESET,
} led_state_t;

/* Initialise GPIO and start the LED task. Must be called once before any
   led_status_set() call. */
void led_status_init(void);

/* Thread-safe: can be called from any task or ISR context. */
void led_status_set(led_state_t state);
