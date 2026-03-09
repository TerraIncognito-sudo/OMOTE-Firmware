# OMOTE - Open Universal Remote - Firmware

![Ubuntu build](https://github.com/OMOTE-Community/OMOTE-Firmware/actions/workflows/build-platformio-ubuntu.yml/badge.svg)
![Windows Build](https://github.com/OMOTE-Community/OMOTE-Firmware/actions/workflows/build-platformio-windows.yml/badge.svg)
![MacOS Build](https://github.com/OMOTE-Community/OMOTE-Firmware/actions/workflows/build-platformio-macos.yml/badge.svg)
[![OMOTE Discord](https://discordapp.com/api/guilds/1138116475559882852/widget.png?style=shield)][link1]

## Overview

ESP32 Arduino firmware for the [OMOTE open-source universal remote](https://github.com/OMOTE-Community/OMOTE-Hardware/). Control your TV, amplifier, streaming boxes, and smart home from a single handheld device using infrared, Bluetooth, and MQTT.

### V2 Firmware (Recommended)

The **V2 firmware** (`src_v2/`) is a complete rewrite that makes OMOTE fully configurable without touching code:

- **No-code setup** — Add devices, learn IR codes, map buttons, and configure transports entirely from the touchscreen or the companion web app
- **Three transports** — IR (all major protocols), BLE Keyboard (Apple TV, Fire TV, etc.), and MQTT (smart home)
- **Companion web app** — Browser-based configuration tool over USB serial, with IR learning, device/activity management, and live monitoring
- **Backup & restore** — SD card snapshots and serial export/import preserve your configuration across firmware updates
- **On-device IR learning** — Point your existing remote at OMOTE and capture codes instantly

**[Full V2 documentation](src_v2/README.md)** | Requires OMOTE Rev 5+ (ESP32-S3) and [PlatformIO](https://platformio.org/)

```bash
# Build and flash V2
pio run -e omote-v2-esp32-s3 -t upload
```

### V1 Firmware (Legacy)

The original firmware in `src/` requires editing source code to add devices and commands. It supports all hardware revisions (Rev 1-5+). See the [wiki](https://github.com/OMOTE-Community/OMOTE-Firmware/wiki/How-to-understand-and-modify-the-firmware) for V1 usage.

### LVGL GUI simulator for Windows, Linux, and macOS

A simulator for running the LVGL UI on your local Windows, Linux, or macOS machine is available.

You can run the simulator in Visual Studio Code with PlatformIO. No need for any other compiler or development environment (no Visual Studio needed as often done in other LVGL simulators).
<div align="center">
  <img src="images/WindowsSimulator.gif" width="60%">
</div>

For details, please see the [wiki for the software simulator for fast creating and testing of LVGL GUIs.](https://github.com/OMOTE-Community/OMOTE-Firmware/wiki/Software-simulator-for-fast-creating-and-testing-of-LVGL-GUIs)

### Status

V2 has achieved the long-standing goals of graphical configuration editing and flash-based storage. Remaining work is UX polish and release hardening.

See the [open issues](https://github.com/OMOTE-Community/OMOTE-Firmware/issues) and [discussions](https://github.com/OMOTE-Community/OMOTE-Firmware/discussions) for proposed features and known issues.

## Contributing

If you have a suggestion for an improvement, please fork the repo and create a pull request. You can also simply open an issue or - for more general feature requests - head over to the [discussions](https://github.com/OMOTE-Community/OMOTE-Firmware/discussions).

## License

Distributed under the GPL v3 License. See [LICENSE](https://github.com/OMOTE-Community/OMOTE-Firmware/blob/main/LICENSE) for more information.

## Contact

[![OMOTE Discord](https://discordapp.com/api/guilds/1138116475559882852/widget.png?style=banner2 "OMOTE Discord")][link1]

Join the OMOTE Discord: [https://discord.gg/5PnYFAsKsG](https://discord.gg/5PnYFAsKsG)

[link1]: https://discord.gg/5PnYFAsKsG
