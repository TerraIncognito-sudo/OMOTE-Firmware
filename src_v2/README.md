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
- `Remote`: choose activity and trigger mapped commands
- `Settings`: timestamped SD backup, selectable SD restore, WiFi settings (scan/select/password/connect), manual time + timezone set
- Backup behavior:
- each backup is saved as its own timestamped file
- restore flow opens a backup picker so you can choose which backup to restore
- Activity switching behavior:
- selected activity is shared between `Activities` and `Remote`
- startup actions execute on activity change
- Keymapping behavior:
- key list includes 24 physical keys
- each key shows mapping state (`(*)` or `(none)`)
- mapping targets a specific `device + named command`
- Status and lifecycle:
- battery percent/charging indicator in top bar
- top-bar clock (`HH:MM`) from system time
- WiFi indicator in top bar (`green` connected, `red` disconnected)
- settings status transitions from `WiFi connecting` to `WiFi connected/disconnected/timeout`
- manual time setting available when WiFi/NTP is unavailable
- charge protection loop
- sleep/activity checks active
- keypad LED update path active
- WiFi backbone:
- WiFi/MQTT HAL is enabled in V2 build target
- NTP sync is requested at boot (when WiFi credentials are valid)

### Current intentional limits

- Runtime transport execution is effectively IR-only in this target today.
- WiFi is currently single-profile (one saved SSID/password); multi-profile management is not implemented yet.
- BLE keyboard transport is not enabled in this target yet.
- Remote page layout is still mostly fixed/static rather than generated from configuration.

## Build and Flash

1. Build:
   - `pio run -e omote-v2-esp32-s3`
2. Upload:
   - `pio run -e omote-v2-esp32-s3 -t upload`
3. Basic on-device validation:
   - Add one IR device in `Devices`
   - In `Edit`, add command name + payload
   - Create activity in `Activities`, include the device
   - Map one physical key to that device command
   - Select activity in `Remote` and test on-screen + physical key behavior

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
