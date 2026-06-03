#include "led_status.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <stdatomic.h>

static const char *TAG = "led";

/* ── Pin selection ───────────────────────────────────────────────────────
 * Dev board  → IO19
 * Production → IO17
 * Override with -DLED_STATUS_GPIO=<n> in build flags.
 */
#ifndef LED_STATUS_GPIO
#define LED_STATUS_GPIO  19
#endif

#define LED_ON()   gpio_set_level(LED_STATUS_GPIO, 1)
#define LED_OFF()  gpio_set_level(LED_STATUS_GPIO, 0)
#define DELAY(ms)  vTaskDelay(pdMS_TO_TICKS(ms))

static atomic_int s_state = LED_STATE_BOOT;

void led_status_set(led_state_t state)
{
    atomic_store(&s_state, (int)state);
}

/* ── Pattern helpers ─────────────────────────────────────────────────── */

/* Returns true if the state changed mid-pattern (caller should abort). */
static inline bool state_changed(led_state_t expected)
{
    return (led_state_t)atomic_load(&s_state) != expected;
}

static void blink_fast(void)          /* 200 ms on / 200 ms off */
{
    LED_ON();  DELAY(200);
    LED_OFF(); DELAY(200);
}

static void blink_slow(void)          /* 1 s on / 1 s off */
{
    LED_ON();  DELAY(1000);
    LED_OFF(); DELAY(1000);
}

static void double_pulse(void)        /* two 100 ms pulses, 2 s gap */
{
    LED_ON();  DELAY(100);
    LED_OFF(); DELAY(150);
    LED_ON();  DELAY(100);
    LED_OFF(); DELAY(1650);
}

static void slow_pulse(void)          /* 500 ms fade-sim: on briefly every 2 s */
{
    LED_ON();  DELAY(500);
    LED_OFF(); DELAY(1500);
}

static void rapid_double_flash(void)  /* OTA: __ __ pause */
{
    LED_ON();  DELAY(80);
    LED_OFF(); DELAY(80);
    LED_ON();  DELAY(80);
    LED_OFF(); DELAY(760);
}

/* SOS: · · ·  — — —  · · · */
static void sos_pattern(void)
{
    static const uint16_t pattern[] = {
        100, 150,  100, 150,  100, 450,   /* S: 3 short */
        300, 150,  300, 150,  300, 450,   /* O: 3 long  */
        100, 150,  100, 150,  100, 1500,  /* S: 3 short + long gap */
    };
    for (int i = 0; i < (int)(sizeof(pattern) / sizeof(pattern[0])); i += 2) {
        LED_ON();  DELAY(pattern[i]);
        LED_OFF(); DELAY(pattern[i + 1]);
    }
}

static void factory_reset_pattern(void)  /* 10 rapid flashes then stay off */
{
    for (int i = 0; i < 10; i++) {
        LED_ON();  DELAY(60);
        LED_OFF(); DELAY(60);
    }
    /* Stay off — led_task will see state change or keep showing off */
}

/* ── LED task ────────────────────────────────────────────────────────── */

static void led_task(void *arg)
{
    (void)arg;
    while (1) {
        led_state_t cur = (led_state_t)atomic_load(&s_state);
        switch (cur) {
            case LED_STATE_BOOT:
                LED_ON();
                DELAY(50);
                break;

            case LED_STATE_WIFI_PORTAL:
                blink_fast();
                break;

            case LED_STATE_BT_PORTAL:
                double_pulse();
                break;

            case LED_STATE_CONNECTING:
                blink_slow();
                break;

            case LED_STATE_PLAYING:
                LED_ON();
                DELAY(100);
                break;

            case LED_STATE_SOFT_PAUSED:
                slow_pulse();
                break;

            case LED_STATE_HARD_PAUSED:
                LED_OFF();
                DELAY(200);
                break;

            case LED_STATE_OTA:
                rapid_double_flash();
                break;

            case LED_STATE_ERROR_LOOP:
                sos_pattern();
                break;

            case LED_STATE_FACTORY_RESET:
                factory_reset_pattern();
                /* After the burst, hold off until state changes */
                while ((led_state_t)atomic_load(&s_state) == LED_STATE_FACTORY_RESET) {
                    LED_OFF();
                    DELAY(100);
                }
                break;

            default:
                LED_OFF();
                DELAY(200);
                break;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void led_status_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_STATUS_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    LED_OFF();

    ESP_LOGI(TAG, "LED on GPIO %d", LED_STATUS_GPIO);

    xTaskCreatePinnedToCore(
        led_task, "led",
        2048, NULL,
        1,   /* lowest priority — never blocks real work */
        NULL,
        0    /* Core 0, away from the audio decode task on Core 1 */
    );
}
