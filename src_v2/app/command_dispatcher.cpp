#include "app/command_dispatcher.h"

#include <cerrno>
#include <list>
#include <string>
#include <cstdlib>

#include "hardwareLayer.h"

namespace omote_v2 {

namespace {
std::string trim_copy(const std::string& in) {
  size_t start = 0;
  while (start < in.size() && (in[start] == ' ' || in[start] == '\t')) ++start;
  size_t end = in.size();
  while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\t')) --end;
  return in.substr(start, end - start);
}

bool parse_u64_token(const std::string& token) {
  if (token.empty()) return false;
  errno = 0;
  char* end_ptr = nullptr;
  (void)strtoull(token.c_str(), &end_ptr, 0);
  return errno == 0 && end_ptr != nullptr && *end_ptr == '\0';
}

bool parse_u32_token(const std::string& token) {
  if (token.empty()) return false;
  errno = 0;
  char* end_ptr = nullptr;
  (void)strtoul(token.c_str(), &end_ptr, 0);
  return errno == 0 && end_ptr != nullptr && *end_ptr == '\0';
}

bool is_valid_ir_payload(const std::string& raw_payload) {
  std::string payload = trim_copy(raw_payload);
  if (payload.empty()) return false;

  size_t first_colon = payload.find(':');
  if (first_colon == std::string::npos) return parse_u64_token(payload);
  size_t second_colon = payload.find(':', first_colon + 1);
  if (second_colon == std::string::npos) return false;
  if (payload.find(':', second_colon + 1) != std::string::npos) return false;

  std::string data = payload.substr(0, first_colon);
  std::string bits = payload.substr(first_colon + 1, second_colon - first_colon - 1);
  std::string repeat = payload.substr(second_colon + 1);
  return parse_u64_token(trim_copy(data)) && parse_u32_token(trim_copy(bits)) && parse_u32_token(trim_copy(repeat));
}
}  // namespace

CommandDispatcher::CommandDispatcher(DeviceRegistry& devices) : devices_(devices) {}

bool CommandDispatcher::dispatch(const ActivityRecord& activity, CommandSlot slot) {
  bool any_sent = false;
  for (uint32_t device_id : activity.device_ids) {
    const DeviceRecord* device = devices_.get_by_id(device_id);
    if (device == nullptr || !device->enabled) continue;
    if (dispatch_device(*device, slot)) any_sent = true;
  }
  return any_sent;
}

bool CommandDispatcher::dispatch_device_command(uint32_t device_id, const std::string& command_name) {
  const DeviceRecord* device = devices_.get_by_id(device_id);
  if (device == nullptr || !device->enabled) return false;
  return dispatch_device_command(*device, command_name);
}

bool CommandDispatcher::dispatch_device(const DeviceRecord& device, CommandSlot slot) {
  const DeviceCommand* command = find_command(device, slot);
  if (command == nullptr || command->payload.empty()) return false;
  return dispatch_device_command(device, command->name);
}

bool CommandDispatcher::dispatch_device_command(const DeviceRecord& device, const std::string& command_name) {
  const DeviceCommand* command = find_command_by_name(device, command_name);
  if (command == nullptr || command->payload.empty()) return false;

  if (device.transport == TransportType::IR) {
    if (!is_valid_ir_payload(command->payload)) {
      return false;
    }
    std::list<std::string> payloads;
    payloads.push_back(command->payload);
    sendIRcode_HAL(device.ir_protocol, payloads, "");
    return true;
  }

  // BLE/MQTT/HTTP support is planned next.
  return false;
}

}  // namespace omote_v2
