#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize NimBLE host, register the Nordic UART Service, and start
// advertising. Must be called after nvs_flash_init() and after display_init()
// (so the passkey can be shown).
//
// The advertised name is "<CONFIG_CLAUDE_BUDDY_BRAND_NAME>-XXXX" where XXXX
// are the last two BT MAC bytes — matches the M5StickC reference.
void ble_nus_init();

// Send a single line of UTF-8 JSON to the connected desktop. The trailing
// '\n' is appended automatically. Drops silently if no client is subscribed.
// Safe to call from any task context.
void ble_nus_send_line(const char *line);

// Whether a client is currently connected (encrypted or not).
bool ble_nus_is_connected();

// Erase all bonds (LTKs) from NVS. Called by the desktop's "unpair" cmd
// and by factory reset. Forces a fresh passkey on next pairing.
void ble_nus_clear_bonds();

#ifdef __cplusplus
}
#endif
