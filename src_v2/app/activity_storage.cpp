#include "app/activity_storage.h"

#include <Preferences.h>

#include <sstream>
#include <string>
#include <vector>

namespace omote_v2 {

namespace {
constexpr const char* kPrefsNs = "omotev2";
constexpr const char* kActCountKey = "act_count";
constexpr const char* kActVersionKey = "act_ver";
constexpr uint32_t kVersion = 4;
constexpr size_t kMaxActivities = 12;

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

std::string make_act_key(size_t index) {
  return "act_" + std::to_string(index);
}

std::string serialize_activity(const ActivityRecord& a) {
  std::stringstream ss;
  ss << a.id << '|' << encode_component(a.name) << '|';
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
  return ss.str();
}

bool deserialize_activity(const std::string& raw, ActivityRecord* out) {
  std::vector<std::string> parts = split(raw, '|');
  if (parts.size() < 4) return false;
  out->id = static_cast<uint32_t>(std::stoul(parts[0]));
  out->name = decode_component(parts[1]);
  out->device_ids.clear();
  out->key_bindings.clear();
  out->startup_actions.clear();

  if (!parts[2].empty()) {
    std::vector<std::string> ids = split(parts[2], ',');
    for (const std::string& id : ids) {
      out->device_ids.push_back(static_cast<uint32_t>(std::stoul(id)));
    }
  }
  if (!parts[3].empty()) {
    std::vector<std::string> binds = split(parts[3], ';');
    for (const std::string& b : binds) {
      std::vector<std::string> kv = split(b, ':');
      if (kv.size() < 2) continue;
      ActivityKeyBinding binding;
      binding.key_char = static_cast<char>(std::stoi(kv[0]));
      if (kv.size() >= 3) {
        binding.device_id = static_cast<uint32_t>(std::stoul(kv[1]));
        binding.command_name = decode_component(kv[2]);
      } else {
        binding.command_name = to_string(static_cast<CommandSlot>(std::stoi(kv[1])));
      }
      out->key_bindings.push_back(binding);
    }
  }
  if (parts.size() >= 5 && !parts[4].empty()) {
    std::vector<std::string> actions = split(parts[4], ';');
    for (const std::string& action_raw : actions) {
      std::vector<std::string> kv = split(action_raw, ':');
      if (kv.size() != 2) continue;
      ActivityStartupAction action;
      action.device_id = static_cast<uint32_t>(std::stoul(kv[0]));
      action.slot = static_cast<CommandSlot>(std::stoi(kv[1]));
      out->startup_actions.push_back(action);
    }
  }
  return true;
}
}  // namespace

bool ActivityStorage::load(std::vector<ActivityRecord>& out_records) {
  out_records.clear();

  Preferences prefs;
  if (!prefs.begin(kPrefsNs, false)) return false;

  (void)prefs.getUInt(kActVersionKey, 0);
  size_t count = prefs.getUInt(kActCountKey, 0);
  if (count > kMaxActivities) count = kMaxActivities;

  out_records.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    std::string key = make_act_key(i);
    String raw = prefs.getString(key.c_str(), "");
    if (raw.length() == 0) continue;
    ActivityRecord a;
    if (deserialize_activity(std::string(raw.c_str()), &a)) {
      out_records.push_back(a);
    }
  }

  prefs.end();
  return true;
}

bool ActivityStorage::save(const std::vector<ActivityRecord>& records) {
  if (records.size() > kMaxActivities) return false;

  Preferences prefs;
  if (!prefs.begin(kPrefsNs, false)) return false;

  prefs.putUInt(kActVersionKey, kVersion);
  prefs.putUInt(kActCountKey, static_cast<uint32_t>(records.size()));
  for (size_t i = 0; i < records.size(); ++i) {
    std::string key = make_act_key(i);
    prefs.putString(key.c_str(), serialize_activity(records[i]).c_str());
  }
  for (size_t i = records.size(); i < kMaxActivities; ++i) {
    std::string key = make_act_key(i);
    prefs.remove(key.c_str());
  }

  prefs.end();
  return true;
}

}  // namespace omote_v2
