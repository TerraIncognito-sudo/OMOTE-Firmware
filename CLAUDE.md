# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

OMOTE is an ESP32-based open-source universal remote firmware supporting IR, BLE keyboard, and MQTT transports. The active development target is **V2** (`src_v2/`), a touch-first rewrite enabling fully on-device configuration without firmware rebuilds. V1 code in `src/` is retained as reference only.

## Build Commands (PlatformIO)

```bash
# Build V2 firmware (primary development target)
pio run -e omote-v2-esp32-s3

# Upload V2 to device
pio run -e omote-v2-esp32-s3 -t upload

# Build legacy V1 targets
pio run -e esp32-Rev1toRev4          # Hardware rev 1-4
pio run -e esp32-s3-Rev5andHigher    # Hardware rev 5+

# Desktop simulators (SDL2-based)
pio run -e linux_64bit
pio run -e windows_64bit
pio run -e macOS

# Serial monitor
pio device monitor -b 115200
```

There is no test suite or linter configured in this project.

## Architecture

### Hardware Abstraction Layer (`hardware/`)

Platform-agnostic interfaces with ESP32 and desktop (Windows/Linux) implementations. Each HAL module has a paired `.h`/`.cpp` (e.g., `battery_hal_esp32.h/cpp`). The entry point `hardware/hardwareLayer.h` selects the appropriate platform implementation.

Key HAL modules: battery, infrared sender/receiver, keypad (5x5 matrix), BLE keyboard, TFT display (LovyanGFX), LVGL init, MQTT (PubSubClient), SD card (SdFat), sleep/wake, NVS preferences storage.

Bundled libraries live in `hardware/ESP32/lib/` (Keypad scanning, ESP32-BLE-Keyboard).

### V2 Application (`src_v2/`)

**Entry point:** `main.cpp` (Arduino `setup()`/`loop()`)

**App layer** (`src_v2/app/`):
- **DeviceRegistry** / **DeviceStorage** — runtime registry of devices persisted to NVS flash. Each device has a type, transport (IR/BLE/MQTT/HTTP), protocol, and a vector of named commands (name→payload pairs).
- **ActivityRegistry** / **ActivityStorage** — runtime registry of activities persisted to NVS. Each activity has device references, physical key→(device, command) bindings, and startup action sequences.
- **CommandDispatcher** — unified dispatch engine across all transports. Returns typed `DispatchResult` (Sent, NotMapped, InvalidPayload, TransportUnavailable, SendFailed, Debounced). Enforces debounce interval (default 140ms).
- **SdBackupService** — backup/restore to SD in text + JSON formats with schema migration support. Writes a sync hook file (`/omote_v2_sync_hook.json`). Also exposes `serialize_to_text()` / `parse_from_text()` static methods for serial backup export/import without SD.
- **SerialHandler** — non-blocking serial command handler (`@@`-prefixed JSON protocol). Supports 25 commands: device/activity CRUD, dispatch, SD backup/restore, serial export/import, chunked SD file transfer (base64). Resets sleep timer on serial activity.

**UI layer** (`src_v2/ui/`):
- **SetupUi** — single large LVGL controller managing 4 tabs (Devices, Activities, Remote, Settings) plus modal flows for editing. Screen is 240x320. Uses LVGL v8.3.

### Build Configuration (`platformio.ini`)

The V2 target `omote-v2-esp32-s3` extends `esp32-s3-Rev5andHigher` and uses an explicit `build_src_filter` that cherry-picks specific HAL files from `hardware/ESP32/` plus all of `src_v2/`. Adding a new HAL module to V2 requires adding it to this filter.

Key build flags: `ENABLE_WIFI_AND_MQTT=1`, `ENABLE_KEYBOARD_BLE=1`, `ARDUINO_LOOP_STACK_SIZE=24576`. ESP32-S3 uses PSRAM for LVGL (128KB pool).

### Companion Webapp (`tools/omote-webapp/`)

Browser-based configuration tool over USB serial. Python FastAPI server (`app.py`) bridges WebSocket ↔ serial via `serial_bridge.py`. Frontend is Alpine.js + Pico CSS (no build step) with tabs: Connection, Devices, Activities, Backup, Monitor. All files in `tools/omote-webapp/static/`.

Start: `cd tools/omote-webapp && pip install -r requirements.txt && python app.py` → `http://localhost:8080`

### Data Flow Pattern

Physical keys and touch UI → ActivityRegistry (resolve key binding) → CommandDispatcher (validate, debounce, route by transport) → HAL sender (IR/BLE/MQTT).

Webapp: Browser → WebSocket → Python bridge → USB serial → SerialHandler → registries/dispatcher.

## V2 Development Status

Phases 0-1, 3-5, 7-10 completed. Phases 5-6 (MQTT/BLE) functional but labeled in-progress. Phases 2, 11, 12 (UX polish, navigation, release hardening) are pending. Companion webapp (serial handler + Python server + frontend) implemented. Full roadmap and current capability snapshot in `src_v2/README.md`.

## Key Conventions

- Registry/Storage separation: registries hold runtime state in memory; storage modules handle NVS persistence.
- HAL filenames follow `<module>_hal_esp32.{h,cpp}` pattern.
- LVGL is configured entirely via `-D` build flags (no `lv_conf.h`); see `platformio.ini` `[env]` section.
- Device command payloads are transport-specific strings: raw hex for IR, `key:<action>`/`media:<action>`/`text:<value>` for BLE, `topic|payload` for MQTT.
- SD icon pack at `/omote_v2_icons.csv` overrides generated button labels (format: `Command Name,LabelOrIconText`).
- Serial protocol: `@@`-prefixed JSON lines over 115200 baud USB serial. See `src_v2/README.md` "Companion Webapp" section for command reference.
- **Keep `src_v2/README.md` updated** when making V2 changes — it is the canonical capability snapshot, roadmap, build instructions, and webapp documentation.
