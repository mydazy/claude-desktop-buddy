#pragma once

// Wire BOOT (GPIO0), VOL+ (GPIO42), VOL− (GPIO45) to:
//   VOL+ single click → approve pending permission
//   VOL- single click → deny    pending permission
//   BOOT single click → reserved (Phase 4: menu / wake)
//
// Uses espressif/button v4 (pulled via idf_component.yml).
void input_init();
