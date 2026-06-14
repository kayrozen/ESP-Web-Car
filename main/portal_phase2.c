/**
 * @file portal_phase2.c
 * @brief STA HTTP server at carradio.local — Phase 2: Bluetooth device selection + OTA.
 *
 * Endpoints:
 *   GET  /           → HTML page (BT scan + device list + OTA)
 *   GET  /bt_scan    → triggers BT GAP scan, returns JSON
 *   POST /bt_select  → save MAC + name, phase=READY, reboot
 *   GET  /ota        → OTA firmware upload form
 *   POST /ota_upload → perform OTA from uploaded URL
 */

#include "portal_phase2.h"
#include "config.h"
#include "storage.h"
#include "bluetooth.h"
#include "ota.h"
#include "supervisor.h"
#include "http_stream.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_http_server.h"

static const char *TAG = "portal2";

static httpd_handle_t s_server = NULL;

/* ── HTML page ───────────────────────────────────────────────────────── */

/* ── GET /now_playing ────────────────────────────────────────────────── */

static esp_err_t now_playing_handler(httpd_req_t *req)
{
    char np[128] = {0};
    bool ok = http_stream_get_now_playing(np, sizeof(np));
    char json[160];
    if (ok)
        snprintf(json, sizeof(json), "{\"title\":\"%s\"}", np);
    else
        strlcpy(json, "{\"title\":\"\"}", sizeof(json));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* ── POST /playback ──────────────────────────────────────────────────── */

static esp_err_t playback_handler(httpd_req_t *req)
{
    char body[32] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret > 0) body[ret] = '\0';

    /* body = "action=play" or "action=pause" */
    if (strstr(body, "play"))
        supervisor_avrcp_command(AVRC_CMD_PLAY);
    else if (strstr(body, "pause"))
        supervisor_avrcp_command(AVRC_CMD_PAUSE);

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static const char *PAGE_HTML =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>CarRadio — Bluetooth Pairing</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#1a1a2e;color:#eee;font-family:sans-serif;padding:20px}"
"h1{color:#e94560;margin-bottom:20px;font-size:1.5em}"
"h2{color:#4ecca3;margin:20px 0 10px;font-size:1.1em}"
"button{padding:12px 20px;background:#0f3460;color:#fff;border:none;"
"border-radius:6px;font-size:1em;cursor:pointer;margin:4px 0;width:100%}"
"button.pair{background:#e94560}"
"button:active{opacity:.8}"
"#devices{margin-top:10px}"
".dev{display:flex;justify-content:space-between;align-items:center;"
"background:#16213e;padding:10px;border-radius:6px;margin:6px 0}"
".dev span{flex:1}"
".dev small{color:#888;font-size:.8em}"
"#status{margin-top:12px;color:#4ecca3;font-size:.9em;min-height:20px}"
"input{width:100%;padding:12px;border:1px solid #333;border-radius:6px;"
"background:#16213e;color:#eee;font-size:1em;margin:4px 0}"
"</style></head><body>"
"<h1>&#127925; CarRadio — Bluetooth Pairing</h1>"
"<button onclick='startScan()'>&#128246; Scan for Bluetooth Devices</button>"
"<div id='status'>Press Scan to discover nearby speakers.</div>"
"<div id='devices'></div>"
"<h2>&#127925; Now Playing</h2>"
"<div id='nowplaying' style='background:#16213e;padding:10px;border-radius:6px;"
"margin-bottom:8px;color:#4ecca3;min-height:24px'></div>"
"<div style='display:flex;gap:8px'>"
"<button onclick='sendPlayback(\"play\")' style='background:#27ae60'>&#9654; Play</button>"
"<button onclick='sendPlayback(\"pause\")' style='background:#e94560'>&#9646;&#9646; Pause</button>"
"</div>"
"<h2>&#128421; OTA Firmware Update</h2>"
"<input type='url' id='ota-url' placeholder='https://example.com/firmware.bin'>"
"<button onclick='startOta()'>&#8593; Update Firmware</button>"
"<div id='ota-status'></div>"
"<script>"
"function pollNowPlaying(){"
"  fetch('/now_playing').then(r=>r.json()).then(d=>{"
"    document.getElementById('nowplaying').textContent=d.title||'—';"
"  }).catch(()=>{});"
"}"
"setInterval(pollNowPlaying,3000);pollNowPlaying();"
"function sendPlayback(action){"
"  fetch('/playback',{method:'POST',"
"    headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"    body:'action='+action}).catch(()=>{});"
"}"
"function startScan(){"
"  document.getElementById('status').textContent='Scanning for 8 seconds...';"
"  document.getElementById('devices').innerHTML='';"
"  fetch('/bt_scan').then(r=>r.json()).then(devs=>{"
"    var html='';"
"    devs.forEach(d=>{"
"      var btn='<button class=\"pair\" onclick=\"pairDevice(\\\"'+d.mac+'\\\",\\\"'+encodeURIComponent(d.name)+'\\\")\">"
"Pair</button>';"
"      html+='<div class=\"dev\"><span>'+d.name+'<br><small>'+d.mac+'</small></span>'+btn+'</div>';"
"    });"
"    document.getElementById('devices').innerHTML=html||'<p>No devices found.</p>';"
"    document.getElementById('status').textContent='Found '+devs.length+' device(s)';"
"  }).catch(e=>"
"    document.getElementById('status').textContent='Scan error: '+e);"
"}"
"function pairDevice(mac,encodedName){"
"  var name=decodeURIComponent(encodedName);"
"  document.getElementById('status').textContent='Pairing with '+name+'...';"
"  fetch('/bt_select',{method:'POST',"
"    headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"    body:'mac='+encodeURIComponent(mac)+'&name='+encodeURIComponent(name)"
"  }).then(r=>r.text()).then(t=>"
"    document.getElementById('status').textContent=t"
"  ).catch(e=>"
"    document.getElementById('status').textContent='Error: '+e);"
"}"
"function startOta(){"
"  var url=document.getElementById('ota-url').value;"
"  if(!url){document.getElementById('ota-status').textContent='Enter a URL first';return;}"
"  document.getElementById('ota-status').textContent='Starting OTA...';"
"  fetch('/ota_start',{method:'POST',"
"    headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"    body:'url='+encodeURIComponent(url)"
"  }).then(r=>r.text()).then(t=>"
"    document.getElementById('ota-status').textContent=t"
"  ).catch(e=>"
"    document.getElementById('ota-status').textContent='Error: '+e);"
"}"
"</script></body></html>";

/* ── GET / ───────────────────────────────────────────────────────────── */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, PAGE_HTML);
    return ESP_OK;
}

/* ── GET /bt_scan ────────────────────────────────────────────────────── */

static esp_err_t bt_scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "BT scan requested");
    bt_device_t *devices = calloc(CARRADIO_BT_MAX_DEVICES, sizeof(bt_device_t));
    if (!devices) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int count = 0;

    esp_err_t err = bluetooth_gap_scan(devices, CARRADIO_BT_MAX_DEVICES, &count);
    if (err != ESP_OK) {
        free(devices);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Build JSON array */
    int json_size = count * 80 + 32;
    char *json = malloc(json_size);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int pos = 0;
    pos += snprintf(json + pos, json_size - pos, "[");
    for (int i = 0; i < count; i++) {
        if (i > 0) pos += snprintf(json + pos, json_size - pos, ",");
        pos += snprintf(json + pos, json_size - pos,
                        "{\"name\":\"%s\",\"mac\":\"%s\"}",
                        devices[i].name, devices[i].mac_str);
    }
    pos += snprintf(json + pos, json_size - pos, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    free(devices);
    return ESP_OK;
}

/* ── POST /bt_select ─────────────────────────────────────────────────── */

static esp_err_t bt_select_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[ret] = '\0';

    char mac[STORAGE_BT_MAC_MAX]  = {0};
    char name[STORAGE_BT_NAME_MAX] = {0};

    /* Parse mac= and name= */
    char *p = body;
    while (p && *p) {
        char *eq  = strchr(p, '=');
        char *amp = strchr(p, '&');
        if (!eq) break;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        if (amp) *amp = '\0';

        /* URL-decode */
        char decoded[STORAGE_URL_MAX] = {0};
        int di = 0;
        for (int vi = 0; val[vi] && di < (int)sizeof(decoded) - 1; vi++) {
            if (val[vi] == '+') {
                decoded[di++] = ' ';
            } else if (val[vi] == '%' && val[vi+1] && val[vi+2]) {
                char hex[3] = {val[vi+1], val[vi+2], 0};
                decoded[di++] = (char)strtol(hex, NULL, 16);
                vi += 2;
            } else {
                decoded[di++] = val[vi];
            }
        }

        if (strcmp(key, "mac") == 0)  strlcpy(mac,  decoded, sizeof(mac));
        if (strcmp(key, "name") == 0) strlcpy(name, decoded, sizeof(name));
        p = amp ? amp + 1 : NULL;
    }

    ESP_LOGI(TAG, "BT select: name='%s' mac='%s'", name, mac);

    if (!storage_validate_mac(mac)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Invalid MAC address");
        return ESP_OK;
    }

    storage_set_bt_name(name);
    storage_set_bt_mac(mac);
    storage_set_phase(CARRADIO_PHASE_READY);

    httpd_resp_sendstr(req, "Paired! Device will reboot and start streaming...");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/* TaskFunction_t wrapper — FreeRTOS tasks take void* and return void */
static void ota_task_wrapper(void *arg)
{
    ota_perform_update((const char *)arg);
    free(arg);
    vTaskDelete(NULL);
}

/* ── POST /ota_start ─────────────────────────────────────────────────── */

static esp_err_t ota_start_handler(httpd_req_t *req)
{
    char body[STORAGE_URL_MAX + 16] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[ret] = '\0';

    char url[STORAGE_URL_MAX] = {0};
    char *eq = strchr(body, '=');
    if (eq) {
        /* URL-decode */
        char *val = eq + 1;
        int di = 0;
        for (int vi = 0; val[vi] && di < (int)sizeof(url) - 1; vi++) {
            if (val[vi] == '+') {
                url[di++] = ' ';
            } else if (val[vi] == '%' && val[vi+1] && val[vi+2]) {
                char hex[3] = {val[vi+1], val[vi+2], 0};
                url[di++] = (char)strtol(hex, NULL, 16);
                vi += 2;
            } else {
                url[di++] = val[vi];
            }
        }
    }

    if (!storage_validate_url(url)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Invalid URL");
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "OTA started — device will reboot when complete.");

    /* Run OTA in background task */
    char *url_copy = strdup(url);
    xTaskCreate(ota_task_wrapper, "ota", 8192, url_copy, 5, NULL);
    return ESP_OK;
}

/* ── Start ───────────────────────────────────────────────────────────── */

esp_err_t portal_phase2_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&s_server, &config));

    httpd_uri_t root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_register_uri_handler(s_server, &root);

    httpd_uri_t scan = {
        .uri     = "/bt_scan",
        .method  = HTTP_GET,
        .handler = bt_scan_handler,
    };
    httpd_register_uri_handler(s_server, &scan);

    httpd_uri_t sel = {
        .uri     = "/bt_select",
        .method  = HTTP_POST,
        .handler = bt_select_handler,
    };
    httpd_register_uri_handler(s_server, &sel);

    httpd_uri_t ota = {
        .uri     = "/ota_start",
        .method  = HTTP_POST,
        .handler = ota_start_handler,
    };
    httpd_register_uri_handler(s_server, &ota);

    httpd_uri_t np = {
        .uri     = "/now_playing",
        .method  = HTTP_GET,
        .handler = now_playing_handler,
    };
    httpd_register_uri_handler(s_server, &np);

    httpd_uri_t pb = {
        .uri     = "/playback",
        .method  = HTTP_POST,
        .handler = playback_handler,
    };
    httpd_register_uri_handler(s_server, &pb);

    ESP_LOGI(TAG, "Phase 2 portal started — http://carradio.local");
    return ESP_OK;
}

void portal_phase2_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
