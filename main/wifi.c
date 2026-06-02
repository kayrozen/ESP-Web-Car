#include "wifi.h"
#include "config.h"
#include "storage.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_events = NULL;
static bool               s_initialized = false;
static volatile bool      s_connected   = false;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_wifi_events)
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        ESP_LOGW(TAG, "WiFi disconnected");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        if (s_wifi_events)
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t ensure_init(void)
{
    if (s_initialized) return ESP_OK;
    esp_netif_init();
    esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_start_softap(void)
{
    ensure_init();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = CARRADIO_SOFTAP_SSID,
            .ssid_len        = strlen(CARRADIO_SOFTAP_SSID),
            .channel         = CARRADIO_SOFTAP_CHANNEL,
            .password        = "",
            .max_connection  = CARRADIO_SOFTAP_MAX_CONN,
            .authmode        = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP started: SSID=%s", CARRADIO_SOFTAP_SSID);
    return ESP_OK;
}

esp_err_t wifi_connect_sta(void)
{
    ensure_init();

    char ssid[STORAGE_SSID_MAX] = {0};
    char pass[STORAGE_PASS_MAX] = {0};
    if (storage_get_wifi_ssid(ssid, sizeof(ssid)) != ESP_OK || !storage_validate_ssid(ssid)) {
        ESP_LOGE(TAG, "No valid SSID saved");
        return ESP_ERR_INVALID_STATE;
    }
    storage_get_wifi_pass(pass, sizeof(pass));

    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_start();
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        wifi_mdns_init();
        return ESP_OK;
    }
    return ESP_FAIL;
}

void wifi_stop(void)
{
    esp_wifi_stop();
    s_connected = false;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_mdns_init(void)
{
    mdns_init();
    mdns_hostname_set(CARRADIO_MDNS_HOSTNAME);
    mdns_instance_name_set(CARRADIO_MDNS_INSTANCE);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", CARRADIO_MDNS_HOSTNAME);
    return ESP_OK;
}
