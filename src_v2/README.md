# OMOTE V2 Firmware Roadmap (Touch-first, On-device Configuration)

This folder contains the V2 rewrite baseline for OMOTE V5+ (ESP32-S3).

## Purpose

OMOTE V2 is intended to be a fully on-device configurable remote.
The end state is:

- no required source edits for normal setup
- no required firmware rebuild for adding devices, commands, or activities
- reliable command execution across IR, BLE keyboard, and MQTT transports
- robust backup/restore and upgrade behavior

## Current Capability Snapshot

### Implemented now

- Separate firmware target: `omote-v2-esp32-s3`
- Runtime registries persisted in flash (`Preferences`):
- devices
- activities
- named per-device commands
- activity physical-key mappings (device + named command)
- Touch UI pages:
- `Devices`: add/remove/rename devices, edit named commands, add/remove command names
- `Activities`: add/remove/rename activities, map physical keys, define startup steps
- `Remote`: choose device and trigger generated command buttons from mapped named commands
- `Settings`: timestamped SD backup, selectable SD restore, WiFi settings (scan/select/password/connect), BLE settings (advertise/stop/disconnect/bonds), MQTT broker settings (host/port/auth/client), icon pack reload, manual time + timezone set, power settings (sleep timeout + command debounce + lift-to-wake)
- Backup behavior:
- each backup is saved as its own timestamped file
- restore flow opens a backup picker so you can choose which backup to restore
- backups are exported as both text and JSON containers for portability
- restore supports both legacy V1 text backups and current schema-backed backups
- backup/restore writes a sync hook file (`/omote_v2_sync_hook.json`) for external tooling
- Activity switching behavior:
- activity selection is managed from `Activities`
- startup actions execute on activity change
- Keymapping behavior:
- key list includes 24 physical keys
- each key shows mapping state (`(*)` or `(none)`)
- mapping targets a specific `device + named command`
- activity `Edit` keymap shows all devices from the device registry
- Remote runtime behavior:
- command buttons are generated from each device's saved named commands (no hardcoded runtime button list)
- generated command pages support pagination for larger command sets
- generated command order prioritizes common command names, then custom commands
- optional SD icon pack (`/omote_v2_icons.csv`) can override generated button labels/icons
- Companion webapp (`tools/omote-webapp/`):
- browser-based configuration over USB serial (no firmware rebuild needed)
- serial command protocol: `@@`-prefixed JSON lines, 25 commands covering device/activity CRUD, dispatch, backup, SD file transfer
- Python FastAPI server bridges WebSocket ↔ serial
- Alpine.js + Pico CSS frontend with tabs: Connection, Devices, Activities, Backup, Monitor
- device and activity editing with full command/key-binding/startup-action management
- SD backup create/list/restore, serial export/import (no SD needed), icon pack upload with progress
- live serial monitor with filter/pause
- serial activity keeps device awake (resets sleep timer)
- Status and lifecycle:
- battery percent/charging indicator in top bar
- top-bar clock (`HH:MM`) from system time
- WiFi indicator in top bar (`green` connected, `red` disconnected, `amber` when WiFi is up but MQTT is not connected)
- settings status transitions from `WiFi connecting` to `WiFi connected/disconnected/timeout`
- manual time setting available when WiFi/NTP is unavailable
- wake source status message after boot/wake
- charge protection loop with hold-time hysteresis before cutoff/resume
- sleep/activity checks active
- keypad LED update path active
- WiFi backbone:
- WiFi/MQTT HAL is enabled in V2 build target
- NTP sync is requested at boot and on WiFi reconnect
- MQTT connection attempts are skipped when broker settings are still placeholder/default
- MQTT dispatch:
- MQTT transport commands publish from `Remote` and physical key paths
- command payload format supports `topic|payload` (or topic on line 1, payload on line 2)
- command editor includes MQTT template import with merge/replace mode
- publish failures are surfaced in UI status text
- BLE dispatch:
- BLE transport commands publish from `Remote` and physical key paths
- BLE payload format supports `key:<action>`, `media:<action>`, `text:<value>`, with optional `address@...` target
- BLE pairing/bond controls are available in `Settings`
- Unified dispatch behavior:
- standardized dispatch result states (`Sent`, `No mapping`, `Invalid payload`, `Transport unavailable`, `Send failed`, `Debounced`)
- centralized command debouncing to suppress rapid-repeat accidental sends

### Current intentional limits

- WiFi is currently single-profile (one saved SSID/password); multi-profile management is not implemented yet.
- HTTP runtime dispatch path is not enabled yet in this target.

## Build and Flash

1. Build and upload:
   - `pio run -e omote-v2-esp32-s3 -t upload`
2. Basic on-device validation:
   - Add one IR device in `Devices`
   - In `Edit`, add command name + payload
   - Create activity in `Activities`
   - Map one physical key to that device command
   - Select device in `Remote` and test on-screen + physical key behavior

## Companion Webapp

A browser-based configuration tool that connects to the OMOTE over USB serial.

### Architecture

```
Browser (HTML/JS/Alpine.js)
    ↕ WebSocket
Python FastAPI server (tools/omote-webapp/)
    ↕ USB Serial (115200 baud)
ESP32-S3 firmware serial handler (src_v2/app/serial_handler.{h,cpp})
    ↕ existing registries
DeviceRegistry / ActivityRegistry / SdBackupService / CommandDispatcher
```

Each tab component (`devicesComponent`, `activitiesComponent`) is self-contained: it owns its own `meta` data and fetches it via `send('meta')` alongside its primary data load. Do not rely on the root `app()` scope for shared reactive data — Alpine.js nested `x-data` components cannot reliably access parent scope reactive properties.

### Serial Protocol

All protocol messages are single lines prefixed with `@@` and terminated by `\n`. Existing `Serial.println()` debug output does not start with `@@`, so the webapp separates protocol from logs trivially.

- Request: `@@{"cmd":"ping"}\n`
- Response: `@@{"res":"ping","ok":true,"data":{"uptime_ms":12345}}\n`

Supported commands: `ping`, `status`, `meta`, `dev_list`, `dev_get`, `dev_add`, `dev_update`, `dev_delete`, `act_list`, `act_get`, `act_add`, `act_update`, `act_delete`, `dispatch`, `backup_sd`, `backup_list`, `restore_sd`, `backup_export`, `backup_import`, `sd_write_start`, `sd_write_chunk`, `sd_write_end`, `sd_read_start`, `sd_read_chunk`, `sd_read_end`.

#### Firmware Field Names (important for webapp ↔ firmware alignment)

- **`meta` response** returns `device_types` (string array), `transport_types` (string array), `ir_protocols` (array of `{id, name}` objects), `command_slots` (string array), `common_commands` (string array).
- **Device CRUD** uses `dev_id` as the device identifier field (not `id`, which is reserved for the request correlation ID).
- **Activity CRUD** uses `act_id` as the activity identifier field.
- **`dispatch`** expects `device_id` (int) and `command` (string) — note: the field is `command`, not `command_name`.
- **Key bindings** use `key` (ASCII int), `device_id` (int), `command_name` (string).
- **Startup actions** use `device_id` (int) and `slot` (CommandSlot string, e.g. `"Power"`, `"VolumeUp"`).
- **IR protocol** can be sent as `ir_protocol` (int ID) or `ir_protocol_name` (string like `"NEC"`) — the firmware resolves either.

### Verification Steps

1. **Firmware build:** `pio run -e omote-v2-esp32-s3 -t upload`
2. **Webapp start:** `cd tools/omote-webapp && pip install -r requirements.txt && python app.py` (or `C:\Users\**useraccount**\.platformio\penv\Scripts\python.exe app.py`) → `http://localhost:8080`
3. **End-to-end:** Connect via webapp, test device list, add/edit devices

## SD Icon Pack Format

- File path: `/omote_v2_icons.csv` on SD root
- Format per line: `Command Name,LabelOrIconText`
- Example:
  - `Power,[PWR]`
  - `Volume Up,+`
  - `Mute,M`
- Use `Settings -> Reload Icon Pack` after updating the file.

## Definition of "Fully Implemented Product"

The project is considered complete when all of the following are true:

- all intended transports (IR, BLE keyboard, MQTT) can be configured and executed on-device
- a user can fully set up and maintain the remote from UI only
- generated runtime control UI is activity-driven and not hardcoded
- backup/restore and upgrade flows are reliable and version-tolerant
- sleep/wake, battery, and charging behavior are stable in daily use
- release checklist and regression tests pass on target hardware

## Ordered Implementation Phases

Work phases in order. Each phase includes dependencies and done criteria.

### Phase 0: Baseline Stabilization (Current Branch)

Status: `active baseline`

Objective:
- lock down current runtime model and UI behavior as a stable starting point

Scope:
- keep current device/activity/keymap runtime persistence
- keep named command model and mapping UX
- keep SD text backup path operational

Done when:
- baseline compiles and runs reproducibly on target hardware
- no major regressions in add/edit/remove/rename workflows

---

### Phase 1: Data Integrity and Referential Safety

Depends on: Phase 0
Status: `completed`

Objective:
- prevent stale or invalid references as users edit runtime data

Scope:
- enforce referential rules:
- deleting a device removes/updates affected activity mappings and startup steps
- deleting a command name clears affected activity key bindings for that device
- normalize IDs and ordering rules during saves/restores
- add schema version migration tests between stored versions

Done when:
- no orphaned bindings/actions after destructive edits
- restore handles old/new data schema without crashes or silent corruption

---

### Phase 2: Configuration UX Completion

Depends on: Phase 1

Objective:
- finish and polish all configuration workflows

Scope:
- improve keymap UX:
- fast filtering/search for devices/commands when large lists exist
- clear conflict/overwrite prompts when remapping keys
- improve command editor UX:
- easier payload validation feedback
- consistent keyboard/scroll behavior on all modals
- add small quality-of-life features:
- duplicate activity/device
- reorder device list and activity list

Done when:
- all core config actions are understandable without documentation
- no blocked workflows due to modal/keyboard/layout constraints

---

### Phase 3: Sleep, Wake, and Power Lifecycle Hardening

Depends on: Phase 2
Status: `completed`

Objective:
- make device power behavior production-safe

Scope:
- finalize inactivity sleep timing and wake responsiveness
- preserve user context across sleep/wake transitions
- validate charge protection thresholds and edge-case behavior
- add clear runtime indicators for sleep/charging state transitions

Done when:
- sleep/wake works consistently in long-run tests
- battery/charge behavior has no unstable toggling under normal use

---

### Phase 4: WiFi Backbone

Depends on: Phase 2 (and coordinated with Phase 3 for power impact)
Status: `completed`

Objective:
- add a reliable network foundation before MQTT features

Scope:
- UI for scan/connect/disconnect and saved network selection
- credential persistence and reconnect logic
- top-bar connection state and error states
- clock sync on connect and manual clock fallback when offline
- network lifecycle hooks for sleep/wake and boot

Done when:
- user can configure and maintain WiFi without serial console
- reconnect behavior is predictable across reboot and sleep/wake

---

### Phase 5: MQTT Backbone

Depends on: Phase 4
Status: `in progress`

Objective:
- enable runtime MQTT command execution on top of working WiFi

Scope:
- broker configuration UI (host/port/auth/client settings)
- topic/payload publish path from named commands
- connection state handling and retry behavior
- status reporting for publish success/failure

Done when:
- MQTT commands execute from `Remote` and physical key mappings
- failures are visible and diagnosable from UI status

---

### Phase 6: BLE Keyboard Backbone

Depends on: Phase 2 (and Phase 3 for power interactions)
Status: `in progress`

Objective:
- enable BLE keyboard transport as a first-class runtime command path

Scope:
- re-enable BLE build/runtime path for V2 target
- pairing/bond management UI
- mapping named commands to BLE key/media actions
- connection and retry state feedback

Done when:
- BLE commands can be configured and triggered fully on-device
- pairing and reconnect behavior is stable after reboots

---

### Phase 7: Unified Runtime Dispatch Engine

Depends on: Phases 5 and 6
Status: `completed`

Objective:
- provide one robust execution pipeline across all transports

Scope:
- dispatch abstraction with transport-specific validation
- per-command execution result model:
- sent
- not mapped
- invalid payload
- transport unavailable
- failed send
- queueing/debouncing rules for rapid key presses

Done when:
- IR, MQTT, and BLE dispatch share predictable behavior and status reporting
- no transport-specific UI hacks are required for normal operation

---

### Phase 8: On-device Learning and Command Import

Depends on: Phase 7
Status: `completed`

Objective:
- let users create commands directly on the remote at scale

Scope:
- IR learning improvements: capture, parse, preview, save
- MQTT template import flow (common topic/payload patterns)
- optional command packs for common device categories
- conflict-safe import behavior (merge/replace choices)

Done when:
- users can build most command sets from UI only
- imported/learned commands persist and map cleanly into activities

---

### Phase 9: Generated Runtime Home-Control UI

Depends on: Phase 8
Status: `completed`

Objective:
- replace fixed remote controls with UI generated from configuration

Scope:
- activity-aware generated pages and button groups
- dynamic labels from named commands
- pagination/layout rules for small screens and larger command sets
- quick activity switching and contextual controls

Done when:
- runtime control experience is generated from saved model, not hardcoded buttons

---

### Phase 10: Backup/Restore and Data Portability Completion

Depends on: Phase 9
Status: `completed`

Objective:
- make data movement and recovery production-grade

Scope:
- harden SD text backup/restore validation and error handling
- add optional JSON export/import format
- schema-version compatibility rules and migration behavior
- optional sync hooks for external tooling/web workflows
- add SD-based icon pack workflow so generated runtime pages can use custom button icons

Done when:
- users can safely move configs across firmware versions/devices
- restore failures are explicit and recoverable

---

### Phase 11: Navigation and Interaction Polish

Depends on: Phase 9

Objective:
- finalize overall interaction model

Scope:
- true circular page navigation without blank/dead-end tiles
- animation/performance tuning for smooth swipe and modal transitions
- consistency pass on typography, spacing, and status messaging

Done when:
- navigation feels continuous and polished in daily use

---

### Phase 12: Release Hardening and Validation

Depends on: Phases 1 through 11

Objective:
- prepare for production use and repeatable releases

Scope:
- end-to-end regression matrix:
- clean device
- upgraded device
- restored-from-backup device
- long-duration stability and memory checks
- release notes and migration guidance for users

Done when:
- all acceptance tests pass on target hardware
- upgrade and recovery flows are documented and validated

## Bugs

- No critical V2-specific bugs currently tracked in this file.
- Keep this section for newly discovered, reproducible runtime issues only.
