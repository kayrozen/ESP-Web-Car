#pragma once

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* ── NVS ─────────────────────────────────────────────────────────────── */
#define CARRADIO_NVS_NAMESPACE      "carradio"

/* ── NVS keys ────────────────────────────────────────────────────────── */
#define NVS_KEY_CONFIG_VERSION      "cfg_ver"
#define NVS_KEY_PHASE               "phase"
#define NVS_KEY_WIFI_SSID           "wifi_ssid"
#define NVS_KEY_WIFI_PASS           "wifi_pass"
#define NVS_KEY_STREAM_URL          "stream_url"
#define NVS_KEY_BT_NAME             "bt_name"
#define NVS_KEY_BT_MAC              "bt_mac"
#define NVS_KEY_BOOT_FAIL_COUNT     "boot_fails"

/* ── WiFi SoftAP ─────────────────────────────────────────────────────── */
#define CARRADIO_SOFTAP_SSID        "CarRadio-Setup"
#define CARRADIO_SOFTAP_PASS        ""          /* open network */
#define CARRADIO_SOFTAP_CHANNEL     1
#define CARRADIO_SOFTAP_MAX_CONN    4

/* ── mDNS ────────────────────────────────────────────────────────────── */
#define CARRADIO_MDNS_HOSTNAME      "carradio"
#define CARRADIO_MDNS_INSTANCE      "CarRadio Setup"

/* ── Setup phases ────────────────────────────────────────────────────── */
#define CARRADIO_PHASE_WIFI         0   /* Need WiFi credentials */
#define CARRADIO_PHASE_BT           1   /* Need Bluetooth device */
#define CARRADIO_PHASE_READY        2   /* Fully configured — stream */

/* ── Boot-fail guard ─────────────────────────────────────────────────── */
#define CARRADIO_BOOT_FAIL_MAX      5
#define CARRADIO_GOOD_BOOT_MS       60000   /* 60 s of streaming = good boot */

/* ── Ring buffer sizes (in PSRAM) ────────────────────────────────────── */
#define CARRADIO_RAW_BUF_SIZE       (32 * 1024)   /* compressed audio */
#define CARRADIO_PCM_BUF_SIZE       (64 * 1024)   /* decoded PCM */

/* ── Bluetooth ───────────────────────────────────────────────────────── */
#define CARRADIO_BT_SCAN_SECS       8
#define CARRADIO_BT_MAX_DEVICES     20

/* ── GPIO ────────────────────────────────────────────────────────────── */
#define CARRADIO_RESET_GPIO         0   /* BOOT button */
#define CARRADIO_RESET_HOLD_MS      3000

/* ── Backoff / timeouts ──────────────────────────────────────────────── */
#define CARRADIO_BACKOFF_INIT_MS    500
#define CARRADIO_BACKOFF_MAX_MS     30000
#define CARRADIO_FAIL_REBOOT_MS     (5 * 60 * 1000)   /* 5 min stuck → reboot */

/* ── HTTP stream ─────────────────────────────────────────────────────── */
#define CARRADIO_HTTP_BUF_SIZE      4096
#define CARRADIO_HTTP_TIMEOUT_MS    10000

/* ── Audio ───────────────────────────────────────────────────────────── */
#define CARRADIO_SAMPLE_RATE        44100
#define CARRADIO_CHANNELS           2
#define CARRADIO_BITS_PER_SAMPLE    16

/* ── OTA ─────────────────────────────────────────────────────────────── */
#define CARRADIO_OTA_BUF_SIZE       4096

/* ── Config version ──────────────────────────────────────────────────── */
#define CARRADIO_CONFIG_VERSION     1

/* ── Task stack sizes ────────────────────────────────────────────────── */
#define TASK_STACK_SUPERVISOR       4096
#define TASK_STACK_HTTP_STREAM      8192
#define TASK_STACK_DECODE           8192
#define TASK_STACK_PORTAL           6144

/* ── Task priorities ─────────────────────────────────────────────────── */
#define TASK_PRIO_SUPERVISOR        3
#define TASK_PRIO_HTTP_STREAM       5
#define TASK_PRIO_DECODE            6
#define TASK_PRIO_PORTAL            4

#endif /* CONFIG_H */
