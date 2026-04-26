#include "power.h"
#include "board_config.h"

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "power";

// 🔴 The mydazy-p30-4g hardware ties the LCD reset, audio codec power, and
// the 4G modem's VDD_EXT to a single ME6211 LDO whose enable line is GPIO9.
// There is no independent reset for the JD9853 panel — the only way to
// recover from a corrupted GRAM after a panic / OTA / factory-reset is to
// drop the LDO and let the cap discharge.
//
// Registering this as an esp-idf shutdown handler means **every** path that
// ends in esp_restart() (panic, OTA, system_reset.cc, our own SwitchNetwork)
// runs the LDO cycle automatically — without patching any base class.
static void shutdown_handler() {
    gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
    rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);            // hold low across reset
    esp_rom_delay_us(500 * 1000);                   // 22 µF cap discharge
}

void power_init() {
    // Backlight off first — otherwise the user sees a flash of stale GRAM
    // pixels for the brief window between LDO-up and InitializeDisplay().
    gpio_reset_pin(DISPLAY_BACKLIGHT);
    gpio_set_direction(DISPLAY_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level(DISPLAY_BACKLIGHT, 0);

    // Master LDO output. RTC hold may have been left set by a prior
    // shutdown_handler — clear it before driving the line.
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << AUDIO_PWR_EN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    rtc_gpio_hold_dis(AUDIO_PWR_EN_GPIO);

    // Soft start: low for 10 ms, then high. Avoids in-rush dipping VBAT
    // below the brown-out threshold on a partially-charged battery.
    gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(AUDIO_PWR_EN_GPIO, 1);

    // ES8311/ES7210 datasheets ask for ~200 ms of stable power before any
    // I²C traffic. Even though we don't drive the codec, the rail is shared
    // with the LCD which also wants ~50 ms before SPI.
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_register_shutdown_handler(shutdown_handler);
    ESP_LOGI(TAG, "LDO up on GPIO%d, shutdown handler armed", AUDIO_PWR_EN_GPIO);
}
