#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Heartbeat snapshot (REFERENCE.md "Heartbeat snapshot")
typedef struct {
    uint8_t  total;
    uint8_t  running;
    uint8_t  waiting;
    bool     recently_completed;
    uint32_t tokens_today;
    char     msg[24];
    char     entries[3][92];          // newest first, capped to 3
    uint8_t  n_entries;
    char     prompt_id[40];           // empty = no prompt
    char     prompt_tool[20];
    char     prompt_hint[64];
    uint32_t last_update_ms;
} tama_state_t;

// Initialize the parser + UI sync. Call once after display_init().
void data_init();

// Apply a single '\n'-delimited JSON line received over BLE or USB.
// Updates tama_state_t and pushes changes to display_*.
void data_apply_line(const char *line);

// Drop all live state (called on BLE disconnect).
void data_on_disconnect();

// True if a permission decision is pending (display is highlighting).
bool data_has_pending_prompt();

// Send the recorded permission decision back to the desktop.
// `decision` must be "once" or "deny" (matches REFERENCE.md).
void data_send_permission(const char *decision);

#ifdef __cplusplus
}
#endif
