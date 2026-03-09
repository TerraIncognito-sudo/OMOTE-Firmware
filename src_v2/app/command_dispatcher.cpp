#include "app/command_dispatcher.h"

#include <cerrno>
#include <chrono>
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

std::string lower_copy(const std::string& in) {
  std::string out = in;
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return out;
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

struct MqttMessagePayload {
  std::string topic;
  std::string payload;
};

struct BleMessagePayload {
  std::string address;
  std::string kind;
  std::string action;
  std::string text;
};

bool parse_mqtt_message_payload(const std::string& raw_payload, const std::string& default_topic,
                                MqttMessagePayload* out_payload, std::string* out_error) {
  if (out_payload == nullptr) return false;
  const std::string payload = trim_copy(raw_payload);
  if (payload.empty()) {
    if (out_error != nullptr) *out_error = "empty payload";
    return false;
  }

  std::string topic;
  std::string body;
  const size_t newline_pos = payload.find('\n');
  const size_t pipe_pos = payload.find('|');
  if (newline_pos != std::string::npos) {
    topic = trim_copy(payload.substr(0, newline_pos));
    body = payload.substr(newline_pos + 1);
  } else if (pipe_pos != std::string::npos) {
    topic = trim_copy(payload.substr(0, pipe_pos));
    body = payload.substr(pipe_pos + 1);
  } else {
    body = payload;
  }

  if (topic.empty()) topic = trim_copy(default_topic);
  if (topic.empty()) {
    if (out_error != nullptr) {
      *out_error = "missing MQTT topic (use topic|payload or set device topic)";
    }
    return false;
  }

  out_payload->topic = topic;
  out_payload->payload = body;
  return true;
}

bool parse_ble_message_payload(const std::string& raw_payload, BleMessagePayload* out_payload, std::string* out_error) {
  if (out_payload == nullptr) return false;
  const std::string payload = trim_copy(raw_payload);
  if (payload.empty()) {
    if (out_error != nullptr) *out_error = "empty payload";
    return false;
  }

  std::string body = payload;
  const size_t at_pos = payload.find('@');
  if (at_pos != std::string::npos) {
    out_payload->address = trim_copy(payload.substr(0, at_pos));
    body = trim_copy(payload.substr(at_pos + 1));
    if (out_payload->address.empty()) {
      if (out_error != nullptr) *out_error = "empty address before '@'";
      return false;
    }
  } else {
    out_payload->address.clear();
  }

  const size_t colon = body.find(':');
  if (colon == std::string::npos) {
    if (out_error != nullptr) *out_error = "use key:<name>, media:<name>, or text:<value>";
    return false;
  }
  out_payload->kind = lower_copy(trim_copy(body.substr(0, colon)));
  const std::string value_raw = trim_copy(body.substr(colon + 1));
  if (value_raw.empty()) {
    if (out_error != nullptr) *out_error = "action/value is empty";
    return false;
  }

  if (out_payload->kind == "text") {
    out_payload->text = value_raw;
    out_payload->action.clear();
    return true;
  }
  if (out_payload->kind != "key" && out_payload->kind != "media") {
    if (out_error != nullptr) *out_error = "unknown BLE type";
    return false;
  }

  out_payload->action = lower_copy(value_raw);
  out_payload->text.clear();
  return true;
}

unsigned long current_millis() {
#if defined(ARDUINO)
  return millis();
#else
  using namespace std::chrono;
  return static_cast<unsigned long>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}
}  // namespace

CommandDispatcher::CommandDispatcher(DeviceRegistry& devices) : devices_(devices) {}

bool CommandDispatcher::dispatch(const ActivityRecord& activity, CommandSlot slot) {
  last_results_.clear();
  set_last_result(DispatchResultCode::NotMapped, 0, to_string(slot), TransportType::IR, "No mapping");
  bool any_sent = false;
  for (uint32_t device_id : activity.device_ids) {
    const DeviceRecord* device = devices_.get_by_id(device_id);
    if (device == nullptr || !device->enabled) {
      DispatchResult r;
      r.code = DispatchResultCode::DeviceUnavailable;
      r.device_id = device_id;
      r.command_name = to_string(slot);
      r.transport = TransportType::IR;
      r.detail = "Device unavailable";
      last_results_.push_back(r);
      continue;
    }
    const DeviceCommand* command = find_command(*device, slot);
    if (command == nullptr || trim_copy(command->payload).empty()) {
      DispatchResult r;
      r.code = DispatchResultCode::NotMapped;
      r.device_id = device->id;
      r.command_name = to_string(slot);
      r.transport = device->transport;
      r.detail = "No mapping";
      last_results_.push_back(r);
      continue;
    }
    if (dispatch_device(*device, slot)) any_sent = true;
    last_results_.push_back(last_result_);
  }

  if (!last_results_.empty()) {
    last_result_ = last_results_.back();
    last_status_ = last_result_.detail;
  }
  if (any_sent) {
    // Keep status deterministic when mixed results are present.
    last_status_ = "Sent";
    last_result_.code = DispatchResultCode::Sent;
    last_result_.detail = "Sent";
    return true;
  }
  if (last_results_.empty()) {
    set_last_result(DispatchResultCode::NotMapped, 0, to_string(slot), TransportType::IR, "No mapping");
  } else if (last_status_.empty()) {
    set_last_result(DispatchResultCode::SendFailed, 0, to_string(slot), TransportType::IR, "Dispatch failed");
  }
  return false;
}

bool CommandDispatcher::dispatch_device_command(uint32_t device_id, const std::string& command_name) {
  last_results_.clear();
  const DeviceRecord* device = devices_.get_by_id(device_id);
  if (device == nullptr || !device->enabled) {
    set_last_result(DispatchResultCode::DeviceUnavailable, device_id, command_name, TransportType::IR, "Device unavailable");
    return false;
  }
  const bool sent = dispatch_device_command(*device, command_name);
  last_results_.push_back(last_result_);
  return sent;
}

bool CommandDispatcher::dispatch_device(const DeviceRecord& device, CommandSlot slot) {
  const DeviceCommand* command = find_command(device, slot);
  if (command == nullptr || trim_copy(command->payload).empty()) {
    set_last_result(DispatchResultCode::NotMapped, device.id, to_string(slot), device.transport, "No mapping");
    return false;
  }
  return dispatch_device_command(device, command->name);
}

bool CommandDispatcher::dispatch_device_command(const DeviceRecord& device, const std::string& command_name) {
  const DeviceCommand* command = find_command_by_name(device, command_name);
  if (command == nullptr || trim_copy(command->payload).empty()) {
    set_last_result(DispatchResultCode::NotMapped, device.id, command_name, device.transport, "No mapping");
    return false;
  }

  if (is_debounced(device.id, command_name)) {
    set_last_result(DispatchResultCode::Debounced, device.id, command_name, device.transport, "Debounced");
    return false;
  }

  if (device.transport == TransportType::IR) {
    if (!is_valid_ir_payload(command->payload)) {
      set_last_result(DispatchResultCode::InvalidPayload, device.id, command_name, device.transport, "Invalid IR payload");
      return false;
    }
    std::list<std::string> payloads;
    payloads.push_back(command->payload);
    sendIRcode_HAL(device.ir_protocol, payloads, "");
    mark_sent(device.id, command_name);
    set_last_result(DispatchResultCode::Sent, device.id, command_name, device.transport, "Sent");
    return true;
  }

  if (device.transport == TransportType::MQTT) {
    if (!mqtt_is_configured_HAL()) {
      set_last_result(DispatchResultCode::TransportUnavailable, device.id, command_name,
                      device.transport, "MQTT broker not configured");
      return false;
    }
    MqttMessagePayload mqtt_payload;
    std::string parse_error;
    if (!parse_mqtt_message_payload(command->payload, device.address, &mqtt_payload, &parse_error)) {
      set_last_result(DispatchResultCode::InvalidPayload, device.id, command_name,
                      device.transport, "Invalid MQTT payload: " + parse_error);
      return false;
    }
    if (!publishMQTTMessage_HAL(mqtt_payload.topic.c_str(), mqtt_payload.payload.c_str())) {
      if (!getIsWifiConnected_HAL()) {
        set_last_result(DispatchResultCode::TransportUnavailable, device.id, command_name,
                        device.transport, "MQTT unavailable: WiFi disconnected");
      } else {
        set_last_result(DispatchResultCode::SendFailed, device.id, command_name,
                        device.transport, "MQTT publish failed");
      }
      return false;
    }
    mark_sent(device.id, command_name);
    set_last_result(DispatchResultCode::Sent, device.id, command_name, device.transport, "Sent");
    return true;
  }

  if (device.transport == TransportType::BLE) {
#if (ENABLE_KEYBOARD_BLE == 1)
    BleMessagePayload ble_payload;
    std::string parse_error;
    if (!parse_ble_message_payload(command->payload, &ble_payload, &parse_error)) {
      set_last_result(DispatchResultCode::InvalidPayload, device.id, command_name,
                      device.transport, "Invalid BLE payload: " + parse_error);
      return false;
    }
    if (!keyboardBLE_forceConnectionToAddress_HAL(ble_payload.address)) {
      set_last_result(DispatchResultCode::TransportUnavailable, device.id, command_name,
                      device.transport, "BLE connect failed");
      return false;
    }

    if (ble_payload.kind == "text") {
      keyboardBLE_sendString_HAL(ble_payload.text);
      mark_sent(device.id, command_name);
      set_last_result(DispatchResultCode::Sent, device.id, command_name, device.transport, "Sent");
      return true;
    }

    if (ble_payload.kind == "key") {
      if (ble_payload.action == "up") keyboardBLE_write_HAL(KEY_UP_ARROW);
      else if (ble_payload.action == "down") keyboardBLE_write_HAL(KEY_DOWN_ARROW);
      else if (ble_payload.action == "left") keyboardBLE_write_HAL(KEY_LEFT_ARROW);
      else if (ble_payload.action == "right") keyboardBLE_write_HAL(KEY_RIGHT_ARROW);
      else if (ble_payload.action == "ok" || ble_payload.action == "enter" || ble_payload.action == "select" || ble_payload.action == "return") keyboardBLE_write_HAL(KEY_RETURN);
      else if (ble_payload.action == "back" || ble_payload.action == "escape" || ble_payload.action == "menu") keyboardBLE_write_HAL(KEY_ESC);
      else if (ble_payload.action == "home") keyboardBLE_write_HAL(KEY_F4);
      else if (ble_payload.action == "space") keyboardBLE_write_HAL(' ');
      else {
        set_last_result(DispatchResultCode::InvalidPayload, device.id, command_name,
                        device.transport, "Invalid BLE key action");
        return false;
      }
      mark_sent(device.id, command_name);
      set_last_result(DispatchResultCode::Sent, device.id, command_name, device.transport, "Sent");
      return true;
    }

    if (ble_payload.kind == "media") {
      if (ble_payload.action == "back") consumerControlBLE_write_HAL(KEY_MEDIA_WWW_BACK);
      else if (ble_payload.action == "home") consumerControlBLE_write_HAL(KEY_MEDIA_WWW_HOME);
      else if (ble_payload.action == "prev") consumerControlBLE_write_HAL(KEY_MEDIA_PREVIOUS_TRACK);
      else if (ble_payload.action == "rewind") consumerControlBLE_write_HAL(KEY_MEDIA_REWIND);
      else if (ble_payload.action == "rewind_long") consumerControlBLE_longpress_HAL(KEY_MEDIA_REWIND);
      else if (ble_payload.action == "playpause") consumerControlBLE_write_HAL(KEY_MEDIA_PLAY_PAUSE);
      else if (ble_payload.action == "ff") consumerControlBLE_write_HAL(KEY_MEDIA_FASTFORWARD);
      else if (ble_payload.action == "ff_long") consumerControlBLE_longpress_HAL(KEY_MEDIA_FASTFORWARD);
      else if (ble_payload.action == "next") consumerControlBLE_write_HAL(KEY_MEDIA_NEXT_TRACK);
      else if (ble_payload.action == "mute") consumerControlBLE_write_HAL(KEY_MEDIA_MUTE);
      else if (ble_payload.action == "volup") consumerControlBLE_write_HAL(KEY_MEDIA_VOLUME_UP);
      else if (ble_payload.action == "voldown") consumerControlBLE_write_HAL(KEY_MEDIA_VOLUME_DOWN);
      else {
        set_last_result(DispatchResultCode::InvalidPayload, device.id, command_name,
                        device.transport, "Invalid BLE media action");
        return false;
      }
      mark_sent(device.id, command_name);
      set_last_result(DispatchResultCode::Sent, device.id, command_name, device.transport, "Sent");
      return true;
    }

    set_last_result(DispatchResultCode::InvalidPayload, device.id, command_name, device.transport, "Invalid BLE payload");
    return false;
#else
    set_last_result(DispatchResultCode::TransportUnavailable, device.id, command_name, device.transport, "BLE disabled in build");
    return false;
#endif
  }

  // HTTP support is planned next.
  set_last_result(DispatchResultCode::UnsupportedTransport, device.id, command_name, device.transport,
                  "Transport not implemented");
  return false;
}

const std::string& CommandDispatcher::last_status() const {
  return last_status_;
}

const DispatchResult& CommandDispatcher::last_result() const {
  return last_result_;
}

const std::vector<DispatchResult>& CommandDispatcher::last_results() const {
  return last_results_;
}

void CommandDispatcher::set_debounce_interval_ms(unsigned long debounce_interval_ms) {
  debounce_interval_ms_ = debounce_interval_ms;
  // Reset debounce history when interval policy changes.
  last_sent_ms_.clear();
}

unsigned long CommandDispatcher::debounce_interval_ms() const {
  return debounce_interval_ms_;
}

void CommandDispatcher::set_last_result(DispatchResultCode code, uint32_t device_id, const std::string& command_name,
                                        TransportType transport, const std::string& detail) {
  last_result_.code = code;
  last_result_.device_id = device_id;
  last_result_.command_name = command_name;
  last_result_.transport = transport;
  if (!detail.empty()) {
    last_result_.detail = detail;
    last_status_ = detail;
  } else {
    last_result_.detail = result_code_text(code);
    last_status_ = last_result_.detail;
  }
}

const char* CommandDispatcher::result_code_text(DispatchResultCode code) {
  switch (code) {
    case DispatchResultCode::Sent:
      return "Sent";
    case DispatchResultCode::NotMapped:
      return "No mapping";
    case DispatchResultCode::InvalidPayload:
      return "Invalid payload";
    case DispatchResultCode::TransportUnavailable:
      return "Transport unavailable";
    case DispatchResultCode::SendFailed:
      return "Send failed";
    case DispatchResultCode::Debounced:
      return "Debounced";
    case DispatchResultCode::DeviceUnavailable:
      return "Device unavailable";
    case DispatchResultCode::UnsupportedTransport:
      return "Transport not implemented";
    default:
      return "Dispatch failed";
  }
}

bool CommandDispatcher::is_debounced(uint32_t device_id, const std::string& command_name) {
  if (debounce_interval_ms_ == 0) return false;
  const std::string key = debounce_key(device_id, command_name);
  const unsigned long now = current_millis();
  auto it = last_sent_ms_.find(key);
  if (it == last_sent_ms_.end()) return false;
  return (now - it->second) < debounce_interval_ms_;
}

void CommandDispatcher::mark_sent(uint32_t device_id, const std::string& command_name) {
  last_sent_ms_[debounce_key(device_id, command_name)] = current_millis();
}

std::string CommandDispatcher::debounce_key(uint32_t device_id, const std::string& command_name) const {
  return std::to_string(device_id) + "|" + command_name;
}

}  // namespace omote_v2
