# OMOTE V2 Firmware

The V2 firmware is a complete rewrite of the OMOTE universal remote. It replaces the original hardcoded approach with a fully configurable system — you set up all your devices, commands, and remote layouts directly on the device or through a companion web app, with no code changes or firmware rebuilds required.

## What's Different from V1

| | V1 | V2 |
|---|---|---|
| **Adding devices** | Edit source code, recompile, reflash | Add directly on the touchscreen or via webapp |
| **Adding commands** | Hardcode IR codes in source | Type or learn IR codes at runtime |
| **Remote layout** | Fixed buttons in code | Auto-generated from your saved commands |
| **Configuration storage** | Compiled into firmware | Saved to flash, survives updates |
| **Backup/Restore** | Not available | SD card or serial export/import |
| **PC companion** | Not available | Browser-based webapp over USB |
| **Hardware support** | Rev 1-5+ | Rev 5+ (ESP32-S3) only |

V1 source code is still in `src/` if you need it as reference. V2 lives in `src_v2/`.

## Supported Hardware

V2 targets **OMOTE hardware revision 5 and higher** using the ESP32-S3 chip. Earlier revisions (Rev 1-4) should continue using the V1 firmware.

You need:
- OMOTE Rev 5+ hardware ([OMOTE Hardware repo](https://github.com/OMOTE-Community/OMOTE-Hardware/))
- USB-C cable for flashing and serial communication
- [PlatformIO](https://platformio.org/) installed (VS Code extension recommended)

## Getting Started

### 1. Install PlatformIO

Install [VS Code](https://code.visualstudio.com/) and the [PlatformIO extension](https://platformio.org/install/ide?install=vscode), or install the [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation.html) standalone.

### 2. Clone and Build

```bash
git clone <this-repo-url>
cd OMOTE-Firmware

# Build the V2 firmware
pio run -e omote-v2-esp32-s3

# Flash it to your OMOTE (connect via USB-C first)
pio run -e omote-v2-esp32-s3 -t upload
```

### 3. First Boot Setup

When V2 boots for the first time, you'll see a touchscreen interface with four tabs:

1. **Devices** — Add your TV, amplifier, media player, etc.
2. **Activities** — Group devices and map physical keys to commands
3. **Remote** — The main control screen with auto-generated buttons
4. **Settings** — WiFi, BLE, MQTT, backups, power, clock

Here's how to get your first device working:

1. Go to **Devices** and tap **+ Add**
2. Give it a name (e.g. "Living Room TV"), pick a type and transport (IR, BLE, or MQTT)
3. Tap the device to open it, then add commands (e.g. "Power", "Volume Up")
4. For IR commands, you can type the hex code manually or tap **Learn IR** to capture it from your existing remote
5. Go to **Activities** and create one (e.g. "Watch TV")
6. Open the activity and tap **Keymap** to assign physical keys to your device commands
7. Switch to the **Remote** tab — your commands appear as buttons you can tap

## Features

### Three Control Transports

- **IR (Infrared)** — Control TVs, amplifiers, and any IR device. Supports all major protocols (NEC, Samsung, Sony, RC5, RC6, and many more). Built-in IR learning lets you capture codes from existing remotes.
- **BLE Keyboard** — Control streaming devices (Fire TV, Apple TV, etc.) by emulating a Bluetooth keyboard. Supports key presses, media keys, and text input. See the BLE command reference below for all supported actions.
- **MQTT** — Control smart home devices over WiFi. Configure broker settings, then send topic/payload commands to any MQTT-compatible device.

### On-Device IR Learning

Point your existing remote at OMOTE and press a button — the IR code is automatically captured, decoded, and saved to the command you're editing. Works from both the touchscreen UI and the companion webapp.

### Activities and Key Mapping

Activities let you group devices for a use case (e.g. "Watch TV" uses your TV + soundbar). Within each activity you can:
- **Map physical keys** — Assign any of the 24 physical buttons to a specific device command
- **Define startup actions** — Automatically send commands when you switch to an activity (e.g. turn on TV + switch soundbar to HDMI input)

### Auto-Generated Remote UI

The Remote tab dynamically generates control buttons from your saved commands. No hardcoded layouts — if you add a command, it appears as a button. Large command sets are paginated automatically. You can customize button labels with an SD card icon pack.

### Backup and Restore

- **SD Card** — Create timestamped backups to SD. Restore from a picker that shows all available backups.
- **Serial Export/Import** — Back up and restore over USB without an SD card (via the companion webapp).
- **Cross-version compatibility** — Restoring from older backup formats is handled automatically.

### Companion Web App

A browser-based tool for configuring your OMOTE from a PC. No firmware rebuild needed.

#### Setup

```bash
cd tools/omote-webapp
pip install -r requirements.txt
python app.py
```

Then open `http://localhost:8080` in your browser.

> **Note:** If `pip` and `python` aren't in your system PATH, you can use the PlatformIO-bundled Python instead. On Windows that's typically:
> `C:\Users\<you>\.platformio\penv\Scripts\python.exe`

#### What It Can Do

- **Connection** — Select your OMOTE's serial port and connect
- **Devices** — Add, edit, delete devices and commands. Test commands. Learn IR codes from the browser.
- **Activities** — Manage activities, key bindings, and startup actions
- **Backup** — Create/restore SD backups, export/import over serial, upload icon packs
- **Monitor** — Live serial log viewer with filtering

The webapp communicates with the OMOTE over USB serial using a JSON protocol. The Python server bridges your browser's WebSocket connection to the serial port.

### WiFi, BLE, and MQTT Settings

All configured from the **Settings** tab on the device:

- **WiFi** — Scan for networks, enter password, connect. Time syncs automatically via NTP when connected.
- **BLE** — Start/stop advertising, manage paired devices, clear bonds.
- **MQTT** — Set broker host, port, username, password, and client ID.

### Status Bar

The top status bar is visible on all tabs and shows at a glance:
- **WiFi status** (left) — green when connected, red when disconnected, includes MQTT indicator
- **Current activity** (left-center) — the name of the currently selected activity, shown in light blue. Long names are truncated with `...`
- **Clock** (center) — current time (synced via NTP when WiFi is connected)
- **Battery** (right) — percentage and charging state

### Power Management

- Configurable sleep timeout (auto-sleep after inactivity)
- Configurable command debounce interval (prevents accidental double-presses)
- Lift-to-wake using the IMU motion sensor
- Charge protection with hysteresis to avoid battery wear from constant trickle charging
- Battery percentage and charging indicator in the status bar

## SD Card Icon Pack

You can customize the button labels on the Remote tab by placing a CSV file on the SD card.

- **File:** `/omote_v2_icons.csv` on the SD card root
- **Format:** One entry per line: `Command Name,LabelOrIconText`
- **Example:**
  ```
  Power,[PWR]
  Volume Up,+
  Volume Down,-
  Mute,M
  ```
- After updating the file, go to **Settings** and tap **Reload Icon Pack**.

## BLE Keyboard Command Reference

BLE device commands use the format `key:<action>`, `media:<action>`, or `text:<value>`.

### Key Actions (`key:`)

| Payload | HID Key | Apple TV Function |
|---------|---------|-------------------|
| `key:up` | Up Arrow | Navigate up |
| `key:down` | Down Arrow | Navigate down |
| `key:left` | Left Arrow | Navigate left |
| `key:right` | Right Arrow | Navigate right |
| `key:ok` / `key:enter` / `key:select` / `key:return` | Return/Enter | Select/Confirm |
| `key:back` / `key:escape` / `key:menu` | Escape | Menu / Back |
| `key:home` | F4 | Home screen |
| `key:space` | Spacebar | Play/Pause (alt) |

### Media Actions (`media:`)

| Payload | HID Consumer Key | Function |
|---------|-----------------|----------|
| `media:playpause` | Play/Pause | Toggle playback |
| `media:volup` | Volume Up | Volume up |
| `media:voldown` | Volume Down | Volume down |
| `media:mute` | Mute | Toggle mute |
| `media:next` | Next Track | Next track |
| `media:prev` | Previous Track | Previous track |
| `media:ff` | Fast Forward | Fast forward |
| `media:ff_long` | Fast Forward (hold) | Fast forward (long press) |
| `media:rewind` | Rewind | Rewind |
| `media:rewind_long` | Rewind (hold) | Rewind (long press) |
| `media:back` | AC Back | Back |
| `media:home` | AC Home | Home |

### Text Input (`text:`)

| Payload | Function |
|---------|----------|
| `text:Hello World` | Types the string character by character |

### Directed Connection

Prefix any command with `address@` to connect to a specific BLE device:
`AA:BB:CC:DD:EE:FF@key:enter`

## Serial Protocol Reference

For developers building tools that talk to the OMOTE over USB serial. All messages use 115200 baud, with protocol lines prefixed by `@@`.

**Request format:** `@@{"cmd":"command_name","id":"optional_request_id",...}\n`
**Response format:** `@@{"res":"command_name","id":"request_id","ok":true,"data":{...}}\n`

### Available Commands

| Command | Description |
|---------|-------------|
| `ping` | Health check (returns `pong`) |
| `status` | Battery, heap, device/activity count, MQTT status |
| `meta` | Device types, transport types, IR protocols, command slots |
| `dev_list` | List all devices |
| `dev_get` | Get single device (requires `dev_id`) |
| `dev_add` | Create device |
| `dev_update` | Update device (requires `dev_id`, partial update OK) |
| `dev_delete` | Delete device (requires `dev_id`) |
| `act_list` | List all activities |
| `act_get` | Get single activity (requires `act_id`) |
| `act_add` | Create activity |
| `act_update` | Update activity (requires `act_id`) |
| `act_delete` | Delete activity (requires `act_id`) |
| `dispatch` | Send a command (requires `device_id` and `command`) |
| `ir_learn_start` | Start IR receiver for code learning |
| `ir_learn_stop` | Stop IR receiver |
| `backup_sd` | Create SD card backup |
| `backup_list` | List available SD backups |
| `restore_sd` | Restore from SD backup (requires `path`) |
| `backup_export` | Export config as text over serial |
| `backup_import` | Import config from text over serial |
| `sd_write_start` | Begin chunked file upload to SD |
| `sd_write_chunk` | Send file chunk (base64 `data`) |
| `sd_write_end` | Finish file upload |
| `sd_read_start` | Begin chunked file download from SD |
| `sd_read_chunk` | Read file chunk |
| `sd_read_end` | Finish file download |

### Unsolicited Events

| Event | Description |
|-------|-------------|
| `ir_learned` | Fired when an IR code is captured during learning. Contains `protocol` and `payload` in `data`. |

### Key Field Names

- Device identifier: `dev_id` (not `id`)
- Activity identifier: `act_id` (not `id`)
- Dispatch command name: `command` (not `command_name`)
- Key bindings: `key` (ASCII int), `device_id`, `command_name`
- Startup actions: `device_id`, `slot` (e.g. `"Power"`, `"VolumeUp"`)
- IR protocol: `ir_protocol_name` (string like `"NEC"`) or `ir_protocol` (int ID)

## Current Limitations

- WiFi supports a single saved network (no multi-profile management yet).
- HTTP transport dispatch is defined but not yet enabled.

## Troubleshooting

- **Device not detected on USB:** Make sure you have ESP32-S3 USB drivers installed. The device uses VID `0x303A`.
- **Build fails:** Run `pio pkg update` to ensure all dependencies are current.
- **Webapp can't connect:** Check that no other program (Arduino IDE serial monitor, PlatformIO monitor) is using the serial port.
- **Webapp changes not showing / Alpine errors in browser console:** The webapp serves static JS files without cache-busting. After updating the firmware or webapp code, **hard-refresh your browser** with **Ctrl+Shift+R** (or Ctrl+F5). Symptoms of stale cache include "X is not defined" errors in the browser console and features not working despite code being correct.
- **Webapp saves not persisting:** Open the browser developer console (F12 → Console) and look for `[activities]` log lines. The save status message below the Save button also shows success/failure. If the firmware returns `ok: true` but data looks wrong, check that device IDs are numbers (not strings) in the console log output.
- **IR learning not working:** Make sure the IR receiver window on your OMOTE is not obstructed. Point your remote directly at it from close range (< 30cm).
- **Commands not sending:** Check that the device is enabled and the transport is available (e.g. WiFi connected for MQTT, BLE paired for BLE).
