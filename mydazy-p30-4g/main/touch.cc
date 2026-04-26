#include "touch.h"
#include "board_config.h"

#include <esp_err.h>
#include <esp_log.h>
#include <driver/i2c_master.h>

#include <axs5106l_touch.h>

static const char *TAG = "touch";

static i2c_master_bus_handle_t i2c_bus_;
static axs5106l_touch_handle_t touch_;

static void i2c_init() {
    i2c_master_bus_config_t cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_I2C_SDA_PIN,
        .scl_io_num = TOUCH_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 3,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 0,    // external 10kΩ pull-ups present
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus_));
}

void touch_init() {
    i2c_init();

    // The AXS5106L driver maps reported coordinates into LVGL's coordinate
    // space. Since we sw-rotate LVGL by 270°, we feed the driver the visible
    // landscape size (DISPLAY_WIDTH × DISPLAY_HEIGHT) and let LVGL handle
    // the rotation transform internally on indev points.
    axs5106l_touch_config_t cfg = AXS5106L_TOUCH_DEFAULT_CONFIG(
        i2c_bus_, TOUCH_RST_NUM, TOUCH_INT_NUM, PANEL_NATIVE_W, PANEL_NATIVE_H);
    cfg.swap_xy = false;
    cfg.mirror_x = false;
    cfg.mirror_y = false;

    esp_err_t err = axs5106l_touch_new(&cfg, &touch_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "axs5106l_touch_new err=%d (touch disabled)", err);
        touch_ = nullptr;
        return;
    }

    err = axs5106l_touch_attach_lvgl(touch_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "attach_lvgl err=%d", err);
        axs5106l_touch_del(touch_);
        touch_ = nullptr;
        return;
    }

    ESP_LOGI(TAG, "AXS5106L attached as LVGL indev");
}
