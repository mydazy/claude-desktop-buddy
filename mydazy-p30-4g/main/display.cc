#include "display.h"
#include "data.h"
#include "board_config.h"

#include <string.h>
#include <stdio.h>

#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_lcd_jd9853.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

static const char *TAG = "display";

// ─── Geometry constraints ────────────────────────────────────────────────
// The 1.83" HQR180009BH panel has a 25 px corner radius. Anything within
// ~14 px of any edge can be clipped by the rounded mask. The AXS5106L FW
// V2905 also has a hard-coded edge suppression of ~25 px — touch targets
// that sit closer than that get unreliable hits. Both constraints push UI
// elements into a 234 × 190 safe-rect centered on the panel.
static const int SAFE_PAD       = 14;     // text-only safe distance
static const int TOUCH_PAD      = 26;     // touch button safe distance

// ─── LVGL widgets (mutated only on the LVGL task) ────────────────────────
static lv_obj_t *root_;
static lv_obj_t *device_name_label_;
static lv_obj_t *link_label_;
static lv_obj_t *passkey_label_;
static lv_obj_t *msg_label_;
static lv_obj_t *counts_label_;
static lv_obj_t *tokens_label_;
static lv_obj_t *entries_label_;
static lv_obj_t *prompt_panel_;
static lv_obj_t *prompt_tool_label_;
static lv_obj_t *prompt_hint_label_;
static lv_obj_t *approve_btn_;
static lv_obj_t *deny_btn_;

// ─── Backlight ───────────────────────────────────────────────────────────
static void backlight_init() {
    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .gpio_num = DISPLAY_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
}

void display_set_brightness(uint8_t pct) {
    if (pct > 100) pct = 100;
    uint32_t duty = (300 * (uint32_t)pct) / 100;     // duty cap 30% of 1023
    if (pct > 0 && duty < 30) duty = 30;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// ─── LCD bring-up ────────────────────────────────────────────────────────
static esp_lcd_panel_handle_t panel_;
static esp_lcd_panel_io_handle_t panel_io_;

static void spi_init() {
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = DISPLAY_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    // jd9853 driver creates panel_io with pclk=80 MHz + trans_queue_depth=10;
    // both require an actual DMA channel.
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void panel_init() {
    ESP_ERROR_CHECK(esp_lcd_jd9853_create_panel(
        DISPLAY_SPI_HOST, DISPLAY_LCD_CS, DISPLAY_LCD_DC, DISPLAY_LCD_RESET,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, DISPLAY_INVERT_COLOR,
        &panel_io_, &panel_));
    ESP_LOGI(TAG, "JD9853 native %dx%d (sw-rotate to %dx%d)",
             PANEL_NATIVE_W, PANEL_NATIVE_H, DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

static lv_display_t *lv_disp_;

static void lvgl_init() {
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = PANEL_NATIVE_W * 40,
        .double_buffer = true,
        .hres = PANEL_NATIVE_W,
        .vres = PANEL_NATIVE_H,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true,
            .swap_bytes = true,
        },
    };
    lv_disp_ = lvgl_port_add_disp(&disp_cfg);
    assert(lv_disp_);

    if (lvgl_port_lock(0)) {
        lv_display_set_rotation(lv_disp_, LV_DISPLAY_ROTATION_270);
        lvgl_port_unlock();
    }
}

// ─── Touch event handlers (run on LVGL task) ─────────────────────────────
static void on_approve_clicked(lv_event_t *e) {
    (void)e;
    data_send_permission("once");
}

static void on_deny_clicked(lv_event_t *e) {
    (void)e;
    data_send_permission("deny");
}

// ─── UI construction ─────────────────────────────────────────────────────
static lv_obj_t *make_decision_btn(lv_obj_t *parent, const char *label,
                                   lv_color_t fill, lv_color_t border,
                                   lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 96, 56);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, fill, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, border, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    // Pressed feedback: brighten fill briefly.
    lv_obj_set_style_bg_color(btn, lv_color_lighten(fill, LV_OPA_30), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, label);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lab, lv_color_white(), 0);
    lv_obj_center(lab);
    return btn;
}

static void ui_build() {
    if (!lvgl_port_lock(0)) return;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);

    // Root container fills the screen but pads SAFE_PAD off every edge so
    // text doesn't intersect the 25 px corner mask.
    root_ = lv_obj_create(scr);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root_, SAFE_PAD, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top row: device name (left) + link state (right) ────────────────
    device_name_label_ = lv_label_create(root_);
    lv_label_set_text(device_name_label_, "Claude");
    lv_obj_set_style_text_font(device_name_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(device_name_label_,
                                lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_align(device_name_label_, LV_ALIGN_TOP_LEFT, 0, 0);

    link_label_ = lv_label_create(root_);
    lv_label_set_text(link_label_, "starting");
    lv_obj_set_style_text_font(link_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(link_label_, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(link_label_, LV_ALIGN_TOP_RIGHT, 0, 0);

    // ── Center: heartbeat msg (large) ───────────────────────────────────
    msg_label_ = lv_label_create(root_);
    lv_label_set_text(msg_label_, "");
    lv_label_set_long_mode(msg_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(msg_label_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(msg_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(msg_label_, DISPLAY_WIDTH - SAFE_PAD * 2);
    lv_obj_align(msg_label_, LV_ALIGN_CENTER, 0, -16);

    counts_label_ = lv_label_create(root_);
    lv_label_set_text(counts_label_, "");
    lv_obj_set_style_text_font(counts_label_, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(counts_label_,
                                lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_align(counts_label_, LV_ALIGN_CENTER, 0, 18);

    // ── Bottom: entries + tokens ────────────────────────────────────────
    entries_label_ = lv_label_create(root_);
    lv_label_set_text(entries_label_, "");
    lv_label_set_long_mode(entries_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(entries_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(entries_label_,
                                lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_width(entries_label_, DISPLAY_WIDTH - SAFE_PAD * 2);
    lv_obj_align(entries_label_, LV_ALIGN_BOTTOM_LEFT, 0, -22);

    tokens_label_ = lv_label_create(root_);
    lv_label_set_text(tokens_label_, "");
    lv_obj_set_style_text_font(tokens_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(tokens_label_,
                                lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(tokens_label_, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // Passkey overlay (replaces center-msg during pairing)
    passkey_label_ = lv_label_create(root_);
    lv_label_set_text(passkey_label_, "");
    lv_obj_set_style_text_font(passkey_label_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(passkey_label_,
                                lv_color_make(0xFF, 0xC8, 0x20), 0);
    lv_obj_align(passkey_label_, LV_ALIGN_CENTER, 0, -16);
    lv_obj_add_flag(passkey_label_, LV_OBJ_FLAG_HIDDEN);

    // ── Permission prompt overlay (full-screen modal) ───────────────────
    // Sized 232×188 leaves TOUCH_PAD (26 px) clearance from every edge so
    // the AXS5106L V2905 edge-suppression doesn't drop touch events.
    prompt_panel_ = lv_obj_create(scr);
    lv_obj_remove_style_all(prompt_panel_);
    lv_obj_set_size(prompt_panel_, DISPLAY_WIDTH - TOUCH_PAD * 2,
                                   DISPLAY_HEIGHT - TOUCH_PAD * 2);
    lv_obj_align(prompt_panel_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(prompt_panel_, lv_color_make(0x1A, 0x14, 0x05), 0);
    lv_obj_set_style_bg_opa(prompt_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(prompt_panel_,
                                  lv_color_make(0xFA, 0x70, 0x20), 0);
    lv_obj_set_style_border_width(prompt_panel_, 2, 0);
    lv_obj_set_style_radius(prompt_panel_, 10, 0);
    lv_obj_set_style_pad_all(prompt_panel_, 10, 0);
    lv_obj_clear_flag(prompt_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN);

    prompt_tool_label_ = lv_label_create(prompt_panel_);
    lv_label_set_text(prompt_tool_label_, "");
    lv_obj_set_style_text_font(prompt_tool_label_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(prompt_tool_label_,
                                lv_color_make(0xFA, 0x90, 0x20), 0);
    lv_obj_align(prompt_tool_label_, LV_ALIGN_TOP_LEFT, 0, 0);

    prompt_hint_label_ = lv_label_create(prompt_panel_);
    lv_label_set_text(prompt_hint_label_, "");
    lv_label_set_long_mode(prompt_hint_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(prompt_hint_label_,
                     DISPLAY_WIDTH - TOUCH_PAD * 2 - 24);
    lv_obj_set_style_text_font(prompt_hint_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(prompt_hint_label_, lv_color_white(), 0);
    lv_obj_align(prompt_hint_label_, LV_ALIGN_TOP_LEFT, 0, 32);

    // Approve / Deny large touch buttons at the bottom of the prompt panel.
    // Green / red 96×56 each, ~20 px gap, both within touch-safe rect.
    approve_btn_ = make_decision_btn(prompt_panel_, "Allow",
                                     lv_color_make(0x1F, 0x7A, 0x2E),
                                     lv_color_make(0x35, 0xC0, 0x52),
                                     on_approve_clicked);
    lv_obj_align(approve_btn_, LV_ALIGN_BOTTOM_LEFT, 4, 0);

    deny_btn_ = make_decision_btn(prompt_panel_, "Deny",
                                  lv_color_make(0x82, 0x1B, 0x1B),
                                  lv_color_make(0xE5, 0x35, 0x35),
                                  on_deny_clicked);
    lv_obj_align(deny_btn_, LV_ALIGN_BOTTOM_RIGHT, -4, 0);

    lvgl_port_unlock();
}

void display_init() {
    backlight_init();
    spi_init();
    panel_init();
    lvgl_init();
    ui_build();
    display_set_brightness(CONFIG_CLAUDE_BUDDY_DEFAULT_BRIGHTNESS);
}

// ─── Setters (each acquires the LVGL lock briefly) ──────────────────────
void display_set_link(display_link_state_t s) {
    if (!lvgl_port_lock(50)) return;
    const char *txt = "?";
    lv_color_t color = lv_palette_main(LV_PALETTE_GREY);
    bool show_passkey = false;
    switch (s) {
        case DISPLAY_LINK_BOOT:         txt = "starting"; break;
        case DISPLAY_LINK_ADVERTISING:  txt = "not paired"; break;
        case DISPLAY_LINK_PAIRING:      txt = "pairing"; show_passkey = true;
                                        color = lv_color_make(0xFF, 0xC8, 0x20); break;
        case DISPLAY_LINK_PAIRED_IDLE:  txt = "idle";
                                        color = lv_palette_main(LV_PALETTE_GREEN); break;
        case DISPLAY_LINK_PAIRED_LIVE:  txt = "live";
                                        color = lv_palette_main(LV_PALETTE_GREEN); break;
    }
    lv_label_set_text(link_label_, txt);
    lv_obj_set_style_text_color(link_label_, color, 0);
    if (show_passkey) {
        lv_obj_clear_flag(passkey_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(msg_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(counts_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(passkey_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(msg_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(counts_label_, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

void display_set_device_name(const char *name) {
    if (!name || !lvgl_port_lock(50)) return;
    lv_label_set_text(device_name_label_, name);
    lvgl_port_unlock();
}

void display_set_passkey(uint32_t passkey) {
    if (!lvgl_port_lock(50)) return;
    if (passkey == 0) {
        lv_label_set_text(passkey_label_, "");
    } else {
        char buf[12];
        snprintf(buf, sizeof(buf), "%06u", (unsigned)passkey);
        lv_label_set_text(passkey_label_, buf);
    }
    lvgl_port_unlock();
}

void display_set_msg(const char *msg) {
    if (!msg || !lvgl_port_lock(50)) return;
    lv_label_set_text(msg_label_, msg);
    lvgl_port_unlock();
}

void display_set_counts(uint8_t total, uint8_t running, uint8_t waiting) {
    if (!lvgl_port_lock(50)) return;
    char buf[40];
    if (total == 0 && running == 0 && waiting == 0) {
        buf[0] = 0;
    } else {
        snprintf(buf, sizeof(buf), "%u total · %u run · %u wait",
                 total, running, waiting);
    }
    lv_label_set_text(counts_label_, buf);
    lvgl_port_unlock();
}

void display_set_tokens(uint32_t tokens_today) {
    if (!lvgl_port_lock(50)) return;
    char buf[32];
    if (tokens_today == 0) {
        buf[0] = 0;
    } else if (tokens_today >= 10000) {
        snprintf(buf, sizeof(buf), "%uK today", (unsigned)(tokens_today / 1000));
    } else {
        snprintf(buf, sizeof(buf), "%u today", (unsigned)tokens_today);
    }
    lv_label_set_text(tokens_label_, buf);
    lvgl_port_unlock();
}

void display_set_entries(const char *entries[], uint8_t n) {
    if (!lvgl_port_lock(50)) return;
    char buf[256];
    buf[0] = 0;
    size_t off = 0;
    uint8_t shown = (n < 3) ? n : 3;
    for (uint8_t i = 0; i < shown; i++) {
        const char *e = entries[i] ? entries[i] : "";
        int w = snprintf(buf + off, sizeof(buf) - off, "%s%s",
                         (i == 0) ? "" : "\n", e);
        if (w < 0 || (size_t)w >= sizeof(buf) - off) break;
        off += w;
    }
    lv_label_set_text(entries_label_, buf);
    lvgl_port_unlock();
}

void display_set_prompt(const char *tool, const char *hint) {
    if (!tool || !hint || !lvgl_port_lock(50)) return;
    lv_label_set_text(prompt_tool_label_, tool[0] ? tool : "permission");
    lv_label_set_text(prompt_hint_label_, hint);
    lv_obj_clear_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void display_clear_prompt() {
    if (!lvgl_port_lock(50)) return;
    lv_obj_add_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}
