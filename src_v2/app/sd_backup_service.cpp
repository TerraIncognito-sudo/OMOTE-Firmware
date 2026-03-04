#include "app/sd_backup_service.h"

#include <Arduino.h>
#include <SdFat.h>
#include <SPI.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "hardwareLayer.h"

namespace omote_v2 {

namespace {
constexpr const char* kLegacyBackupPath = "/omote_v2_backup.txt";
constexpr const char* kBackupPrefix = "/omote_v2_backup_";
constexpr const char* kBackupSuffixText = ".txt";
constexpr const char* kBackupSuffixJson = ".json";
constexpr const char* kBackupIndexPath = "/omote_v2_backup_index.txt";
constexpr const char* kHeader = "OMOTEV2_BACKUP_V1";
constexpr const char* kHeaderV2 = "OMOTEV2_BACKUP_V2";
constexpr const char* kSyncHookPath = "/omote_v2_sync_hook.json";
constexpr const char* kIconPackPath = "/omote_v2_icons.csv";
constexpr size_t kMaxDevices = 24;
constexpr size_t kMaxActivities = 12;
constexpr size_t kMaxBackupEntries = 48;
constexpr size_t kMaxBackupFileBytes = 256 * 1024;
constexpr time_t kMinValidEpoch = 1704067200;  // 2024-01-01 00:00:00 UTC
SdFs g_sd;

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

std::string trim_copy(const std::string& in) {
  size_t start = 0;
  while (start < in.size() && (in[start] == ' ' || in[start] == '\t')) ++start;
  size_t end = in.size();
  while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\t')) --end;
  return in.substr(start, end - start);
}

std::string lower_copy(const std::string& in) {
  std::string out = in;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return static_cast<char>(c);
  });
  return out;
}

std::string json_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (char c : in) {
    switch (c) {
      case '\"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

bool json_unescape(const std::string& in, std::string* out) {
  if (out == nullptr) return false;
  out->clear();
  out->reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '\\') {
      out->push_back(in[i]);
      continue;
    }
    if (i + 1 >= in.size()) return false;
    char esc = in[++i];
    switch (esc) {
      case 'n':
        out->push_back('\n');
        break;
      case 'r':
        out->push_back('\r');
        break;
      case 't':
        out->push_back('\t');
        break;
      case '\\':
        out->push_back('\\');
        break;
      case '"':
        out->push_back('"');
        break;
      default:
        // Keep unknown escape as-is to preserve payload data.
        out->push_back(esc);
        break;
    }
  }
  return true;
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

bool read_line(FsFile* file, String* out) {
  if (file == nullptr || out == nullptr) return false;
  *out = "";
  while (file->available()) {
    int c = file->read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') return true;
    out->concat(static_cast<char>(c));
  }
  return out->length() > 0;
}

bool sd_begin_with_fallback() {
  if (g_sd.begin(SdSpiConfig(SD_CS_GPIO, SHARED_SPI, SD_SCK_MHZ(10), &SPI))) return true;
  if (g_sd.begin(SdSpiConfig(SD_CS_GPIO, SHARED_SPI, SD_SCK_MHZ(4), &SPI))) return true;
  if (g_sd.begin(SdSpiConfig(SD_CS_GPIO, SHARED_SPI, SD_SCK_MHZ(1), &SPI))) return true;
  return false;
}

bool mount_sd(std::string* out_status) {
  init_SD_HAL();
  if (sd_begin_with_fallback()) return true;

  // Retry once after reinitializing the SPI bus.
  SPI.end();
  delay(20);
  init_SD_HAL();
  if (sd_begin_with_fallback()) return true;

  set_status(out_status, "SD mount failed. Card must be FAT32 or exFAT.");
  return false;
}

bool is_valid_time(time_t now) {
  return now >= kMinValidEpoch;
}

std::string build_backup_token(bool* out_has_valid_time) {
  time_t now = time(nullptr);
  if (is_valid_time(now)) {
    if (out_has_valid_time != nullptr) *out_has_valid_time = true;
    struct tm now_tm {};
    localtime_r(&now, &now_tm);
    char stamp[20];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &now_tm);
    return stamp;
  }
  if (out_has_valid_time != nullptr) *out_has_valid_time = false;
  char stamp[24];
  snprintf(stamp, sizeof(stamp), "unsynced_%010lu", static_cast<unsigned long>(millis()));
  return stamp;
}

std::string build_backup_text_path_from_token(const std::string& token) {
  return std::string(kBackupPrefix) + token + kBackupSuffixText;
}

std::string build_backup_json_path_from_token(const std::string& token) {
  return std::string(kBackupPrefix) + token + kBackupSuffixJson;
}

bool is_structured_backup_path_with_suffix(const std::string& path, const std::string& suffix) {
  const std::string prefix = kBackupPrefix;
  return path.size() > prefix.size() + suffix.size() &&
         path.rfind(prefix, 0) == 0 &&
         path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_structured_backup_path(const std::string& path) {
  return is_structured_backup_path_with_suffix(path, kBackupSuffixText) ||
         is_structured_backup_path_with_suffix(path, kBackupSuffixJson);
}

bool is_json_backup_path(const std::string& path) {
  return is_structured_backup_path_with_suffix(path, kBackupSuffixJson);
}

std::string backup_token_from_path(const std::string& path) {
  const std::string prefix = kBackupPrefix;
  if (is_structured_backup_path_with_suffix(path, kBackupSuffixText)) {
    return path.substr(prefix.size(), path.size() - prefix.size() - strlen(kBackupSuffixText));
  }
  if (is_structured_backup_path_with_suffix(path, kBackupSuffixJson)) {
    return path.substr(prefix.size(), path.size() - prefix.size() - strlen(kBackupSuffixJson));
  }
  return "";
}

std::string backup_label_from_path(const std::string& path) {
  if (path == kLegacyBackupPath) return "Legacy backup";
  if (!is_structured_backup_path(path)) return path;

  const std::string token = backup_token_from_path(path);
  const bool is_json = is_json_backup_path(path);

  if (token.rfind("unsynced_", 0) == 0) {
    return std::string(is_json ? "Unsynced JSON " : "Unsynced ") + token.substr(strlen("unsynced_"));
  }

  if (token.size() == 15 && token[8] == '_') {
    bool valid = true;
    for (size_t i = 0; i < token.size(); ++i) {
      if (i == 8) continue;
      if (token[i] < '0' || token[i] > '9') {
        valid = false;
        break;
      }
    }
    if (valid) {
      std::string pretty;
      pretty.reserve(19);
      pretty.append(token.substr(0, 4));
      pretty.push_back('-');
      pretty.append(token.substr(4, 2));
      pretty.push_back('-');
      pretty.append(token.substr(6, 2));
      pretty.push_back(' ');
      pretty.append(token.substr(9, 2));
      pretty.push_back(':');
      pretty.append(token.substr(11, 2));
      pretty.push_back(':');
      pretty.append(token.substr(13, 2));
      return pretty + (is_json ? " (JSON)" : "");
    }
  }
  return token + (is_json ? " (JSON)" : "");
}

bool path_exists(const std::string& path) {
  if (path.empty()) return false;
  return g_sd.exists(path.c_str());
}

std::vector<std::string> load_backup_index() {
  std::vector<std::string> entries;
  FsFile index = g_sd.open(kBackupIndexPath, O_RDONLY);
  if (!index) return entries;

  String line;
  while (read_line(&index, &line)) {
    line.trim();
    if (line.isEmpty()) continue;
    std::string entry = line.c_str();
    if (entry.empty()) continue;
    if (std::find(entries.begin(), entries.end(), entry) != entries.end()) continue;
    entries.push_back(entry);
    if (entries.size() >= kMaxBackupEntries) break;
  }
  index.close();
  return entries;
}

bool save_backup_index(const std::vector<std::string>& entries) {
  FsFile index = g_sd.open(kBackupIndexPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!index) return false;
  for (const std::string& entry : entries) {
    if (entry.empty()) continue;
    index.println(entry.c_str());
  }
  index.close();
  return true;
}

void prepend_backup_index(std::vector<std::string>* entries, const std::string& path) {
  if (entries == nullptr || path.empty()) return;
  auto existing = std::find(entries->begin(), entries->end(), path);
  if (existing != entries->end()) {
    entries->erase(existing);
  }
  entries->insert(entries->begin(), path);
  if (entries->size() > kMaxBackupEntries) {
    entries->resize(kMaxBackupEntries);
  }
}

bool parse_backup_file(const std::string& backup_path, std::vector<DeviceRecord>* out_devices, std::vector<ActivityRecord>* out_activities,
                       std::string* out_status) {
  FsFile file = g_sd.open(backup_path.c_str(), O_RDONLY);
  if (!file) {
    set_status(out_status, "SD restore failed: backup missing");
    return false;
  }
  if (file.size() > kMaxBackupFileBytes) {
    file.close();
    set_status(out_status, "SD restore failed: file too large");
    return false;
  }

  std::vector<std::string> lines;
  String line;
  while (read_line(&file, &line)) {
    line.trim();
    lines.push_back(std::string(line.c_str()));
    if (lines.size() > 4096) {
      file.close();
      set_status(out_status, "SD restore failed: file has too many lines");
      return false;
    }
  }
  file.close();
  if (lines.empty()) {
    set_status(out_status, "SD restore failed: empty backup");
    return false;
  }

  auto validate_loaded_records = [&](const std::vector<DeviceRecord>& devices, const std::vector<ActivityRecord>& activities) -> bool {
    if (devices.size() > kMaxDevices || activities.size() > kMaxActivities) {
      set_status(out_status, "SD restore failed: record limits exceeded");
      return false;
    }
    std::set<uint32_t> device_ids;
    for (const DeviceRecord& device : devices) {
      if (device.id == 0) {
        set_status(out_status, "SD restore failed: device id is zero");
        return false;
      }
      if (!device_ids.insert(device.id).second) {
        set_status(out_status, "SD restore failed: duplicate device id");
        return false;
      }
    }
    std::set<uint32_t> activity_ids;
    for (const ActivityRecord& activity : activities) {
      if (activity.id == 0) {
        set_status(out_status, "SD restore failed: activity id is zero");
        return false;
      }
      if (!activity_ids.insert(activity.id).second) {
        set_status(out_status, "SD restore failed: duplicate activity id");
        return false;
      }
    }
    return true;
  };

  auto parse_backup_lines = [&](const std::vector<std::string>& source_lines, std::vector<DeviceRecord>* devices_out,
                                std::vector<ActivityRecord>* activities_out) -> bool {
    if (devices_out == nullptr || activities_out == nullptr) return false;
    size_t line_index = 0;
    auto next_line = [&](std::string* out) -> bool {
      if (out == nullptr || line_index >= source_lines.size()) return false;
      *out = source_lines[line_index++];
      return true;
    };

    std::string row;
    if (!next_line(&row)) {
      set_status(out_status, "SD restore failed: bad header");
      return false;
    }
    if (row != kHeader && row != kHeaderV2) {
      set_status(out_status, "SD restore failed: unsupported header");
      return false;
    }

    uint32_t schema_version = 1;
    if (!next_line(&row)) {
      set_status(out_status, "SD restore failed: invalid device count");
      return false;
    }
    if (row.rfind("SCHEMA ", 0) == 0) {
      if (!parse_u32_token(row.substr(strlen("SCHEMA ")), &schema_version)) {
        set_status(out_status, "SD restore failed: invalid schema version");
        return false;
      }
      if (schema_version < 1 || schema_version > 2) {
        set_status(out_status, "SD restore failed: unsupported schema version");
        return false;
      }
      if (!next_line(&row)) {
        set_status(out_status, "SD restore failed: invalid device count");
        return false;
      }
    }

    uint32_t expected_devices = 0;
    if (!parse_count_line(row, "DEVICES ", &expected_devices) || expected_devices > kMaxDevices) {
      set_status(out_status, "SD restore failed: invalid device count");
      return false;
    }

    std::vector<DeviceRecord> devices;
    devices.reserve(expected_devices);
    for (uint32_t i = 0; i < expected_devices; ++i) {
      if (!next_line(&row)) {
        set_status(out_status, "SD restore failed: invalid device row");
        return false;
      }
      DeviceRecord parsed;
      if (!parse_device_line(row, &parsed)) {
        set_status(out_status, "SD restore failed: invalid device row");
        return false;
      }
      devices.push_back(parsed);
    }

    if (!next_line(&row)) {
      set_status(out_status, "SD restore failed: invalid activity count");
      return false;
    }
    uint32_t expected_activities = 0;
    if (!parse_count_line(row, "ACTIVITIES ", &expected_activities) || expected_activities > kMaxActivities) {
      set_status(out_status, "SD restore failed: invalid activity count");
      return false;
    }

    std::vector<ActivityRecord> activities;
    activities.reserve(expected_activities);
    for (uint32_t i = 0; i < expected_activities; ++i) {
      if (!next_line(&row)) {
        set_status(out_status, "SD restore failed: invalid activity row");
        return false;
      }
      ActivityRecord parsed;
      if (!parse_activity_line(row, &parsed)) {
        set_status(out_status, "SD restore failed: invalid activity row");
        return false;
      }
      activities.push_back(parsed);
    }

    if (!next_line(&row) || row != "END") {
      set_status(out_status, "SD restore failed: missing end marker");
      return false;
    }

    if (!validate_loaded_records(devices, activities)) return false;
    *devices_out = std::move(devices);
    *activities_out = std::move(activities);
    return true;
  };

  if (!parse_backup_lines(lines, out_devices, out_activities)) return false;
  return true;
}

bool write_sync_hook(const std::string& event_name, const std::string& backup_path, const std::string& format, bool ok,
                     size_t device_count, size_t activity_count) {
  FsFile file = g_sd.open(kSyncHookPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return false;

  time_t now = time(nullptr);
  char ts[32];
  ts[0] = '\0';
  if (is_valid_time(now)) {
    struct tm now_tm {};
    localtime_r(&now, &now_tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &now_tm);
  } else {
    snprintf(ts, sizeof(ts), "unsynced_%010lu", static_cast<unsigned long>(millis()));
  }

  file.print("{\"event\":\"");
  file.print(json_escape(event_name).c_str());
  file.print("\",\"path\":\"");
  file.print(json_escape(backup_path).c_str());
  file.print("\",\"format\":\"");
  file.print(json_escape(format).c_str());
  file.print("\",\"ok\":");
  file.print(ok ? "true" : "false");
  file.print(",\"devices\":");
  file.print(static_cast<unsigned>(device_count));
  file.print(",\"activities\":");
  file.print(static_cast<unsigned>(activity_count));
  file.print(",\"timestamp\":\"");
  file.print(ts);
  file.println("\"}");
  file.close();
  return true;
}

std::string serialize_text_backup_payload(const std::vector<DeviceRecord>& devices,
                                          const std::vector<ActivityRecord>& activities) {
  std::stringstream ss;
  ss << kHeaderV2 << "\n";
  ss << "SCHEMA 2\n";
  ss << "DEVICES " << devices.size() << "\n";
  for (const DeviceRecord& device : devices) {
    ss << serialize_device_line(device).c_str() << "\n";
  }
  ss << "ACTIVITIES " << activities.size() << "\n";
  for (const ActivityRecord& activity : activities) {
    ss << serialize_activity_line(activity).c_str() << "\n";
  }
  ss << "END\n";
  return ss.str();
}

bool extract_json_string_field(const std::string& json, const std::string& key, std::string* out_value) {
  if (out_value == nullptr) return false;
  const std::string pattern = "\"" + key + "\"";
  size_t key_pos = json.find(pattern);
  if (key_pos == std::string::npos) return false;
  size_t colon = json.find(':', key_pos + pattern.size());
  if (colon == std::string::npos) return false;
  size_t first_quote = json.find('"', colon + 1);
  if (first_quote == std::string::npos) return false;

  std::string encoded;
  bool escape = false;
  for (size_t i = first_quote + 1; i < json.size(); ++i) {
    char c = json[i];
    if (escape) {
      encoded.push_back('\\');
      encoded.push_back(c);
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      return json_unescape(encoded, out_value);
    }
    encoded.push_back(c);
  }
  return false;
}

bool parse_json_backup_file(const std::string& backup_path, std::vector<DeviceRecord>* out_devices,
                            std::vector<ActivityRecord>* out_activities, std::string* out_status) {
  FsFile file = g_sd.open(backup_path.c_str(), O_RDONLY);
  if (!file) {
    set_status(out_status, "SD restore failed: backup missing");
    return false;
  }
  if (file.size() > kMaxBackupFileBytes) {
    file.close();
    set_status(out_status, "SD restore failed: JSON file too large");
    return false;
  }

  std::string json_raw;
  json_raw.reserve(static_cast<size_t>(file.size()) + 8);
  while (file.available()) {
    int c = file.read();
    if (c < 0) break;
    json_raw.push_back(static_cast<char>(c));
  }
  file.close();
  if (trim_copy(json_raw).empty()) {
    set_status(out_status, "SD restore failed: JSON backup is empty");
    return false;
  }

  std::string payload_text;
  if (!extract_json_string_field(json_raw, "payload", &payload_text)) {
    set_status(out_status, "SD restore failed: JSON payload missing");
    return false;
  }

  std::vector<std::string> lines;
  std::stringstream ss(payload_text);
  std::string row;
  while (std::getline(ss, row)) {
    if (!row.empty() && row.back() == '\r') row.pop_back();
    lines.push_back(row);
  }
  if (lines.empty()) {
    set_status(out_status, "SD restore failed: JSON payload empty");
    return false;
  }

  // Reuse text parser by writing a temporary in-memory parse flow.
  const std::string temp_path = "/.omote_tmp_payload.txt";
  FsFile tmp = g_sd.open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!tmp) {
    set_status(out_status, "SD restore failed: JSON temp write");
    return false;
  }
  tmp.print(payload_text.c_str());
  tmp.close();
  const bool ok = parse_backup_file(temp_path, out_devices, out_activities, out_status);
  g_sd.remove(temp_path.c_str());
  return ok;
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

  bool has_valid_time = false;
  const std::string token = build_backup_token(&has_valid_time);
  const std::string text_path = build_backup_text_path_from_token(token);
  const std::string json_path = build_backup_json_path_from_token(token);
  const std::string text_payload = serialize_text_backup_payload(devices, activities);

  FsFile file = g_sd.open(text_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    set_status(out_status, "SD backup failed: open write");
    return false;
  }
  file.print(text_payload.c_str());
  file.close();

  FsFile json_file = g_sd.open(json_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!json_file) {
    set_status(out_status, "SD backup failed: open JSON write");
    return false;
  }
  json_file.print("{\"format\":\"omote_v2_backup\",\"container_schema\":1,\"data_schema\":2,\"payload\":\"");
  json_file.print(json_escape(text_payload).c_str());
  json_file.println("\"}");
  json_file.close();

  std::vector<std::string> index = load_backup_index();
  prepend_backup_index(&index, json_path);
  prepend_backup_index(&index, text_path);
  const bool index_saved = save_backup_index(index);
  (void)write_sync_hook("backup", text_path, "txt+json", true, devices.size(), activities.size());

  if (has_valid_time) {
    if (index_saved) {
      set_status(out_status, "SD backup saved: " + text_path + " (+ JSON)");
    } else {
      set_status(out_status, "SD backup saved, index update failed");
    }
  } else {
    if (index_saved) {
      set_status(out_status, "SD backup saved with unsynced time. Connect WiFi or set time manually for dated backups.");
    } else {
      set_status(out_status, "SD backup saved with unsynced time; index update failed");
    }
  }
  return true;
}

bool SdBackupService::list_backups(std::vector<SdBackupEntry>* out_backups, std::string* out_status) const {
  if (out_backups == nullptr) {
    set_status(out_status, "SD list failed: invalid output");
    return false;
  }
  out_backups->clear();

  if (!mount_sd(out_status)) {
    if (out_status != nullptr && out_status->empty()) {
      set_status(out_status, "SD list failed: card init");
    }
    return false;
  }

  std::vector<std::string> index = load_backup_index();
  std::set<std::string> seen;
  for (const std::string& path : index) {
    if (!path_exists(path)) continue;
    if (seen.find(path) != seen.end()) continue;
    SdBackupEntry entry;
    entry.path = path;
    entry.label = backup_label_from_path(path);
    out_backups->push_back(entry);
    seen.insert(path);

    // If text backup is indexed, also surface sibling JSON backup (and vice versa).
    const std::string token = backup_token_from_path(path);
    if (!token.empty()) {
      const std::string sibling_text = build_backup_text_path_from_token(token);
      const std::string sibling_json = build_backup_json_path_from_token(token);
      if (path != sibling_text && path_exists(sibling_text) && seen.find(sibling_text) == seen.end()) {
        SdBackupEntry text_entry;
        text_entry.path = sibling_text;
        text_entry.label = backup_label_from_path(sibling_text);
        out_backups->push_back(text_entry);
        seen.insert(sibling_text);
      }
      if (path != sibling_json && path_exists(sibling_json) && seen.find(sibling_json) == seen.end()) {
        SdBackupEntry json_entry;
        json_entry.path = sibling_json;
        json_entry.label = backup_label_from_path(sibling_json);
        out_backups->push_back(json_entry);
        seen.insert(sibling_json);
      }
    }
  }

  if (out_backups->empty() && path_exists(kLegacyBackupPath)) {
    SdBackupEntry legacy;
    legacy.path = kLegacyBackupPath;
    legacy.label = backup_label_from_path(kLegacyBackupPath);
    out_backups->push_back(legacy);
  }

  if (out_backups->empty()) {
    set_status(out_status, "No backups found on SD");
  } else {
    set_status(out_status, "Backups found");
  }
  return true;
}

bool SdBackupService::restore_from_sd(const std::string& backup_path, std::vector<DeviceRecord>* out_devices,
                                      std::vector<ActivityRecord>* out_activities, std::string* out_status) const {
  if (out_devices == nullptr || out_activities == nullptr) {
    set_status(out_status, "SD restore failed: invalid output");
    return false;
  }
  if (backup_path.empty()) {
    set_status(out_status, "SD restore failed: no backup selected");
    return false;
  }

  if (!mount_sd(out_status)) {
    if (out_status != nullptr && out_status->empty()) {
      set_status(out_status, "SD restore failed: card init");
    }
    return false;
  }

  std::vector<DeviceRecord> devices;
  std::vector<ActivityRecord> activities;
  const bool is_json = is_json_backup_path(backup_path);
  const bool parse_ok = is_json
                            ? parse_json_backup_file(backup_path, &devices, &activities, out_status)
                            : parse_backup_file(backup_path, &devices, &activities, out_status);
  if (!parse_ok) {
    (void)write_sync_hook("restore", backup_path, is_json ? "json" : "txt", false, 0, 0);
    return false;
  }

  *out_devices = std::move(devices);
  *out_activities = std::move(activities);
  (void)write_sync_hook("restore", backup_path, is_json ? "json" : "txt", true, out_devices->size(), out_activities->size());
  set_status(out_status, "SD restore complete: " + backup_label_from_path(backup_path));
  return true;
}

bool SdBackupService::load_icon_pack(std::unordered_map<std::string, std::string>* out_icon_overrides, std::string* out_status) const {
  if (out_icon_overrides == nullptr) {
    set_status(out_status, "Icon pack load failed: invalid output");
    return false;
  }
  out_icon_overrides->clear();

  if (!mount_sd(out_status)) {
    if (out_status != nullptr && out_status->empty()) {
      set_status(out_status, "Icon pack load failed: card init");
    }
    return false;
  }
  if (!path_exists(kIconPackPath)) {
    set_status(out_status, "Icon pack not found");
    return false;
  }

  FsFile file = g_sd.open(kIconPackPath, O_RDONLY);
  if (!file) {
    set_status(out_status, "Icon pack load failed: open");
    return false;
  }

  String line;
  size_t count = 0;
  while (read_line(&file, &line)) {
    std::string row = trim_copy(std::string(line.c_str()));
    if (row.empty() || row[0] == '#') continue;
    const size_t comma = row.find(',');
    if (comma == std::string::npos) continue;
    const std::string command_name = trim_copy(row.substr(0, comma));
    const std::string icon_text = trim_copy(row.substr(comma + 1));
    if (command_name.empty() || icon_text.empty()) continue;
    (*out_icon_overrides)[lower_copy(command_name)] = icon_text;
    ++count;
    if (count >= 256) break;
  }
  file.close();

  set_status(out_status, count > 0 ? ("Icon pack loaded: " + std::to_string(static_cast<unsigned long>(count)) + " entries")
                                   : "Icon pack loaded: 0 entries");
  return true;
}

std::string SdBackupService::serialize_to_text(const std::vector<DeviceRecord>& devices,
                                               const std::vector<ActivityRecord>& activities) {
  return serialize_text_backup_payload(devices, activities);
}

bool SdBackupService::parse_from_text(const std::string& text, std::vector<DeviceRecord>* out_devices,
                                      std::vector<ActivityRecord>* out_activities, std::string* out_status) {
  if (out_devices == nullptr || out_activities == nullptr) {
    set_status(out_status, "Parse failed: invalid output");
    return false;
  }

  std::vector<std::string> lines;
  std::stringstream ss(text);
  std::string row;
  while (std::getline(ss, row)) {
    if (!row.empty() && row.back() == '\r') row.pop_back();
    lines.push_back(row);
  }
  if (lines.empty()) {
    set_status(out_status, "Parse failed: empty input");
    return false;
  }

  // Reuse the same parsing logic as parse_backup_file's inner lambda.
  size_t line_index = 0;
  auto next_line = [&](std::string* out) -> bool {
    if (out == nullptr || line_index >= lines.size()) return false;
    *out = lines[line_index++];
    return true;
  };

  std::string header_row;
  if (!next_line(&header_row) || (header_row != kHeader && header_row != kHeaderV2)) {
    set_status(out_status, "Parse failed: bad header");
    return false;
  }

  std::string count_row;
  uint32_t schema_version = 1;
  if (!next_line(&count_row)) {
    set_status(out_status, "Parse failed: truncated");
    return false;
  }
  if (count_row.rfind("SCHEMA ", 0) == 0) {
    if (!parse_u32_token(count_row.substr(strlen("SCHEMA ")), &schema_version) || schema_version < 1 || schema_version > 2) {
      set_status(out_status, "Parse failed: bad schema");
      return false;
    }
    if (!next_line(&count_row)) {
      set_status(out_status, "Parse failed: truncated");
      return false;
    }
  }

  uint32_t expected_devices = 0;
  if (!parse_count_line(count_row, "DEVICES ", &expected_devices) || expected_devices > kMaxDevices) {
    set_status(out_status, "Parse failed: bad device count");
    return false;
  }

  std::vector<DeviceRecord> devices;
  devices.reserve(expected_devices);
  for (uint32_t i = 0; i < expected_devices; ++i) {
    if (!next_line(&row)) { set_status(out_status, "Parse failed: truncated device"); return false; }
    DeviceRecord parsed;
    if (!parse_device_line(row, &parsed)) { set_status(out_status, "Parse failed: bad device line"); return false; }
    devices.push_back(parsed);
  }

  if (!next_line(&count_row)) { set_status(out_status, "Parse failed: truncated"); return false; }
  uint32_t expected_activities = 0;
  if (!parse_count_line(count_row, "ACTIVITIES ", &expected_activities) || expected_activities > kMaxActivities) {
    set_status(out_status, "Parse failed: bad activity count");
    return false;
  }

  std::vector<ActivityRecord> activities;
  activities.reserve(expected_activities);
  for (uint32_t i = 0; i < expected_activities; ++i) {
    if (!next_line(&row)) { set_status(out_status, "Parse failed: truncated activity"); return false; }
    ActivityRecord parsed;
    if (!parse_activity_line(row, &parsed)) { set_status(out_status, "Parse failed: bad activity line"); return false; }
    activities.push_back(parsed);
  }

  if (!next_line(&row) || row != "END") {
    set_status(out_status, "Parse failed: missing END");
    return false;
  }

  *out_devices = std::move(devices);
  *out_activities = std::move(activities);
  set_status(out_status, "Parse OK");
  return true;
}

}  // namespace omote_v2
