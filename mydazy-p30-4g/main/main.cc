// Phase-1 entry for claude-desktop-buddy on mydazy-p30-4g.
// See PORTING_MYDAZY.md for the full plan and the four upcoming phases.

#include "ble_nus.h"
#include "data.h"
#include "display.h"
#include "input.h"
#include "power.h"
#include "touch.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

static const char *TAG = "main";

extern "C" void app_main() {
    ESP_LOGI(TAG, "claude-buddy mydazy-p30-4g booting");

    // NVS first (NimBLE bond storage + buddy/owner names live here)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    // 🔴 LDO up + ShutdownHandler armed BEFORE display / BLE init.
    power_init();

    display_init();
    display_set_link(DISPLAY_LINK_BOOT);

    touch_init();        // attaches AXS5106L as LVGL indev
    data_init();
    ble_nus_init();
    input_init();        // VOL+/VOL- kept as redundant approve/deny path

    ESP_LOGI(TAG, "boot complete");

    // Idle the main task — all work happens in NimBLE host task,
    // LVGL task, and button task.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
