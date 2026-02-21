#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace omote_v2 {

enum class DeviceType : uint8_t {
  TV = 0,
  AVR = 1,
  MediaPlayer = 2,
  SmartHome = 3,
  Lighting = 4,
  Custom = 5
};

enum class TransportType : uint8_t {
  IR = 0,
  BLE = 1,
  MQTT = 2,
  HTTP = 3
};

enum class CommandSlot : uint8_t {
  Power = 0,
  VolumeUp = 1,
  VolumeDown = 2,
  Mute = 3,
  Up = 4,
  Down = 5,
  Left = 6,
  Right = 7,
  Ok = 8,
  Back = 9,
  Home = 10,
  Count = 11
};

struct DeviceCommand {
  std::string name;
  std::string payload;
};

struct DeviceRecord {
  uint32_t id = 0;
  DeviceType type = DeviceType::Custom;
  TransportType transport = TransportType::IR;
  int ir_protocol = 3;  // NEC by default
  std::string name;
  std::string address;
  bool enabled = true;
  std::vector<DeviceCommand> commands;
};

const char* to_string(DeviceType type);
const char* to_string(TransportType transport);
const char* to_string(CommandSlot slot);
DeviceType device_type_from_string(const std::string& value);
TransportType transport_type_from_string(const std::string& value);
bool command_slot_from_string(const std::string& value, CommandSlot* out);
std::string protocol_name(int protocol);
int protocol_from_name(const std::string& value);
const std::vector<int>& supported_ir_protocols();
const std::vector<CommandSlot>& all_command_slots();
const std::vector<std::string>& common_command_names();
const DeviceCommand* find_command(const DeviceRecord& device, CommandSlot slot);
const DeviceCommand* find_command_by_name(const DeviceRecord& device, const std::string& name);
void upsert_command(DeviceRecord* device, const std::string& name, const std::string& payload);

}  // namespace omote_v2
