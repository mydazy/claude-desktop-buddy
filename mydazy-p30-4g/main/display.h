#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_init();

// Backlight 0..100. Persisted by the caller; we only drive LEDC.
void display_set_brightness(uint8_t pct);

// ─── UI surface ──────────────────────────────────────────────────────────
// Single-screen UI for Phase 1. Each setter dispatches an update to the
// LVGL task — safe to call from any context (NimBLE, button ISR
// trampoline, main loop). All strings are copied; no caller retention.

// Connection state
typedef enum {
    DISPLAY_LINK_BOOT,         // "starting"
    DISPLAY_LINK_ADVERTISING,  // "Claude-XXXX  not paired"
    DISPLAY_LINK_PAIRING,      // shows passkey big
    DISPLAY_LINK_PAIRED_IDLE,  // "paired · waiting"
    DISPLAY_LINK_PAIRED_LIVE,  // heartbeat received recently
} display_link_state_t;

void display_set_link(display_link_state_t state);
void display_set_device_name(const char *name);   // "Claude-A1B2"
void display_set_passkey(uint32_t passkey);       // 0 hides

// Heartbeat content
void display_set_msg(const char *msg);
void display_set_counts(uint8_t total, uint8_t running, uint8_t waiting);
void display_set_tokens(uint32_t tokens_today);
void display_set_entries(const char *entries[], uint8_t n);

// Permission prompt overlay. tool/hint may be empty; pass "" to clear.
void display_set_prompt(const char *tool, const char *hint);
void display_clear_prompt();

#ifdef __cplusplus
}
#endif
