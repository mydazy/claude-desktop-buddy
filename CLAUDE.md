# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is (and isn't)

This is a **reference implementation** of the Claude desktop apps' BLE Hardware Buddy protocol. The protocol (`REFERENCE.md`) is the stable surface; the firmware trees are just devices that speak it.

Two trees coexist:
- `src/` — original M5StickC Plus implementation, **Arduino + PlatformIO**. The upstream-maintained reference.
- `mydazy-p30-4g/` — ESP32-S3 port for the mydazy-p30-4g board, **ESP-IDF 5.5 + LVGL + NimBLE**. Forked per `CONTRIBUTING.md`'s "fork it and make it yours". See `PORTING_MYDAZY.md` for the rationale and phase plan.

Per `CONTRIBUTING.md`, the upstream maintainers (Anthropic) **will not accept**: new features, new pets, new screens, ports to other boards, refactors, style changes, or dependency bumps to `src/`. They **will** accept fixes to `REFERENCE.md` and bug fixes that make `src/` *not work as a reference* (pairing, rendering, boot crashes). The `mydazy-p30-4g/` tree is downstream-only — it lives here because the protocol is shared, not because it ships back upstream.

Default for feature work: if it's a new species / new screen / new behavior on M5StickC, suggest a fork. If it's protocol-touching, update `REFERENCE.md` + both trees in lockstep.

## Build & flash — M5StickC Plus (`src/`, PlatformIO)

The single env in `platformio.ini` is `m5stickc-plus` (Arduino framework, espressif32 platform, LittleFS, no-OTA partitions, 160 MHz). All commands run from the repo root.

```bash
pio run                         # compile only
pio run -t upload               # flash firmware over USB
pio run -t erase                # wipe flash before re-flashing (use after factory reset)
pio run -t uploadfs             # flash whatever's in data/ to LittleFS
pio device monitor              # serial console at 115200 baud (CORE_DEBUG_LEVEL=0)
```

Iterating on a GIF character pack — bypass the BLE drop target:

```bash
python3 tools/prep_character.py <source-dir-or-zip>   # 96px-wide normalize → characters/<name>/
python3 tools/flash_character.py characters/bufo      # stages → data/characters/<name>/ → uploadfs
```

Smoke tests against a connected stick (auto-discovers `/dev/cu.usbserial-*`):

```bash
python3 tools/test_serial.py    # cycles state JSON every 3s, watch the screen react
python3 tools/test_xfer.py      # streams a character pack over serial, verifies acks
```

There is no unit test harness, no linter config, and no CI. Verification = compile + flash + watch the device.

## Build & flash — mydazy-p30-4g (`mydazy-p30-4g/`, ESP-IDF)

Independent ESP-IDF 5.5+ project. Components are pulled by `idf_component_manager` from the registry (`mydazy/esp_lcd_jd9853`, `mydazy/esp_lcd_touch_axs5106l`, `lvgl/lvgl`, `espressif/esp_lvgl_port`, `espressif/button`).

```bash
cd mydazy-p30-4g
source idf55                              # or any IDF 5.5+ env
idf.py set-target esp32s3                 # one-time
idf.py build
idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor
```

The full bring-up sequence and pairing walk-through is in `mydazy-p30-4g/README.md`. The phase plan (Phase 1 MVP shipped, Phases 2–5 roadmapped) is in `PORTING_MYDAZY.md`.

When working in this tree, do not "share" code with `src/` — they're different toolchains. Cross-tree consistency is enforced at the protocol layer (`REFERENCE.md`) and the smoke tests under `tools/test_*.py`, which can drive either device by serial or BLE.

## Architecture (the things that span files)

The wire protocol in `REFERENCE.md` is the ground truth. Everything in `src/` is one implementation of it. When changing protocol-touching code, update `REFERENCE.md` in the same change — that doc is what other devices implement against.

### Data flow

```
Serial (USB) ─┐
              ├─→ data.h::dataPoll() → JSON line buffer → _applyJson() → TamaState
BLE NUS RX ───┘                                    │
                                                   └─→ xfer.h::xferCommand()  (cmd dispatch:
                                                          name/owner/species/unpair/status,
                                                          char_begin/file/chunk/file_end/char_end,
                                                          permission)

main.cpp loop:
  TamaState ──→ persona state machine ──→ buddy.cpp (ASCII) or character.cpp (GIF)
                                       └→ UI screens (NORMAL / PET / INFO / menu / approval)
                                       └→ BLE TX (acks, status snapshots, permission decisions)
```

`data.h` and `xfer.h` are **header-only with file-static state** — they must be included from exactly one TU (`main.cpp`). Including from a second `.cpp` produces duplicate symbols. `stats.h` has the same constraint.

### Personas, species, GIFs

- `PersonaState` (in `main.cpp`) is the 7-state enum: sleep/idle/busy/attention/celebrate/dizzy/heart. Same order everywhere.
- ASCII species: 18 files in `src/buddies/<name>.cpp`, each exposing 7 state functions in `PersonaState` order, registered in `buddy.cpp`'s `SPECIES_TABLE`. Adding/removing species means editing both the file list in `platformio.ini`'s `build_src_filter` and the table.
- GIF mode: a character pack is a `manifest.json` + 96px-wide GIFs in `/characters/<name>/` on LittleFS, decoded by `AnimatedGIF`. The same 7 state names map to filenames (or arrays that rotate).
- "Current pet" is a single NVS byte (`species` key). `0xFF` = use installed GIF; `0..N-1` = ASCII species index. `nextPet()` in `main.cpp` cycles `GIF (if installed) → species 0 → ... → species N-1 → GIF`.

### Storage layout

- **NVS** (`Preferences` namespace `buddy`): stats, settings, owner name, pet name, species choice. `stats.h` saves on significant events only — never on a timer (NVS sectors have ~100K write cycles).
- **LittleFS**: `/characters/<name>/manifest.json` + GIFs. Whole-folder cap is 1.8 MB. Only one character lives on the device at a time; `char_begin` wipes everything under `/characters/` after a fit-check.

### BLE specifics

- Advertises as `Claude-XXXX` (last two BT MAC bytes) over Nordic UART Service. The `Claude` prefix is what the desktop's picker filters on — don't change it.
- NUS characteristics are encrypted-only with LE Secure Connections bonding (DisplayOnly IO capability, 6-digit passkey). Bonds persist in NVS. `bleClearBonds()` wipes them; called from `unpair` cmd and factory reset.
- `bleConnected()` lies about traffic — `data.h` tracks `_lastBtByteMs` separately to know if the link is actually live.
- Writes from desktop are line-buffered (`\n`-terminated JSON) and dispatched through the same `_applyJson` path as USB serial.

## ESP32 / Arduino constraints worth knowing

- The `M5.Lcd` is 135×240 portrait; sprite (`spr`) is the same size. Render to the sprite, then `pushSprite(0,0)`.
- Buddy art is drawn at 1× (peek mode in PET/INFO screens) or 2× (home screen) — species files write 1× coords and `buddy.cpp` transforms. Don't hardcode pixel positions in species files; use `BUDDY_X_CENTER`, `BUDDY_Y_OVERLAY`, etc. from `buddy_common.h`.
- Render gating: `buddyTick()` only redraws when state changes or the 200 ms tick advances. Calling `buddyInvalidate()` forces a redraw on the next tick (e.g. after species swap, mode change).
- The xfer protocol acks **every chunk** — LittleFS writes can block on flash erase, and the UART RX buffer is only ~256 bytes. Without per-chunk acks the sender overruns it. Don't "optimize" this away.
- Arduino BLE library can't cleanly tear down the BLE stack. The `bluetooth: off` setting is a stored preference only; advertising stays live.

## When editing the protocol

The `REFERENCE.md` doc is consumed by external implementers (Pi Pico W, nRF52, etc.). For any change that affects the wire format:

1. Update `REFERENCE.md` first — payload shape, field meanings, ack contract.
2. Implement on the device side (`data.h::_applyJson` for inbound, `xfer.h::_xAck` and the manual `snprintf` builders for outbound).
3. Smoke-test with `tools/test_serial.py` or `tools/test_xfer.py` over USB before touching BLE.

The status ack in `xfer.h::xferCommand` uses manual `snprintf` instead of ArduinoJson serialization — less heap churn, and the shape is fixed. Keep it that way.
