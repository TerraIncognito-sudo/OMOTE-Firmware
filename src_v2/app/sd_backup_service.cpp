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
#include <sstream>
#include <string>
#include <vector>

#include "hardwareLayer.h"

namespace omote_v2 {

namespace {
constexpr const char* kLegacyBackupPath = "/omote_v2_backup.txt";
constexpr const char* kBackupPrefix = "/omote_v2_backup_";
constexpr const char* kBackupSuffix = ".txt";
constexpr const char* kBackupIndexPath = "/omote_v2_backup_index.txt";
constexpr const char* kHeader = "OMOTEV2_BACKUP_V1";
constexpr size_t kMaxDevices = 24;
constexpr size_t kMaxActivities = 12;
constexpr size_t kMaxBackupEntries = 48;
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

std::string build_backup_path_from_token(const std::string& token) {
  return std::string(kBackupPrefix) + token + kBackupSuffix;
}

bool is_structured_backup_path(const std::string& path) {
  const std::string prefix = kBackupPrefix;
  const std::string suffix = kBackupSuffix;
  return path.size() > prefix.size() + suffix.size() &&
         path.rfind(prefix, 0) == 0 &&
         path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string backup_label_from_path(const std::string& path) {
  if (path == kLegacyBackupPath) return "Legacy backup";
  if (!is_structured_backup_path(path)) return path;

  const std::string prefix = kBackupPrefix;
  const std::string suffix = kBackupSuffix;
  const std::string token = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());

  if (token.rfind("unsynced_", 0) == 0) {
    return "Unsynced " + token.substr(strlen("unsynced_"));
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
      return pretty;
    }
  }
  return token;
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

  String line;
  if (!read_line(&file, &line)) {
    file.close();
    set_status(out_status, "SD restore failed: bad header");
    return false;
  }
  line.trim();
  if (line != kHeader) {
    file.close();
    set_status(out_status, "SD restore failed: bad header");
    return false;
  }

  if (!read_line(&file, &line)) {
    file.close();
    set_status(out_status, "SD restore failed: invalid device count");
    return false;
  }
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
    String row;
    if (!read_line(&file, &row)) {
      file.close();
      set_status(out_status, "SD restore failed: invalid device row");
      return false;
    }
    row.trim();
    DeviceRecord parsed;
    if (!parse_device_line(std::string(row.c_str()), &parsed)) {
      file.close();
      set_status(out_status, "SD restore failed: invalid device row");
      return false;
    }
    devices.push_back(parsed);
  }

  if (!read_line(&file, &line)) {
    file.close();
    set_status(out_status, "SD restore failed: invalid activity count");
    return false;
  }
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
    String row;
    if (!read_line(&file, &row)) {
      file.close();
      set_status(out_status, "SD restore failed: invalid activity row");
      return false;
    }
    row.trim();
    ActivityRecord parsed;
    if (!parse_activity_line(std::string(row.c_str()), &parsed)) {
      file.close();
      set_status(out_status, "SD restore failed: invalid activity row");
      return false;
    }
    activities.push_back(parsed);
  }

  if (!read_line(&file, &line)) {
    file.close();
    set_status(out_status, "SD restore failed: missing end marker");
    return false;
  }
  line.trim();
  file.close();
  if (line != "END") {
    set_status(out_status, "SD restore failed: missing end marker");
    return false;
  }

  *out_devices = std::move(devices);
  *out_activities = std::move(activities);
  return true;
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
  const std::string backup_path = build_backup_path_from_token(build_backup_token(&has_valid_time));
  FsFile file = g_sd.open(backup_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
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

  std::vector<std::string> index = load_backup_index();
  prepend_backup_index(&index, backup_path);
  const bool index_saved = save_backup_index(index);

  if (has_valid_time) {
    if (index_saved) {
      set_status(out_status, "SD backup saved: " + backup_path);
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
  for (const std::string& path : index) {
    if (!path_exists(path)) continue;
    SdBackupEntry entry;
    entry.path = path;
    entry.label = backup_label_from_path(path);
    out_backups->push_back(entry);
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
  if (!parse_backup_file(backup_path, &devices, &activities, out_status)) {
    return false;
  }

  *out_devices = std::move(devices);
  *out_activities = std::move(activities);
  set_status(out_status, "SD restore complete: " + backup_label_from_path(backup_path));
  return true;
}

}  // namespace omote_v2
