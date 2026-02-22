#include "app/device_storage.h"

#include <Preferences.h>

#include <sstream>
#include <string>
#include <vector>
#include <cerrno>
#include <cstdlib>

namespace omote_v2 {

namespace {
constexpr const char* kPrefsNs = "omotev2";
constexpr const char* kDevCountKey = "dev_count";
constexpr const char* kDevVersionKey = "dev_ver";
constexpr uint32_t kVersion = 3;
constexpr size_t kMaxDevices = 24;

std::string encode_component(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  const char* hex = "0123456789ABCDEF";
  for (unsigned char c : in) {
    if (c == '%' || c == '|' || c == ';' || c == ',' || c == ':') {
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

bool parse_int_token(const std::string& token, int* out) {
  if (token.empty() || out == nullptr) return false;
  errno = 0;
  char* end_ptr = nullptr;
  long parsed = strtol(token.c_str(), &end_ptr, 10);
  if (errno != 0 || end_ptr == nullptr || *end_ptr != '\0') return false;
  *out = static_cast<int>(parsed);
  return true;
}

std::string make_dev_key(size_t index) {
  return "dev_" + std::to_string(index);
}

std::string serialize_device(const DeviceRecord& d) {
  std::stringstream ss;
  ss << d.id << '|'
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
  return ss.str();
}

bool deserialize_device(const std::string& raw, DeviceRecord* out) {
  std::vector<std::string> parts = split(raw, '|');
  if (parts.size() < 8) return false;

  out->id = static_cast<uint32_t>(std::stoul(parts[0]));
  out->type = static_cast<DeviceType>(std::stoi(parts[1]));
  out->transport = static_cast<TransportType>(std::stoi(parts[2]));
  out->ir_protocol = std::stoi(parts[3]);
  out->enabled = std::stoi(parts[4]) != 0;
  out->name = decode_component(parts[5]);
  out->address = decode_component(parts[6]);
  out->commands.clear();

  if (!parts[7].empty()) {
    std::vector<std::string> commands = split(parts[7], ';');
    for (const std::string& c : commands) {
      std::vector<std::string> fields = split(c, ',');
      if (fields.size() < 2) continue;
      DeviceCommand cmd;
      int legacy_slot = 0;
      if (parse_int_token(fields[0], &legacy_slot)) {
        cmd.name = fields.size() >= 3 ? decode_component(fields[2]) : to_string(static_cast<CommandSlot>(legacy_slot));
        if (cmd.name.empty()) {
          cmd.name = to_string(static_cast<CommandSlot>(legacy_slot));
        }
        cmd.payload = decode_component(fields[1]);
      } else {
        cmd.name = decode_component(fields[0]);
        cmd.payload = decode_component(fields[1]);
      }
      out->commands.push_back(cmd);
    }
  }

  return true;
}
}  // namespace

bool DeviceStorage::load(std::vector<DeviceRecord>& out_records) {
  out_records.clear();

  Preferences prefs;
  if (!prefs.begin(kPrefsNs, false)) return false;

  (void)prefs.getUInt(kDevVersionKey, 0);
  size_t count = prefs.getUInt(kDevCountKey, 0);
  if (count > kMaxDevices) count = kMaxDevices;

  out_records.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    std::string key = make_dev_key(i);
    String raw = prefs.getString(key.c_str(), "");
    if (raw.length() == 0) continue;
    DeviceRecord d;
    if (deserialize_device(std::string(raw.c_str()), &d)) {
      out_records.push_back(d);
    }
  }

  prefs.end();
  return true;
}

bool DeviceStorage::save(const std::vector<DeviceRecord>& records) {
  if (records.size() > kMaxDevices) return false;

  Preferences prefs;
  if (!prefs.begin(kPrefsNs, false)) return false;

  prefs.putUInt(kDevVersionKey, kVersion);
  prefs.putUInt(kDevCountKey, static_cast<uint32_t>(records.size()));
  for (size_t i = 0; i < records.size(); ++i) {
    std::string key = make_dev_key(i);
    prefs.putString(key.c_str(), serialize_device(records[i]).c_str());
  }
  for (size_t i = records.size(); i < kMaxDevices; ++i) {
    std::string key = make_dev_key(i);
    if (prefs.isKey(key.c_str())) {
      prefs.remove(key.c_str());
    }
  }

  prefs.end();
  return true;
}

}  // namespace omote_v2
