#include "avrcp.h"
#include "supervisor.h"
#include "telemetry.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_avrc_api.h"

static const char *TAG = "avrcp";

/* ── Metadata storage ────────────────────────────────────────────────── */

#define META_TITLE_MAX   97
#define META_ARTIST_MAX  97
#define META_GENRE_MAX   33

static char s_title[META_TITLE_MAX]   = {0};
static char s_artist[META_ARTIST_MAX] = {0};
static char s_genre[META_GENRE_MAX]   = {0};

static SemaphoreHandle_t s_meta_mutex = NULL;

/* Track which notification event IDs the car has registered for.
   Bit N = car registered for event ID N. */
static uint32_t s_ntf_registered_mask = 0;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static uint8_t playback_state_to_avrc(playback_state_t state)
{
    switch (state) {
        case PLAYBACK_STATE_PLAYING:     return ESP_AVRC_PLAYBACK_PLAYING;
        case PLAYBACK_STATE_SOFT_PAUSED: return ESP_AVRC_PLAYBACK_PAUSED;
        case PLAYBACK_STATE_HARD_PAUSED: return ESP_AVRC_PLAYBACK_STOPPED;
        default:                          return ESP_AVRC_PLAYBACK_ERROR;
    }
}

/* ── AVRCP TG callback ───────────────────────────────────────────────── */

static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {

        case ESP_AVRC_TG_CONNECTION_STATE_EVT: {
            if (param->conn_stat.connected) {
                ESP_LOGI(TAG, "AVRCP connected");
                s_ntf_registered_mask = 0;
                /* Push current metadata immediately so the car display updates */
                avrcp_publish_playback_status(supervisor_get_playback_state());
            } else {
                ESP_LOGI(TAG, "AVRCP disconnected");
                s_ntf_registered_mask = 0;
            }
            break;
        }

        case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
            ESP_LOGD(TAG, "AVRCP remote features: 0x%"PRIx32,
                     (uint32_t)param->rmt_feats.feat_mask);
            break;

        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT: {
            uint8_t key  = param->psth_cmd.key_code;
            uint8_t kst  = param->psth_cmd.key_state;
            /* Act only on key-pressed, ignore key-released */
            if (kst != ESP_AVRC_PT_CMD_STATE_PRESSED) break;

            ESP_LOGI(TAG, "PASSTHROUGH key=0x%02x", key);
            {
                char pt_payload[48];
                snprintf(pt_payload, sizeof(pt_payload),
                         "{\"command\":\"0x%02x\",\"state\":%d}", key, kst);
                telemetry_log("avrcp_passthrough", pt_payload);
            }
            switch (key) {
                case ESP_AVRC_PT_CMD_PLAY:
                    supervisor_avrcp_command(AVRC_CMD_PLAY);
                    break;
                case ESP_AVRC_PT_CMD_PAUSE:
                    supervisor_avrcp_command(AVRC_CMD_PAUSE);
                    break;
                case ESP_AVRC_PT_CMD_STOP:
                    supervisor_avrcp_command(AVRC_CMD_STOP);
                    break;
                case ESP_AVRC_PT_CMD_FORWARD:
                    supervisor_avrcp_command(AVRC_CMD_NEXT_STATION);
                    break;
                case ESP_AVRC_PT_CMD_BACKWARD:
                    supervisor_avrcp_command(AVRC_CMD_PREV_STATION);
                    break;
                default:
                    ESP_LOGD(TAG, "PASSTHROUGH key 0x%02x ignored", key);
                    break;
            }
            break;
        }

        case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT: {
            esp_avrc_rn_event_ids_t ev = param->reg_ntf.event_id;
            esp_avrc_rn_param_t rn_param = {0};

            s_ntf_registered_mask |= (1u << ev);

            /* Respond with interim (current state) */
            switch (ev) {
                case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
                    rn_param.playback = playback_state_to_avrc(
                        supervisor_get_playback_state());
                    esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_PLAY_STATUS_CHANGE,
                                             ESP_AVRC_RN_RSP_INTERIM, &rn_param);
                    break;

                case ESP_AVRC_RN_TRACK_CHANGE:
                    /* UID 0 = current track */
                    memset(rn_param.elm_id, 0, sizeof(rn_param.elm_id));
                    esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_TRACK_CHANGE,
                                             ESP_AVRC_RN_RSP_INTERIM, &rn_param);
                    break;

                default:
                    esp_avrc_tg_send_rn_rsp(ev, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
                    break;
            }
            break;
        }

        case ESP_AVRC_TG_GET_ELEM_ATTR_EVT: {
            /* Car is requesting track metadata */
            xSemaphoreTake(s_meta_mutex, portMAX_DELAY);

            esp_avrc_elem_attr_t attrs[4];
            int n = 0;

#define ADD_ATTR(id, str) do {                          \
    if (str[0] != '\0') {                               \
        attrs[n].attr_id  = (id);                       \
        attrs[n].char_set = 0x006A; /* UTF-8 */         \
        attrs[n].str_len  = (uint16_t)strlen(str);      \
        attrs[n].p_str    = (uint8_t *)(str);           \
        n++;                                            \
    }                                                   \
} while(0)

            ADD_ATTR(ESP_AVRC_MD_ATTR_TITLE,  s_title);
            ADD_ATTR(ESP_AVRC_MD_ATTR_ARTIST, s_artist);
            ADD_ATTR(ESP_AVRC_MD_ATTR_GENRE,  s_genre);

#undef ADD_ATTR

            if (n == 0) {
                /* Nothing — send a single blank title */
                static const uint8_t empty[] = {0};
                attrs[0].attr_id  = ESP_AVRC_MD_ATTR_TITLE;
                attrs[0].char_set = 0x006A;
                attrs[0].str_len  = 0;
                attrs[0].p_str    = (uint8_t *)empty;
                n = 1;
            }

            esp_avrc_get_ele_attr_rsp(n, attrs);
            xSemaphoreGive(s_meta_mutex);
            break;
        }

        case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
            /* Car-side amp controls its own volume — no action needed */
            ESP_LOGD(TAG, "Set absolute volume: %u", param->set_abs_vol.volume);
            break;

        default:
            ESP_LOGD(TAG, "AVRC TG event %d", event);
            break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t avrcp_init(void)
{
    s_meta_mutex = xSemaphoreCreateMutex();
    if (!s_meta_mutex) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_avrc_tg_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "avrc_tg_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_avrc_tg_register_callback(avrc_tg_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "avrc_tg_register_callback failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Declare supported passthrough commands so the car enables next/prev buttons */
    esp_avrc_psth_bit_mask_t psth = {0};
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, ESP_AVRC_PT_CMD_PLAY);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, ESP_AVRC_PT_CMD_PAUSE);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, ESP_AVRC_PT_CMD_STOP);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, ESP_AVRC_PT_CMD_FORWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, ESP_AVRC_PT_CMD_BACKWARD);
    esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORT_CMD, &psth);

    ESP_LOGI(TAG, "AVRCP TG initialised");
    return ESP_OK;
}

void avrcp_publish_metadata(const char *title, const char *artist, const char *genre)
{
    if (!s_meta_mutex) return;

    xSemaphoreTake(s_meta_mutex, portMAX_DELAY);
    if (title)  strlcpy(s_title,  title,  sizeof(s_title));
    if (artist) strlcpy(s_artist, artist, sizeof(s_artist));
    if (genre)  strlcpy(s_genre,  genre,  sizeof(s_genre));
    xSemaphoreGive(s_meta_mutex);

    /* Log metadata push */
    {
        char meta_hash[33] = {0};
        if (title) telemetry_hash_id(title, meta_hash, sizeof(meta_hash));
        char payload[96];
        snprintf(payload, sizeof(payload),
                 "{\"title_hash\":\"%s\",\"has_artist\":%s}",
                 meta_hash, (artist && artist[0]) ? "true" : "false");
        telemetry_log("avrcp_metadata_sent", payload);
    }

    /* Send track-change notification if car has registered for it */
    if (s_ntf_registered_mask & (1u << ESP_AVRC_RN_TRACK_CHANGE)) {
        esp_avrc_rn_param_t rn_param = {0};
        /* UID 0 = "unknown" single track — valid for streaming radio */
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_TRACK_CHANGE,
                                  ESP_AVRC_RN_RSP_CHANGED, &rn_param);
        /* Car will re-register and call GetElementAttributes */
        s_ntf_registered_mask &= ~(1u << ESP_AVRC_RN_TRACK_CHANGE);
    }
}

void avrcp_publish_playback_status(playback_state_t state)
{
    if (!s_meta_mutex) return;

    if (s_ntf_registered_mask & (1u << ESP_AVRC_RN_PLAY_STATUS_CHANGE)) {
        esp_avrc_rn_param_t rn_param = {0};
        rn_param.playback = playback_state_to_avrc(state);
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_PLAY_STATUS_CHANGE,
                                  ESP_AVRC_RN_RSP_CHANGED, &rn_param);
        s_ntf_registered_mask &= ~(1u << ESP_AVRC_RN_PLAY_STATUS_CHANGE);
    }
}
