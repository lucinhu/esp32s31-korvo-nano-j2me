#include "korvo_settings.h"
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define SETTINGS_VERSION 0x4b4a0001u
static nvs_handle_t handle;
static bool opened, have_saved;
static korvo_settings saved;

static bool valid(const korvo_settings *s)
{
    return s->version == SETTINGS_VERSION && s->width >= 64 && s->width <= 320 &&
        s->height >= 64 && s->height <= 320 && s->volume <= 100 && s->fps <= 1 &&
        s->pixel_perfect <= 1 && s->jit_mode <= 2 && s->jit_internal <= 1;
}

esp_err_t korvo_settings_load(korvo_settings *out)
{
    *out = (korvo_settings){.version = SETTINGS_VERSION, .width = 240, .height = 320,
        .volume = 70, .fps = 0, .pixel_perfect = 0, .jit_mode = 0, .jit_internal = 0};
    esp_err_t rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW("korvo-settings", "Reinitializing incompatible NVS: %s", esp_err_to_name(rc));
        rc = nvs_flash_erase();
        if (rc == ESP_OK) rc = nvs_flash_init();
    }
    if (rc != ESP_OK) return rc;
    rc = nvs_open("korvo-j2me", NVS_READWRITE, &handle);
    if (rc != ESP_OK) return rc;
    opened = true;
    korvo_settings loaded;
    size_t length = sizeof loaded;
    rc = nvs_get_blob(handle, "settings", &loaded, &length);
    if (rc == ESP_OK && length == sizeof loaded && valid(&loaded)) {
        *out = saved = loaded;
        have_saved = true;
    }
    return ESP_OK;
}

esp_err_t korvo_settings_save(const korvo_settings *settings)
{
    if (!opened) return ESP_ERR_INVALID_STATE;
    if (!valid(settings)) return ESP_ERR_INVALID_ARG;
    if (have_saved && !memcmp(&saved, settings, sizeof saved)) return ESP_OK;
    /* One versioned blob and one commit retain a consistent preference set.
     * NVS supplies wear levelling and CRC validation. */
    esp_err_t rc = nvs_set_blob(handle, "settings", settings, sizeof *settings);
    if (rc == ESP_OK) rc = nvs_commit(handle);
    if (rc == ESP_OK) { saved = *settings; have_saved = true; }
    return rc;
}
