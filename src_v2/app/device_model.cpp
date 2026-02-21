#include "app/device_model.h"

namespace omote_v2 {

namespace {
const std::vector<int> kSupportedProtocols = {
    3,   // NEC
    1,   // RC5
    2,   // RC6
    7,   // JVC
    8,   // SAMSUNG
    5,   // SONY
    10,  // LG
};

const std::vector<CommandSlot> kAllCommandSlots = {
    CommandSlot::Power,      CommandSlot::VolumeUp, CommandSlot::VolumeDown, CommandSlot::Mute,
    CommandSlot::Up,         CommandSlot::Down,     CommandSlot::Left,       CommandSlot::Right,
    CommandSlot::Ok,         CommandSlot::Back,     CommandSlot::Home,
};

const std::vector<std::string> kCommonCommandNames = {
    "Power", "Volume Up", "Volume Down", "Mute", "Up", "Down", "Left", "Right", "OK", "Back", "Home",
};
}  // namespace

const char* to_string(DeviceType type) {
  switch (type) {
    case DeviceType::TV:
      return "TV";
    case DeviceType::AVR:
      return "AVR";
    case DeviceType::MediaPlayer:
      return "Media Player";
    case DeviceType::SmartHome:
      return "Smart Home";
    case DeviceType::Lighting:
      return "Lighting";
    case DeviceType::Custom:
    default:
      return "Custom";
  }
}

const char* to_string(TransportType transport) {
  switch (transport) {
    case TransportType::IR:
      return "IR";
    case TransportType::BLE:
      return "BLE";
    case TransportType::MQTT:
      return "MQTT";
    case TransportType::HTTP:
      return "HTTP";
    default:
      return "IR";
  }
}

const char* to_string(CommandSlot slot) {
  switch (slot) {
    case CommandSlot::Power:
      return "Power";
    case CommandSlot::VolumeUp:
      return "Volume Up";
    case CommandSlot::VolumeDown:
      return "Volume Down";
    case CommandSlot::Mute:
      return "Mute";
    case CommandSlot::Up:
      return "Up";
    case CommandSlot::Down:
      return "Down";
    case CommandSlot::Left:
      return "Left";
    case CommandSlot::Right:
      return "Right";
    case CommandSlot::Ok:
      return "OK";
    case CommandSlot::Back:
      return "Back";
    case CommandSlot::Home:
      return "Home";
    default:
      return "Power";
  }
}

DeviceType device_type_from_string(const std::string& value) {
  if (value == "TV") return DeviceType::TV;
  if (value == "AVR") return DeviceType::AVR;
  if (value == "Media Player") return DeviceType::MediaPlayer;
  if (value == "Smart Home") return DeviceType::SmartHome;
  if (value == "Lighting") return DeviceType::Lighting;
  return DeviceType::Custom;
}

TransportType transport_type_from_string(const std::string& value) {
  if (value == "IR") return TransportType::IR;
  if (value == "BLE") return TransportType::BLE;
  if (value == "MQTT") return TransportType::MQTT;
  if (value == "HTTP") return TransportType::HTTP;
  return TransportType::IR;
}

bool command_slot_from_string(const std::string& value, CommandSlot* out) {
  if (value == "Power") {
    *out = CommandSlot::Power;
    return true;
  }
  if (value == "Volume Up") {
    *out = CommandSlot::VolumeUp;
    return true;
  }
  if (value == "Volume Down") {
    *out = CommandSlot::VolumeDown;
    return true;
  }
  if (value == "Mute") {
    *out = CommandSlot::Mute;
    return true;
  }
  if (value == "Up") {
    *out = CommandSlot::Up;
    return true;
  }
  if (value == "Down") {
    *out = CommandSlot::Down;
    return true;
  }
  if (value == "Left") {
    *out = CommandSlot::Left;
    return true;
  }
  if (value == "Right") {
    *out = CommandSlot::Right;
    return true;
  }
  if (value == "OK") {
    *out = CommandSlot::Ok;
    return true;
  }
  if (value == "Back") {
    *out = CommandSlot::Back;
    return true;
  }
  if (value == "Home") {
    *out = CommandSlot::Home;
    return true;
  }
  return false;
}

std::string protocol_name(int protocol) {
  switch (protocol) {
    case 1:
      return "RC5";
    case 2:
      return "RC6";
    case 3:
      return "NEC";
    case 5:
      return "SONY";
    case 7:
      return "JVC";
    case 8:
      return "SAMSUNG";
    case 10:
      return "LG";
    default:
      return "NEC";
  }
}

int protocol_from_name(const std::string& value) {
  if (value == "RC5") return 1;
  if (value == "RC6") return 2;
  if (value == "NEC") return 3;
  if (value == "SONY") return 5;
  if (value == "JVC") return 7;
  if (value == "SAMSUNG") return 8;
  if (value == "LG") return 10;
  return 3;
}

const std::vector<int>& supported_ir_protocols() {
  return kSupportedProtocols;
}

const std::vector<CommandSlot>& all_command_slots() {
  return kAllCommandSlots;
}

const std::vector<std::string>& common_command_names() {
  return kCommonCommandNames;
}

const DeviceCommand* find_command(const DeviceRecord& device, CommandSlot slot) {
  const std::string expected = to_string(slot);
  for (const DeviceCommand& command : device.commands) {
    if (command.name == expected) return &command;
  }
  return nullptr;
}

const DeviceCommand* find_command_by_name(const DeviceRecord& device, const std::string& name) {
  for (const DeviceCommand& command : device.commands) {
    if (command.name == name) return &command;
  }
  return nullptr;
}

void upsert_command(DeviceRecord* device, const std::string& name, const std::string& payload) {
  if (device == nullptr || name.empty()) return;
  for (DeviceCommand& command : device->commands) {
    if (command.name == name) {
      command.payload = payload;
      return;
    }
  }
  DeviceCommand command;
  command.name = name;
  command.payload = payload;
  device->commands.push_back(command);
}

}  // namespace omote_v2
