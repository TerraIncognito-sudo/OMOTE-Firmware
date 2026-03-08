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
- **SerialHandler** — non-blocking serial command handler (`@@`-prefixed JSON protocol). Supports 27 commands: device/activity CRUD, dispatch, IR learning (`ir_learn_start`/`ir_learn_stop` + unsolicited `ir_learned` event), SD backup/restore, serial export/import, chunked SD file transfer (base64). Resets sleep timer on serial activity. IR learning polls the receiver in `poll()` and auto-stops after one code is captured.

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

V2 is feature-complete for daily use. All core transports (IR, BLE, MQTT) are fully configurable and dispatchable on-device. Companion webapp provides browser-based device/activity management, IR learning, backup/restore, and live monitoring over USB serial. Remaining work is UX polish, navigation refinement, and release hardening. See `src_v2/README.md` for full user-facing documentation.

## Key Conventions

- Registry/Storage separation: registries hold runtime state in memory; storage modules handle NVS persistence.
- HAL filenames follow `<module>_hal_esp32.{h,cpp}` pattern.
- LVGL is configured entirely via `-D` build flags (no `lv_conf.h`); see `platformio.ini` `[env]` section.
- Device command payloads are transport-specific strings: raw hex for IR, `key:<action>`/`media:<action>`/`text:<value>` for BLE, `topic|payload` for MQTT.
- SD icon pack at `/omote_v2_icons.csv` overrides generated button labels (format: `Command Name,LabelOrIconText`).
- Serial protocol: `@@`-prefixed JSON lines over 115200 baud USB serial. See `src_v2/README.md` "Companion Webapp" section for command reference and field name mapping.
- Webapp ↔ firmware field alignment: firmware uses `dev_id`/`act_id` (not `id`) for entity identifiers in CRUD commands, `command` (not `command_name`) for dispatch, `key` (ASCII int) for key bindings, `slot` (CommandSlot string) for startup actions, and `ir_protocol_name` (string) for IR protocol. The `meta` response returns `transport_types` and `ir_protocols` (array of `{id, name}` objects).
- Webapp Alpine.js scoping: each tab component (`devicesComponent`, `activitiesComponent`) owns its own `meta` data and fetches it via `send('meta')` during its load function. Do NOT rely on parent `app()` scope for shared data — nested `x-data` components cannot reliably access parent reactive properties. Each component must be self-contained.
- Webapp Alpine.js proxy safety: when modifying objects inside reactive arrays (e.g., key bindings), always **replace the entire object** using `splice(idx, 1, newObj)` instead of mutating individual properties in place. In-place property mutations on Alpine proxy objects may silently fail to persist. Similarly, always convert `<select>` values to the correct type (e.g., `parseInt(String(val), 10)` for numeric IDs) immediately at assignment time — `x-model` on `<select>` elements returns strings, not the original `:value` type.
- Webapp browser caching: the webapp serves static JS files without cache-busting hashes. After modifying any JS file, users must hard-refresh the browser (Ctrl+Shift+R) to pick up changes. Stale JS causes Alpine expression errors like "X is not defined" even when the variable exists in the updated source. When debugging webapp issues, always rule out caching first.
- Firmware JSON parser type handling: the firmware's `extract_json_int()` only parses **unquoted** JSON numbers (e.g., `"device_id":2`). If a value is sent as a quoted string (e.g., `"device_id":"2"`), it returns the default value (typically 0). All numeric fields in serial payloads MUST be serialized as JSON numbers, not strings. Use `parseInt(String(val), 10)` on the webapp side to ensure values are JavaScript numbers before `JSON.stringify`.
- On-device status bar: the 30px LVGL status bar at the top has two rows. Row 1: WiFi status (left), time (center), battery (right). Row 2: current activity name (full width, light blue, truncated with `...`). The activity name updates every 1s tick from `selected_activity()`.
- **Keep `src_v2/README.md` and `CLAUDE.md` updated** whenever features are added or removed. `src_v2/README.md` is the user-facing documentation; `CLAUDE.md` is the developer/AI reference. Both must reflect the current state of the project after every feature change.
- Webapp visual key map: the Activities tab includes an interactive SVG overlay of the remote image (`static/img/remote.png`, sourced from the simulator's `hardware/windows_linux/keypad_gui/buttons.png`). Button polygon coordinates are in `static/js/keymap.js` (extracted from `buttons.map.json`). Key map methods are merged directly into `activitiesComponent()` to avoid Alpine.js nested scope issues.
