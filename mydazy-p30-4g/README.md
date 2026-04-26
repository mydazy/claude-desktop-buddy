# claude-buddy on mydazy-p30-4g

ESP-IDF 5.5 port of the Claude desktop BLE Hardware Buddy protocol for the **mydazy-p30-4g** board (ESP32-S3R8, JD9853 LCD, AXS5106L touch, ML307R 4G).

This sub-project lives in parallel with the M5StickC Plus reference under `../src/`. Both speak the same wire protocol (`../REFERENCE.md`); see [`../PORTING_MYDAZY.md`](../PORTING_MYDAZY.md) for the rationale and roadmap.

## Status

**Phase 1 — MVP**:
- ✅ Master LDO (GPIO9) bring-up + `esp_register_shutdown_handler` (cycles GPIO9 across `esp_restart()` so the JD9853 panel always recovers)
- ✅ JD9853 1.83" 284×240 LCD via `mydazy/esp_lcd_jd9853` + LVGL 9
- ✅ NimBLE NUS service, advertise as `Claude-XXXX`, encrypted-only, LE Secure Connections + DisplayOnly IO + bonding
- ✅ Heartbeat snapshot decoder (cJSON) → status / counts / entries / tokens / prompt
- ✅ `status` / `name` / `owner` / `unpair` ack handlers
- ✅ BOOT / VOL+ / VOL− input via `espressif/button` (VOL+ = approve, VOL− = deny)

Phase 2/3/4/5 (ASCII pets, GIF push, accelerometer, sleep) are roadmapped in `PORTING_MYDAZY.md`.

## Build

You need ESP-IDF **5.5+** — earlier versions don't have `mydazy/esp_lcd_jd9853` 2.0.0's LCD ops shape.

```bash
cd mydazy-p30-4g
source idf55                                    # or set IDF_PATH manually
idf.py set-target esp32s3
idf.py build
```

First build pulls 6 components from the Espressif Component Registry into `managed_components/`:

| Component | Source |
|---|---|
| `espressif/button` | upstream button driver |
| `espressif/esp_lvgl_port` | LVGL ↔ esp_lcd glue |
| `lvgl/lvgl` | LVGL 9.2.x |
| `mydazy/esp_lcd_jd9853` | JD9853 panel driver (open-source, Apache-2.0) |
| `mydazy/esp_lcd_touch_axs5106l` | AXS5106L touch driver (Phase 4 wires it up) |

Build artifacts go to `build/`; `managed_components/` is auto-generated and is `.gitignore`d.

## Flash

```bash
# usual USB CDC after first program — same TX/RX baud as xiaozhi-esp32
idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor

# brick recovery (BOOT held during reset)
idf.py -p /dev/cu.usbmodem2101 erase-flash
idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor
```

The serial port name varies by host. On macOS it's typically `/dev/cu.usbmodem2101` once the S3 native USB-OTG enumerates; on Linux it's `/dev/ttyACM0`.

## Pair & verify

1. After flashing, the LCD lights up showing `Claude-XXXX` (top-left) and `not paired` (top-right).
2. On Claude.app: **Help → Troubleshooting → Enable Developer Mode**, then **Developer → Open Hardware Buddy…** and click **Connect**.
3. Pick `Claude-XXXX` from the picker. macOS asks for the 6-digit passkey now visible on the LCD.
4. Top-right flips to `paired · idle`. With Claude Code or Claude Cowork running, heartbeats start arriving — the center label shows `msg`, recent entries appear at the bottom, `tokens_today` accumulates.
5. Trigger any tool that needs a permission decision (Bash, file write…). The orange overlay shows the tool name and hint. Press **VOL+** (top-right edge of board) to approve, **VOL−** (next to it) to deny.

## Sanity checks

- **Empty serial output** = panel power isn't up. Check that `GPIO9` is being driven high and `rtc_gpio_hold_dis` ran (`power.cc`). On reboot, the shutdown handler would have left it held low.
- **Stuck on `starting`** = NimBLE host task didn't reach `on_sync`. Look for `nimble_port_init err=` in the boot log; usually means `CONFIG_BT_ENABLED=y` got dropped from sdkconfig.
- **Pairs but nothing displays** = `display_set_msg` is being called from inside the NimBLE host task while `lvgl_port_lock(50)` times out. The LVGL port task should have priority high enough; if your sdkconfig overrode `CONFIG_LVGL_PORT_TASK_PRIORITY`, raise it back to ≥4.
- **`esp_restart()` then black screen** = the shutdown handler didn't run. Verify with the boot log line `LDO up on GPIO9, shutdown handler armed` is followed by, on reboot, the LDO actually being driven low between resets.

## Layout

```
mydazy-p30-4g/
├── CMakeLists.txt              # IDF top-level
├── sdkconfig.defaults          # SPIRAM_OCT, NimBLE, LVGL, panic=halt
├── partitions.csv              # nvs + factory (3MB) + littlefs (12MB)
├── README.md                   # this file
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml       # component registry deps
    ├── Kconfig.projbuild       # Claude Buddy menu (brand prefix, brightness)
    ├── board_config.h          # GPIO map (subset of mydazy/config.h)
    ├── main.cc                 # app_main: power → display → BLE → input
    ├── power.{h,cc}            # GPIO9 LDO + esp_register_shutdown_handler
    ├── display.{h,cc}          # JD9853 + LVGL + Phase-1 UI tree
    ├── ble_nus.{h,cc}          # NimBLE host + NUS service + GAP pairing
    ├── data.{h,cc}             # cJSON parse + tama_state + ack writer
    └── input.{h,cc}            # espressif/button BOOT / VOL+ / VOL-
```

## Phase 4 prereq — touch driver

Touch is wired in `board_config.h` (RST=GPIO4, INT=GPIO5, shared I²C1 SDA=11/SCL=12) and the `mydazy/esp_lcd_touch_axs5106l` component is in `idf_component.yml`, but the Phase-1 firmware doesn't read it. Phase 4 will replace the BOOT/VOL inputs with on-screen approve/deny buttons in the prompt overlay.
