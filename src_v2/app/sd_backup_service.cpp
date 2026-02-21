#include "app/sd_backup_service.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "hardwareLayer.h"

namespace omote_v2 {

namespace {
constexpr const char* kBackupPath = "/omote_v2_backup.txt";
constexpr const char* kHeader = "OMOTEV2_BACKUP_V1";
constexpr size_t kMaxDevices = 24;
constexpr size_t kMaxActivities = 12;

std::string encode_component(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  const char* hex = "0123456789ABCDEF";
  for (unsigned char c : in) {
    if (c == '%' || c == '|' || c == ';' || c == ',' || c == ':' || c == '\n' || c == '\r') {
      out.push_back('%');
      out.push_back(hex[(c >> 4) & 0x0F]);
      out.push_back(hex[c & 0x0F]);
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  return -1;
}

std::string decode_component(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size()) {
      int hi = hex_nibble(in[i + 1]);
      int lo = hex_nibble(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(in[i]);
  }
  return out;
}

std::vector<std::string> split(const std::string& in, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(in);
  std::string token;
  while (std::getline(ss, token, delim)) out.push_back(token);
  return out;
}

bool parse_u32_token(const std::string& token, uint32_t* out) {
  if (token.empty() || out == nullptr) return false;
  errno = 0;
  char* end_ptr = nullptr;
  unsigned long parsed = strtoul(token.c_str(), &end_ptr, 10);
  if (errno != 0 || end_ptr == nullptr || *end_ptr != '\0') return false;
  *out = static_cast<uint32_t>(parsed);
  return true;
}

bool parse_int_token(const std::string& token, int* out) {
  if (token.empty() || out == nullptr) return false;
  errno = 0;
  char* end_ptr = nullptr;
  long parsed = strtol(token.c_str(), &end_ptr, 10);
  if (errno != 0 || end_ptr == nullptr || *end_ptr != '\0') return false;
  *out = static_cast<int>(parsed);
  return true;
}

String serialize_device_line(const DeviceRecord& d) {
  std::stringstream ss;
  ss << "D|"
     << d.id << '|'
     << static_cast<int>(d.type) << '|'
     << static_cast<int>(d.transport) << '|'
     << d.ir_protocol << '|'
     << (d.enabled ? 1 : 0) << '|'
     << encode_component(d.name) << '|'
     << encode_component(d.address) << '|';
  for (size_t i = 0; i < d.commands.size(); ++i) {
    if (i > 0) ss << ';';
    ss << encode_component(d.commands[i].name) << ','
       << encode_component(d.commands[i].payload);
  }
  return String(ss.str().c_str());
}

String serialize_activity_line(const ActivityRecord& a) {
  std::stringstream ss;
  ss << "A|"
     << a.id << '|'
     << encode_component(a.name) << '|';
  for (size_t i = 0; i < a.device_ids.size(); ++i) {
    if (i > 0) ss << ',';
    ss << a.device_ids[i];
  }
  ss << '|';
  for (size_t i = 0; i < a.key_bindings.size(); ++i) {
    if (i > 0) ss << ';';
    ss << static_cast<int>(a.key_bindings[i].key_char) << ':'
       << a.key_bindings[i].device_id << ':'
       << encode_component(a.key_bindings[i].command_name);
  }
  ss << '|';
  for (size_t i = 0; i < a.startup_actions.size(); ++i) {
    if (i > 0) ss << ';';
    ss << a.startup_actions[i].device_id << ':' << static_cast<int>(a.startup_actions[i].slot);
  }
  return String(ss.str().c_str());
}

bool parse_device_line(const std::string& line, DeviceRecord* out) {
  if (out == nullptr) return false;
  std::vector<std::string> parts = split(line, '|');
  if (parts.size() < 9 || parts[0] != "D") return false;

  uint32_t id = 0;
  int type = 0;
  int transport = 0;
  int protocol = 0;
  int enabled = 0;
  if (!parse_u32_token(parts[1], &id) ||
      !parse_int_token(parts[2], &type) ||
      !parse_int_token(parts[3], &transport) ||
      !parse_int_token(parts[4], &protocol) ||
      !parse_int_token(parts[5], &enabled)) {
    return false;
  }

  out->id = id;
  out->type = static_cast<DeviceType>(type);
  out->transport = static_cast<TransportType>(transport);
  out->ir_protocol = protocol;
  out->enabled = enabled != 0;
  out->name = decode_component(parts[6]);
  out->address = decode_component(parts[7]);
  out->commands.clear();

  if (!parts[8].empty()) {
    std::vector<std::string> commands = split(parts[8], ';');
    for (const std::string& command : commands) {
      std::vector<std::string> fields = split(command, ',');
      if (fields.size() < 2) continue;
      DeviceCommand mapped;
      int legacy_slot = 0;
      if (parse_int_token(fields[0], &legacy_slot)) {
        mapped.name = fields.size() >= 3 ? decode_component(fields[2]) : to_string(static_cast<CommandSlot>(legacy_slot));
        if (mapped.name.empty()) {
          mapped.name = to_string(static_cast<CommandSlot>(legacy_slot));
        }
        mapped.payload = decode_component(fields[1]);
      } else {
        mapped.name = decode_component(fields[0]);
        mapped.payload = decode_component(fields[1]);
      }
      out->commands.push_back(mapped);
    }
  }
  return true;
}

bool parse_activity_line(const std::string& line, ActivityRecord* out) {
  if (out == nullptr) return false;
  std::vector<std::string> parts = split(line, '|');
  if (parts.size() < 5 || parts[0] != "A") return false;

  uint32_t id = 0;
  if (!parse_u32_token(parts[1], &id)) return false;
  out->id = id;
  out->name = decode_component(parts[2]);
  out->device_ids.clear();
  out->key_bindings.clear();
  out->startup_actions.clear();

  if (!parts[3].empty()) {
    std::vector<std::string> ids = split(parts[3], ',');
    for (const std::string& id_raw : ids) {
      uint32_t parsed = 0;
      if (parse_u32_token(id_raw, &parsed)) {
        out->device_ids.push_back(parsed);
      }
    }
  }

  if (!parts[4].empty()) {
    std::vector<std::string> bindings = split(parts[4], ';');
    for (const std::string& binding_raw : bindings) {
      std::vector<std::string> kv = split(binding_raw, ':');
      if (kv.size() < 2) continue;
      int key_ascii = 0;
      if (!parse_int_token(kv[0], &key_ascii)) continue;
      ActivityKeyBinding binding;
      binding.key_char = static_cast<char>(key_ascii);
      if (kv.size() >= 3) {
        uint32_t device_id = 0;
        if (!parse_u32_token(kv[1], &device_id)) continue;
        binding.device_id = device_id;
        binding.command_name = decode_component(kv[2]);
      } else {
        int slot = 0;
        if (!parse_int_token(kv[1], &slot)) continue;
        binding.command_name = to_string(static_cast<CommandSlot>(slot));
      }
      out->key_bindings.push_back(binding);
    }
  }
  if (parts.size() >= 6 && !parts[5].empty()) {
    std::vector<std::string> actions = split(parts[5], ';');
    for (const std::string& action_raw : actions) {
      std::vector<std::string> kv = split(action_raw, ':');
      if (kv.size() != 2) continue;
      uint32_t device_id = 0;
      int slot = 0;
      if (!parse_u32_token(kv[0], &device_id) || !parse_int_token(kv[1], &slot)) continue;
      ActivityStartupAction action;
      action.device_id = device_id;
      action.slot = static_cast<CommandSlot>(slot);
      out->startup_actions.push_back(action);
    }
  }
  return true;
}

bool parse_count_line(const std::string& line, const char* prefix, uint32_t* out_count) {
  if (prefix == nullptr || out_count == nullptr) return false;
  size_t prefix_len = strlen(prefix);
  if (line.size() <= prefix_len || line.compare(0, prefix_len, prefix) != 0) return false;
  std::string number = line.substr(prefix_len);
  return parse_u32_token(number, out_count);
}

void set_status(std::string* out_status, const std::string& status) {
  if (out_status != nullptr) *out_status = status;
}

bool mount_sd(std::string* out_status) {
  init_SD_HAL();
  if (SD.begin(SD_CS_GPIO, SPI, 10000000)) return true;

  // Retry once after reinitializing the SPI bus.
  SPI.end();
  delay(20);
  init_SD_HAL();
  if (SD.begin(SD_CS_GPIO, SPI, 10000000)) return true;

  set_status(out_status, "SD mount failed. Card must be FAT/exFAT.");
  return false;
}
}  // namespace

bool SdBackupService::backup_to_sd(const std::vector<DeviceRecord>& devices, const std::vector<ActivityRecord>& activities, std::string* out_status) const {
  if (devices.size() > kMaxDevices || activities.size() > kMaxActivities) {
    set_status(out_status, "SD backup failed: too many records");
    return false;
  }

  if (!mount_sd(out_status)) {
    if (out_status != nullptr && out_status->empty()) {
      set_status(out_status, "SD backup failed: card init");
    }
    return false;
  }

  SD.remove(kBackupPath);
  File file = SD.open(kBackupPath, FILE_WRITE);
  if (!file) {
    set_status(out_status, "SD backup failed: open write");
    return false;
  }

  file.println(kHeader);
  file.print("DEVICES ");
  file.println(static_cast<unsigned>(devices.size()));
  for (const DeviceRecord& device : devices) {
    file.println(serialize_device_line(device));
  }
  file.print("ACTIVITIES ");
  file.println(static_cast<unsigned>(activities.size()));
  for (const ActivityRecord& activity : activities) {
    file.println(serialize_activity_line(activity));
  }
  file.println("END");
  file.close();

  set_status(out_status, "SD backup saved: /omote_v2_backup.txt");
  return true;
}

bool SdBackupService::restore_from_sd(std::vector<DeviceRecord>* out_devices, std::vector<ActivityRecord>* out_activities, std::string* out_status) const {
  if (out_devices == nullptr || out_activities == nullptr) {
    set_status(out_status, "SD restore failed: invalid output");
    return false;
  }

  if (!mount_sd(out_status)) {
    if (out_status != nullptr && out_status->empty()) {
      set_status(out_status, "SD restore failed: card init");
    }
    return false;
  }

  File file = SD.open(kBackupPath, FILE_READ);
  if (!file) {
    set_status(out_status, "SD restore failed: backup missing");
    return false;
  }

  String line = file.readStringUntil('\n');
  line.trim();
  if (line != kHeader) {
    file.close();
    set_status(out_status, "SD restore failed: bad header");
    return false;
  }

  line = file.readStringUntil('\n');
  line.trim();
  uint32_t expected_devices = 0;
  if (!parse_count_line(std::string(line.c_str()), "DEVICES ", &expected_devices) || expected_devices > kMaxDevices) {
    file.close();
    set_status(out_status, "SD restore failed: invalid device count");
    return false;
  }

  std::vector<DeviceRecord> devices;
  devices.reserve(expected_devices);
  for (uint32_t i = 0; i < expected_devices; ++i) {
    String row = file.readStringUntil('\n');
    row.trim();
    DeviceRecord parsed;
    if (!parse_device_line(std::string(row.c_str()), &parsed)) {
      file.close();
      set_status(out_status, "SD restore failed: invalid device row");
      return false;
    }
    devices.push_back(parsed);
  }

  line = file.readStringUntil('\n');
  line.trim();
  uint32_t expected_activities = 0;
  if (!parse_count_line(std::string(line.c_str()), "ACTIVITIES ", &expected_activities) || expected_activities > kMaxActivities) {
    file.close();
    set_status(out_status, "SD restore failed: invalid activity count");
    return false;
  }

  std::vector<ActivityRecord> activities;
  activities.reserve(expected_activities);
  for (uint32_t i = 0; i < expected_activities; ++i) {
    String row = file.readStringUntil('\n');
    row.trim();
    ActivityRecord parsed;
    if (!parse_activity_line(std::string(row.c_str()), &parsed)) {
      file.close();
      set_status(out_status, "SD restore failed: invalid activity row");
      return false;
    }
    activities.push_back(parsed);
  }

  line = file.readStringUntil('\n');
  line.trim();
  file.close();
  if (line != "END") {
    set_status(out_status, "SD restore failed: missing end marker");
    return false;
  }

  *out_devices = devices;
  *out_activities = activities;
  set_status(out_status, "SD restore complete");
  return true;
}

bool SdBackupService::format_sd_card(std::string* out_status) const {
  if (!mount_sd(out_status)) {
    if (out_status != nullptr && out_status->empty()) {
      set_status(out_status, "SD format failed: card init");
    }
    return false;
  }

  SD.remove(kBackupPath);
  File file = SD.open(kBackupPath, FILE_WRITE);
  if (!file) {
    set_status(out_status, "SD format failed: open write");
    return false;
  }

  file.println(kHeader);
  file.println("DEVICES 0");
  file.println("ACTIVITIES 0");
  file.println("END");
  file.close();

  set_status(out_status, "SD quick format complete");
  return true;
}

}  // namespace omote_v2
