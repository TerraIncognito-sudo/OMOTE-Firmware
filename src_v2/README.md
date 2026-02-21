# OMOTE V2 Rewrite (Touch-first, On-device Configuration)

This folder contains the new rewrite baseline for OMOTE V5+ (ESP32-S3), focused on ease of use.

## What is implemented now

- Separate firmware target: `omote-v2-esp32-s3`
- Runtime device registry persisted in flash (`Preferences`)
- Runtime activity registry persisted in flash (`Preferences`)
- Touchscreen swipe pages (linear navigation):
- `Devices`: add/remove devices, set protocol, map command slots
- `Activities`: add/remove activities, map physical keys, build startup action steps
- `Remote`: select activity and send mapped commands
- `Devices` and `Activities` now support renaming from the UI
- Device command mapping now uses named commands per device:
- New devices start with blank command mappings.
- In `Devices -> Edit`, use the command dropdown `Add New` option to choose from a common list or `Custom`, then save payload for that named command.
- `Settings`: SD backup, SD restore, and SD quick format
- Current page order: `Devices -> Activities -> Remote -> Settings`
- Runtime command dispatch:
- IR commands are executed on-device from saved mappings

## Why this matters

The legacy firmware requires editing and compiling scene/device files on a computer.  
V2 starts from a runtime data model so the remote can be configured without rebuilding firmware.

## Current scope limits

- BLE/MQTT/HTTP dispatch is not implemented yet (UI fields exist for future support)
- Activity editor currently supports assigning first 4 devices only
- Migration from legacy scene files is not yet implemented

## Build and test upload

1. Build:
   - `pio run -e omote-v2-esp32-s3`
2. Upload to connected OMOTE V5 hardware:
   - `pio run -e omote-v2-esp32-s3 -t upload`
3. First-run test checklist on remote:
   - Add one IR device in `Devices`
   - Set at least one command slot payload (`Power` suggested)
   - Create an activity in `Activities` and include that device
   - In `Remote`, select the activity and press the mapped command button

## Current test-mode behavior

- V2 currently boots in a touchscreen-first safe mode:
- Touchscreen + GUI + IR sending + flash persistence are enabled.
- Motion sleep/wake is enabled:
- inactivity triggers sleep mode after timeout.
- wake is handled by movement or button wake sources.
- Battery telemetry is enabled:
- top status bar now shows battery percentage and charging indicator (`+`).
- charge-protection loop monitors battery state periodically.
- On V5, charge cutoff now uses TP4056 `CE` control from firmware.
- Note: no dedicated TP4056 charge-status pin is routed to MCU on V5, so charging state is estimated from SOC + CE state.
- Persistence:
- Device/activity/command/keymap data now persists in NVS using key/value records.
- Activity switching behavior:
- The selected activity is shared between `Activities` and `Remote`.
- Changing activity in either place updates the same active context for on-screen commands and physical key dispatch.
- On activity switch, startup actions run for the newly selected activity and key mappings are activity-specific.
- Activity keymap source clarity:
- In `Activities -> Keymap`, each physical key maps to a specific `device + named command`.
- Mapping hints show command and source device, so duplicate names (e.g. multiple `Power`) remain clear.
- SD backup/restore:
- Use `Settings -> Backup to SD` to write `/omote_v2_backup.txt` on the onboard SD card.
- Use `Settings -> Restore from SD` to reload records from SD into runtime + NVS.
- Use `Settings -> Format SD (Quick)` to recreate a clean OMOTE backup file structure on SD.
- Notes for firmware updates:
- Normal firmware upload keeps NVS data.
- Full chip erase (or explicit erase flags) will clear NVS.
- If NVS is cleared, you can restore from SD backup.

## Next implementation phases

1. Add protocol drivers for dynamic runtime command execution (IR, BLE keyboard, MQTT).
2. Add on-device command learning/import for IR and MQTT topic templates.
3. Add Activity Builder UI (started: startup action steps per activity implemented).
4. Add runtime home-control UI generated from configured activities and commands.
5. Add optional JSON backup format and optional web sync.
6. Implement Wifi functionality (connect to wifi networks, add status to top menu bar)
7. Battery status and charge protection (implemented on V5 with TP4056 CE control)
8. Fully imnplement backup/restore to SD card
9. Implement sleep/screen turn off functionality when not moved for a period of time, wakes up when picked up or moved (started)
10. Implement true circular page navigation without blank tiles

## Bugs
- No critical V2-specific bugs tracked in this section currently.
