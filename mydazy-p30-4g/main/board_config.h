// Board GPIO map for mydazy-p30-4g.
// Subset of xiaozhi-esp32/main/boards/mydazy-p30-4g/config.h — only
// what claude-buddy actually needs (display + touch + buttons + power gate).
#pragma once

#include <driver/gpio.h>

// ─── Master LDO (cascade: LCD + audio + 4G VDD_EXT) ───
// 🔴 Drop this to 0 to reset the JD9853 panel — see PORTING_MYDAZY.md.
#define AUDIO_PWR_EN_GPIO       GPIO_NUM_9

// ─── Display (SPI2, JD9853 panel, 284x240 landscape) ───
#define DISPLAY_SPI_HOST        SPI2_HOST
#define DISPLAY_SPI_MOSI        GPIO_NUM_38
#define DISPLAY_SPI_SCLK        GPIO_NUM_47
#define DISPLAY_LCD_CS          GPIO_NUM_39
#define DISPLAY_LCD_DC          GPIO_NUM_48
#define DISPLAY_LCD_RESET       GPIO_NUM_NC      // shared via AUDIO_PWR_EN_GPIO
#define DISPLAY_LCD_TE          GPIO_NUM_40      // reserved, VSYNC not used
#define DISPLAY_BACKLIGHT       GPIO_NUM_41

// Panel hardware is left in native 240×284 portrait. We rotate to landscape
// in LVGL (sw_rotate) instead of via MADCTL — the JD9853 driver's swap_xy
// flips MADCTL.MV but doesn't widen the CASET range, so a hardware-rotated
// 284×240 frame writes columns 240..283 outside the panel's short axis,
// producing color noise in those rows.
#define PANEL_NATIVE_W          240
#define PANEL_NATIVE_H          284

#define DISPLAY_WIDTH           284   // visible canvas after sw_rotate
#define DISPLAY_HEIGHT          240
#define DISPLAY_MIRROR_X        false
#define DISPLAY_MIRROR_Y        false
#define DISPLAY_SWAP_XY         false
#define DISPLAY_INVERT_COLOR    false

// ─── Touch (AXS5106L on shared I²C1, 0x63) ───
#define TOUCH_I2C_PORT          I2C_NUM_1
#define TOUCH_I2C_SDA_PIN       GPIO_NUM_11
#define TOUCH_I2C_SCL_PIN       GPIO_NUM_12
#define TOUCH_I2C_SPEED_HZ      (400 * 1000)
#define TOUCH_RST_NUM           GPIO_NUM_4
#define TOUCH_INT_NUM           GPIO_NUM_5

// ─── Buttons ───
#define BOOT_BUTTON_GPIO        GPIO_NUM_0       // multi-function (menu / wake)
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_42      // claude: APPROVE
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_45      // claude: DENY

// ─── Battery (Phase 4) ───
#define BATTERY_ADC_GPIO        GPIO_NUM_8
#define BATTERY_CHRG_GPIO       GPIO_NUM_21      // open-drain, low = charging
