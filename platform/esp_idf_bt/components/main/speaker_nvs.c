#include "speaker_internal.h"
#include <esp_log.h>
#include <nvs.h>
#include <string.h>

static const char *TAG = "eaf_speaker";
static nvs_handle_t settings;
static bool settings_open, settings_dirty;
static int64_t save_at;

static void save_settings(void) {
    if (!settings_open)
        return;
    if (speaker.have_peer && !speaker.forgetting)
        speaker_check("save peer",
                      nvs_set_blob(settings, "peer", speaker.last_peer, sizeof(speaker.last_peer)));
    speaker_check("commit settings", nvs_commit(settings));
    settings_dirty = false;
}

void speaker_nvs_init(void) {
    if (nvs_open("eaf_bt", NVS_READWRITE, &settings) == ESP_OK) {
        settings_open = true;
        size_t length = sizeof(speaker.last_peer);
        speaker.have_peer =
            nvs_get_blob(settings, "peer", speaker.last_peer, &length) == ESP_OK && length == 6;
        ESP_LOGI(TAG, "restored peer=%d", speaker.have_peer);
    }
}

void speaker_nvs_touch(void) {
    settings_dirty = true;
    save_at = speaker_now_ms() + 2000;
}

void speaker_nvs_tick(int64_t now) {
    if (settings_dirty && now >= save_at)
        save_settings();
}

void speaker_nvs_forget_peer(void) {
    if (!settings_open)
        return;
    esp_err_t err = nvs_erase_key(settings, "peer");
    if (err != ESP_ERR_NVS_NOT_FOUND)
        speaker_check("forget peer", err);
    speaker_check("save forget", nvs_commit(settings));
}
