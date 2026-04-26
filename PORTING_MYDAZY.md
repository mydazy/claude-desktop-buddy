# Porting to mydazy-p30-4g (ESP32-S3)

This document explains how the M5StickC Plus reference implementation under `src/` was forked to run on the **mydazy-p30-4g** board — an ESP32-S3R8 with 1.83" JD9853 LCD, AXS5106L capacitive touch, SC7A20H accelerometer, dual-mic + ES8311/ES7210 codec, ML307R Cat.1 4G, and a single shared LDO power tree on GPIO9.

The port lives in [`mydazy-p30-4g/`](mydazy-p30-4g/) as an **independent ESP-IDF 5.5 project**. The original Arduino sources under `src/` are untouched — they remain the maintained reference for M5StickC Plus.

> **Why two trees instead of multi-board PlatformIO?** mydazy-p30-4g requires JD9853, AXS5106L, Octal PSRAM, NimBLE host, LVGL 9 and `esp_register_shutdown_handler` — all of which are first-class in ESP-IDF and are absent or hostile under the Arduino framework. Forking to a sibling tree is the path of least friction; both share `REFERENCE.md` as the protocol contract.

## What stays the same

The only stable surface is the BLE **wire protocol** in [`REFERENCE.md`](REFERENCE.md):

- Nordic UART Service UUIDs (advertise prefix `Claude`, RX/TX characteristics, `\n`-delimited UTF-8 JSON)
- Heartbeat snapshot fields (`total`/`running`/`waiting`/`msg`/`entries`/`tokens`/`prompt`)
- Permission decisions (`{"cmd":"permission","id":...,"decision":"once|deny"}`)
- One-shot commands (`status`/`name`/`owner`/`unpair`/`time`)
- Folder-push transport (`char_begin` → `file`/`chunk`/`file_end` → `char_end`)

The desktop app does not care whether the device is an M5StickC, an mydazy-p30-4g, or a Raspberry Pi — as long as the wire bytes match `REFERENCE.md`.

## What changes

| Concern | Arduino (src/) | IDF (mydazy-p30-4g/) |
|---|---|---|
| Build system | PlatformIO | ESP-IDF 5.5+ + idf_component_manager |
| BLE host | Arduino BLEDevice / Bluedroid | NimBLE (IDF native) |
| LCD | M5StickC ST7789 + TFT_eSPI | JD9853 via `mydazy/esp_lcd_jd9853` + LVGL 9 |
| Touch | n/a (button-only) | AXS5106L via `mydazy/esp_lcd_touch_axs5106l` |
| Buttons | M5 A/B/Power | espressif/button (BOOT, VOL+, VOL−) |
| IMU | M5 internal MPU6886 | SC7A20H via `mydazy/esp_sc7a20h` |
| Filesystem | LittleFS (Arduino lib) | esp_littlefs |
| NVS | Arduino Preferences | esp_nvs (raw) |
| JSON | ArduinoJson | cJSON (IDF builtin) |
| Backlight | M5.Axp.ScreenBreath | LEDC PWM on GPIO41 |
| Reboot LCD recovery | n/a | `esp_register_shutdown_handler` cycles GPIO9 LDO |
| Power tree | independent rails | shared GPIO9 LDO (LCD + audio + 4G) |

## Phased plan

| Phase | Scope | Status |
|---|---|---|
| **1 — MVP** | LCD on, BLE advertise + bond, heartbeat decode + render, BOOT/VOL approve/deny, ShutdownHandler | ✅ implemented |
| **2 — Pets** | Port the 18 ASCII species to LVGL canvas (fixed-width font, 200ms tick), persona state machine matching Arduino logic | ⏳ planned |
| **3 — GIF push** | `char_begin`/`file`/`chunk` over BLE → esp_littlefs → LVGL `lv_gif` decoder | ⏳ planned |
| **4 — Polish** | SC7A20H shake/face-down (dizzy + nap), battery ADC + charge-detect display, info pages, settings menu, brightness/volume keys ramp | ⏳ planned |
| **5 — Power** | PowerSaveTimer (5-min idle → light/deep sleep), EXT0/EXT1 wake | ⏳ planned |

This document, the IDF tree, and the M5StickC tree all evolve in lock-step — bumping the protocol means updating `REFERENCE.md`, both implementations, and `tools/test_*.py`.

## Phase 1 architecture

```
mydazy-p30-4g/main/
  ├── main.cc          # app_main: power → display → BLE → input → loop
  ├── board_config.h   # GPIO macros (subset of mydazy-p30-4g/config.h)
  ├── power.{h,cc}     # GPIO9 LDO bring-up + esp_register_shutdown_handler
  ├── display.{h,cc}   # JD9853 panel + LVGL port + status/prompt UI
  ├── ble_nus.{h,cc}   # NimBLE GAP+GATT, NUS service, line-buffered RX/TX
  ├── data.{h,cc}      # cJSON parse, TamaState, ack serializer, time sync
  └── input.{h,cc}     # espressif/button: BOOT (menu), VOL+ (approve), VOL- (deny)
```

```
   USB / boot
       │
       ▼
   power_init()  ──── GPIO9 LDO up, register ShutdownHandler
       │
       ▼
   display_init() ─── JD9853 SPI panel, LVGL port, mount UI
       │
       ▼
   ble_nus_init() ─── NimBLE host, NUS service, advertise "Claude-XXXX"
       │
       ▼
   input_init()   ─── BOOT / VOL+ / VOL− buttons
       │
       ▼
   ┌── main loop (FreeRTOS task) ──────────────────┐
   │                                                │
   │  ble_nus drains RX → data_apply_line()         │
   │       │                                         │
   │       ▼                                         │
   │  TamaState updated → display_render(state)     │
   │                                                │
   │  on button: input → permission decision        │
   │       │                                         │
   │       ▼                                         │
   │  ble_nus_send(json) → desktop                  │
   └────────────────────────────────────────────────┘
```

## Build & flash

See [`mydazy-p30-4g/README.md`](mydazy-p30-4g/README.md) for the exact `idf.py` commands, sdkconfig flags, and partition layout.

Quick path:

```bash
cd mydazy-p30-4g
source idf55                                    # ESP-IDF 5.5+
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor
```

## Pairing & verification

1. Flash, power on. The LCD shows `Claude Buddy` + `Claude-XXXX` + `not paired`.
2. On macOS Claude.app: **Help → Troubleshooting → Enable Developer Mode**, then **Developer → Open Hardware Buddy…** and pick `Claude-XXXX`.
3. macOS prompts for the 6-digit passkey shown on the LCD; enter it.
4. The LCD switches to `paired · waiting` and starts receiving heartbeats. The session msg field renders centered.
5. With Claude Code running, trigger a permission-required tool call. The LCD highlights the prompt; press **VOL+** to approve or **VOL−** to deny. The decision flows back to Claude Code immediately.

## Hardware caveats inherited from mydazy-p30-4g

These are documented in `xiaozhi-esp32/main/boards/mydazy-p30-4g/HARDWARE.md` and apply equally here:

- 🔴 **GPIO9 cascades LCD + audio + 4G VDD_EXT.** Reboot path uses `esp_register_shutdown_handler` to drop GPIO9 + `rtc_gpio_hold_en` across `esp_restart()` so the JD9853 GRAM resets cleanly.
- 🔴 **Octal PSRAM occupies GPIO 33–37.** Do not repurpose them.
- 🟡 **PSRAM-stack tasks crash if scheduled during NVS / flash ops.** Tasks running on Core 0 that may execute during NVS writes (the BLE host task, anything calling `nvs_set_*`) must use internal RAM stacks.
- 🟡 **I²C is shared 4-way (codec + touch + sensor + ML307R indirect).** AXS5106L driver retries 3× with 5/10/20 ms back-off; we keep that behavior.
- 🟡 **Touch firmware V2905 has hard-coded edge suppression.** Approve/deny touch zones (added in Phase 4) need a ≥25 px margin.
