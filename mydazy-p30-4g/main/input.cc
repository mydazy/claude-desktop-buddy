#include "input.h"
#include "board_config.h"
#include "data.h"
#include "display.h"

#include <esp_log.h>
#include <iot_button.h>
#include <button_gpio.h>

static const char *TAG = "input";

// ─── Helpers ─────────────────────────────────────────────────────────────
static button_handle_t make_button(int gpio, bool active_low) {
    const button_config_t cfg = { 0 };
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = gpio,
        .active_level = static_cast<uint8_t>(active_low ? 0 : 1),
        .enable_power_save = false,
        .disable_pull = false,
    };
    button_handle_t h = NULL;
    esp_err_t err = iot_button_new_gpio_device(&cfg, &gpio_cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create button gpio=%d err=%d", gpio, err);
        return NULL;
    }
    return h;
}

// ─── Click callbacks ─────────────────────────────────────────────────────
static void on_volup_click(void *btn, void *user) {
    (void)btn; (void)user;
    if (data_has_pending_prompt()) {
        ESP_LOGI(TAG, "VOL+ -> approve");
        data_send_permission("once");
    } else {
        ESP_LOGI(TAG, "VOL+ (no prompt) - ignored");
    }
}

static void on_voldown_click(void *btn, void *user) {
    (void)btn; (void)user;
    if (data_has_pending_prompt()) {
        ESP_LOGI(TAG, "VOL- -> deny");
        data_send_permission("deny");
    } else {
        ESP_LOGI(TAG, "VOL- (no prompt) - ignored");
    }
}

static void on_boot_click(void *btn, void *user) {
    (void)btn; (void)user;
    ESP_LOGI(TAG, "BOOT click (reserved for Phase 4 menu)");
}

void input_init() {
    button_handle_t boot = make_button(BOOT_BUTTON_GPIO, true);
    button_handle_t vup  = make_button(VOLUME_UP_BUTTON_GPIO, true);
    button_handle_t vdn  = make_button(VOLUME_DOWN_BUTTON_GPIO, true);

    if (boot) iot_button_register_cb(boot, BUTTON_SINGLE_CLICK, NULL, on_boot_click, NULL);
    if (vup)  iot_button_register_cb(vup,  BUTTON_SINGLE_CLICK, NULL, on_volup_click, NULL);
    if (vdn)  iot_button_register_cb(vdn,  BUTTON_SINGLE_CLICK, NULL, on_voldown_click, NULL);
}
