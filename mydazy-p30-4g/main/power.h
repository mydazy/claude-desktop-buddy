#pragma once

// Bring up the master LDO on GPIO9 (LCD + audio + 4G VDD_EXT) and register
// the ESP-IDF shutdown handler that cycles GPIO9 across esp_restart() so
// the JD9853 GRAM is reset to a known state on every reboot.
//
// Must be called once, before InitializeDisplay().
void power_init();
