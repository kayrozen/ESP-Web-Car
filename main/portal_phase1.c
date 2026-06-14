#include "portal_phase1.h"
#include "config.h"
#include "storage.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_http_server.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "portal1";

static httpd_handle_t s_server = NULL;

/* ── DNS hijack task ─────────────────────────────────────────────────── */
/* Listens on UDP port 53, replies with the SoftAP IP for every query    */

#define DNS_PORT 53
#define DNS_MAX_LEN 512

static void dns_hijack_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS hijack listening on port 53");

    uint8_t buf[DNS_MAX_LEN];
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);

    /* Hardcode the SoftAP gateway IP: 192.168.4.1 */
    uint32_t ap_ip = htonl(0xC0A80401UL);  /* 192.168.4.1 */

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;  /* Too short to be a valid DNS packet */

        /* Build a minimal DNS response: copy the query, set QR=1, ANCOUNT=1 */
        uint8_t resp[DNS_MAX_LEN];
        memcpy(resp, buf, len);
        resp[2] = 0x81;  /* QR=1 OPCODE=0 AA=0 TC=0 RD=1 */
        resp[3] = 0x80;  /* RA=1 */
        resp[6] = 0x00;  /* ANCOUNT hi */
        resp[7] = 0x01;  /* ANCOUNT lo = 1 */

        /* Append answer record pointing to AP IP */
        uint8_t answer[] = {
            0xC0, 0x0C,             /* name: pointer to offset 12 (question name) */
            0x00, 0x01,             /* type A */
            0x00, 0x01,             /* class IN */
            0x00, 0x00, 0x00, 0x3C, /* TTL 60s */
            0x00, 0x04,             /* rdlength 4 */
            (uint8_t)(ntohl(ap_ip) >> 24),
            (uint8_t)(ntohl(ap_ip) >> 16),
            (uint8_t)(ntohl(ap_ip) >>  8),
            (uint8_t)(ntohl(ap_ip)      ),
        };

        int resp_len = len;
        if (resp_len + (int)sizeof(answer) < DNS_MAX_LEN) {
            memcpy(resp + resp_len, answer, sizeof(answer));
            resp_len += sizeof(answer);
        }

        sendto(sock, resp, resp_len, 0, (struct sockaddr *)&client, client_len);
    }
}

/* ── HTML page ───────────────────────────────────────────────────────── */

/* PAGE_HTML is built dynamically in root_get_handler to include device name / playlist */
static const char *PAGE_HTML_HEAD =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>CarRadio Setup</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#1a1a2e;color:#eee;font-family:sans-serif;padding:20px}"
"h1{color:#e94560;margin-bottom:20px;font-size:1.5em}"
"h2{color:#4ecca3;font-size:1em;margin:16px 0 8px}"
"label{display:block;margin:12px 0 4px;font-size:.9em;color:#aaa}"
"input,select{width:100%;padding:12px;border:1px solid #333;border-radius:6px;"
"background:#16213e;color:#eee;font-size:1em}"
"button{width:100%;padding:14px;margin-top:20px;background:#e94560;color:#fff;"
"border:none;border-radius:6px;font-size:1em;cursor:pointer}"
"button:active{background:#c73652}"
"#scan-btn{background:#0f3460;margin-top:8px}"
"#status{margin-top:12px;color:#4ecca3;font-size:.9em}"
".playlist-list{list-style:none;margin:8px 0}"
".playlist-list li{padding:8px;background:#16213e;border-radius:4px;"
"margin-bottom:4px;font-size:.9em;color:#ccc}"
"</style></head><body>"
"<h1>&#127925; CarRadio Setup</h1>";

static const char *PAGE_HTML_FORM =
"<form id='f' method='POST' action='/save'>"
"<label>WiFi Network</label>"
"<select id='ssid-sel' name='ssid'><option value=''>-- select --</option></select>"
"<button type='button' id='scan-btn' onclick='scanWifi()'>&#128246; Scan</button>"
"<label>WiFi Password</label>"
"<input type='password' name='password' placeholder='leave empty for open network'>"
"<button type='submit'>Save &amp; Continue &#8594;</button>"
"</form>"
"<div id='status'></div>"
"<script>"
"function scanWifi(){"
"  document.getElementById('status').textContent='Scanning...';"
"  fetch('/wifi_scan').then(r=>r.json()).then(nets=>{"
"    var sel=document.getElementById('ssid-sel');"
"    sel.innerHTML='<option value=\"\">-- select --</option>';"
"    nets.forEach(n=>{"
"      var o=document.createElement('option');"
"      o.value=o.textContent=n.ssid;"
"      sel.appendChild(o);"
"    });"
"    document.getElementById('status').textContent='Found '+nets.length+' networks';"
"  }).catch(e=>{"
"    document.getElementById('status').textContent='Scan error: '+e;"
"  });"
"}"
"window.onload=scanWifi;"
"</script></body></html>";

/* ── /wifi_scan handler ──────────────────────────────────────────────── */

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    /* Start a passive scan */
    wifi_scan_config_t scan_cfg = {
        .ssid       = NULL,
        .bssid      = NULL,
        .channel    = 0,
        .show_hidden = true,
        .scan_type  = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_wifi_scan_start(&scan_cfg, true);  /* blocking */

    uint16_t count = 20;
    wifi_ap_record_t records[20];
    memset(records, 0, sizeof(records));
    esp_wifi_scan_get_ap_records(&count, records);

    char *json = malloc(count * 64 + 32);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int pos = 0;
    pos += sprintf(json + pos, "[");
    for (int i = 0; i < count; i++) {
        if (i > 0) pos += sprintf(json + pos, ",");
        /* Escape double-quotes in SSID (simple approach) */
        pos += sprintf(json + pos, "{\"ssid\":\"%s\",\"rssi\":%d}",
                       (char *)records[i].ssid, records[i].rssi);
    }
    pos += sprintf(json + pos, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

/* ── / GET handler ───────────────────────────────────────────────────── */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HTML_HEAD);

    /* Show device name if set */
    char dev_name[STORAGE_DEVICE_NAME_MAX] = {0};
    if (storage_get_device_name(dev_name, sizeof(dev_name)) == ESP_OK && dev_name[0]) {
        char name_html[160];
        snprintf(name_html, sizeof(name_html),
                 "<p style='color:#aaa;font-size:.9em;margin-bottom:12px'>"
                 "Device: <b style='color:#4ecca3'>%s</b></p>", dev_name);
        httpd_resp_sendstr_chunk(req, name_html);
    }

    /* Show current playlist if available */
    char *playlist_json = malloc(2048);
    if (playlist_json &&
        storage_get_playlist_json(playlist_json, 2048) == ESP_OK && playlist_json[0]) {
        httpd_resp_sendstr_chunk(req, "<h2>Current Playlist</h2><ul class='playlist-list'>");
        /* Parse and list station names */
        cJSON *arr = cJSON_Parse(playlist_json);
        if (arr && cJSON_IsArray(arr)) {
            cJSON *item;
            int i = 0;
            cJSON_ArrayForEach(item, arr) {
                cJSON *nm = cJSON_GetObjectItemCaseSensitive(item, "name");
                if (cJSON_IsString(nm)) {
                    char li[128];
                    snprintf(li, sizeof(li), "<li>%d. %s</li>", ++i, nm->valuestring);
                    httpd_resp_sendstr_chunk(req, li);
                }
            }
            cJSON_Delete(arr);
        }
        httpd_resp_sendstr_chunk(req, "</ul>");
        httpd_resp_sendstr_chunk(req,
            "<p style='color:#aaa;font-size:.85em;margin:8px 0 16px'>"
            "Playlist was set during installation via the install page. "
            "Re-flash to change it.</p>");
    }
    free(playlist_json);

    httpd_resp_sendstr_chunk(req, PAGE_HTML_FORM);
    httpd_resp_sendstr_chunk(req, NULL);  /* end chunked */
    return ESP_OK;
}

/* ── /save POST handler ──────────────────────────────────────────────── */

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[ret] = '\0';

    /* Parse URL-encoded body: ssid=...&password=... */
    char ssid[STORAGE_SSID_MAX]  = {0};
    char pass[STORAGE_PASS_MAX]  = {0};

    /* Simple key=value parser */
    char *p = body;
    while (p && *p) {
        char *eq  = strchr(p, '=');
        char *amp = strchr(p, '&');
        if (!eq) break;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        if (amp) *amp = '\0';

        /* URL-decode val in place (simple: %XX → char, + → space) */
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

        if (strcmp(key, "ssid") == 0)          strlcpy(ssid, decoded, sizeof(ssid));
        else if (strcmp(key, "password") == 0) strlcpy(pass, decoded, sizeof(pass));

        p = amp ? amp + 1 : NULL;
    }

    ESP_LOGI(TAG, "Save: ssid='%s'", ssid);

    /* Validate */
    if (!storage_validate_ssid(ssid)) {
        const char *err_html = "<html><body>Error: invalid SSID. <a href='/'>Back</a></body></html>";
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, err_html);
        return ESP_OK;
    }

    /* Persist */
    storage_set_wifi_ssid(ssid);
    storage_set_wifi_pass(pass);
    storage_set_phase(CARRADIO_PHASE_BT);

    /* Respond then reboot */
    const char *ok_html =
        "<html><body style='background:#1a1a2e;color:#eee;font-family:sans-serif;padding:20px'>"
        "<h2 style='color:#4ecca3'>Saved! Rebooting...</h2>"
        "<p>The device will restart and connect to your WiFi network.<br>"
        "Then visit <b>http://carradio.local</b> to pair a Bluetooth speaker.</p>"
        "</body></html>";
    httpd_resp_sendstr(req, ok_html);

    /* Slight delay so response is sent before reboot */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/* ── /generate_204, /hotspot-detect.html redirects (captive portal) ── */

static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

/* ── Start ───────────────────────────────────────────────────────────── */

esp_err_t portal_phase1_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.uri_match_fn     = httpd_uri_match_wildcard;

    ESP_ERROR_CHECK(httpd_start(&s_server, &config));

    httpd_uri_t root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
    };
    httpd_register_uri_handler(s_server, &root);

    httpd_uri_t scan = {
        .uri      = "/wifi_scan",
        .method   = HTTP_GET,
        .handler  = wifi_scan_handler,
    };
    httpd_register_uri_handler(s_server, &scan);

    httpd_uri_t save = {
        .uri      = "/save",
        .method   = HTTP_POST,
        .handler  = save_post_handler,
    };
    httpd_register_uri_handler(s_server, &save);

    /* Captive portal redirects */
    httpd_uri_t redir = {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = redirect_handler,
    };
    httpd_register_uri_handler(s_server, &redir);

    /* Start DNS hijack task */
    xTaskCreatePinnedToCore(dns_hijack_task, "dns_hijack", 3072, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Phase 1 portal started");
    return ESP_OK;
}

void portal_phase1_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
