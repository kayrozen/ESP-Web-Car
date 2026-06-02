#include "playlist.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "playlist";

/* ── NVS helpers ─────────────────────────────────────────────────────── */

static esp_err_t nvs_open_rw(nvs_handle_t *handle)
{
    return nvs_open(CARRADIO_NVS_NAMESPACE, NVS_READWRITE, handle);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void playlist_load_from_nvs(playlist_t *out)
{
    memset(out, 0, sizeof(*out));
    out->count   = 0;
    out->current = 0;

    nvs_handle_t h;
    if (nvs_open_rw(&h) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open NVS — empty playlist");
        return;
    }

    /* Read playlist_idx */
    uint8_t idx = 0;
    nvs_get_u8(h, NVS_KEY_PLAYLIST_IDX, &idx);

    /* Read playlist_json */
    size_t json_len = 0;
    esp_err_t err = nvs_get_str(h, NVS_KEY_PLAYLIST_JSON, NULL, &json_len);
    if (err != ESP_OK || json_len == 0) {
        ESP_LOGW(TAG, "No playlist_json in NVS — empty playlist");
        nvs_close(h);
        return;
    }

    char *json_buf = malloc(json_len);
    if (!json_buf) {
        ESP_LOGE(TAG, "OOM reading playlist_json");
        nvs_close(h);
        return;
    }

    err = nvs_get_str(h, NVS_KEY_PLAYLIST_JSON, json_buf, &json_len);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_str playlist_json failed: %d", err);
        free(json_buf);
        return;
    }

    cJSON *arr = cJSON_Parse(json_buf);
    free(json_buf);

    if (!arr || !cJSON_IsArray(arr)) {
        ESP_LOGE(TAG, "playlist_json is not a valid JSON array");
        cJSON_Delete(arr);
        return;
    }

    int n = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (n >= PLAYLIST_MAX) break;

        playlist_entry_t *e = &out->entries[n];

        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *url  = cJSON_GetObjectItemCaseSensitive(item, "url");
        if (!cJSON_IsString(name) || !cJSON_IsString(url)) continue;

        strlcpy(e->name, name->valuestring, sizeof(e->name));
        strlcpy(e->url,  url->valuestring,  sizeof(e->url));

        cJSON *uuid    = cJSON_GetObjectItemCaseSensitive(item, "stationuuid");
        cJSON *codec   = cJSON_GetObjectItemCaseSensitive(item, "codec");
        cJSON *bitrate = cJSON_GetObjectItemCaseSensitive(item, "bitrate");
        cJSON *source  = cJSON_GetObjectItemCaseSensitive(item, "source");

        if (cJSON_IsString(uuid) && uuid->valuestring[0])
            strlcpy(e->stationuuid, uuid->valuestring, sizeof(e->stationuuid));
        if (cJSON_IsString(codec) && codec->valuestring[0])
            strlcpy(e->codec, codec->valuestring, sizeof(e->codec));
        if (cJSON_IsNumber(bitrate))
            e->bitrate_kbps = (uint16_t)bitrate->valuedouble;
        if (cJSON_IsString(source))
            e->is_custom = (strcmp(source->valuestring, "custom_url") == 0);

        n++;
    }

    cJSON_Delete(arr);

    out->count   = n;
    out->current = (n > 0 && idx < n) ? idx : 0;

    ESP_LOGI(TAG, "Loaded %d station(s), current=%d", n, out->current);
}

void playlist_save_to_nvs(const playlist_t *p)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return;

    for (int i = 0; i < p->count; i++) {
        const playlist_entry_t *e = &p->entries[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name",        e->name);
        cJSON_AddStringToObject(obj, "url",         e->url);
        cJSON_AddStringToObject(obj, "stationuuid", e->stationuuid);
        cJSON_AddStringToObject(obj, "codec",       e->codec);
        cJSON_AddNumberToObject(obj, "bitrate",     e->bitrate_kbps);
        cJSON_AddStringToObject(obj, "source",      e->is_custom ? "custom_url" : "radio_browser");
        cJSON_AddItemToArray(arr, obj);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json_str) return;

    nvs_handle_t h;
    if (nvs_open_rw(&h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_PLAYLIST_JSON, json_str);
        nvs_set_u8(h,  NVS_KEY_PLAYLIST_IDX,  (uint8_t)p->current);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGD(TAG, "Playlist saved (%d entries, current=%d)", p->count, p->current);
    }

    free(json_str);
}

const playlist_entry_t *playlist_current(const playlist_t *p)
{
    if (p->count == 0) return NULL;
    return &p->entries[p->current];
}

void playlist_next(playlist_t *p)
{
    if (p->count == 0) return;
    p->current = (p->current + 1) % p->count;
    playlist_save_to_nvs(p);
    ESP_LOGI(TAG, "Next station: %d/%d (%s)", p->current, p->count - 1,
             p->entries[p->current].name);
}

void playlist_prev(playlist_t *p)
{
    if (p->count == 0) return;
    p->current = (p->current == 0) ? (p->count - 1) : (p->current - 1);
    playlist_save_to_nvs(p);
    ESP_LOGI(TAG, "Prev station: %d/%d (%s)", p->current, p->count - 1,
             p->entries[p->current].name);
}
