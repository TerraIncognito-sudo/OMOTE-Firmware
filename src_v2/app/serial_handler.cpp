#include "app/serial_handler.h"

#include <Arduino.h>
#include <SdFat.h>
#include <SPI.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "hardwareLayer.h"

namespace omote_v2 {

namespace {

// SD filesystem instance for file transfer operations.
SdFs g_serial_sd;
FsFile g_transfer_file;

bool serial_sd_begin() {
  if (g_serial_sd.begin(SdSpiConfig(SD_CS_GPIO, SHARED_SPI, SD_SCK_MHZ(10), &SPI))) return true;
  if (g_serial_sd.begin(SdSpiConfig(SD_CS_GPIO, SHARED_SPI, SD_SCK_MHZ(4), &SPI))) return true;
  if (g_serial_sd.begin(SdSpiConfig(SD_CS_GPIO, SHARED_SPI, SD_SCK_MHZ(1), &SPI))) return true;
  return false;
}

bool serial_mount_sd() {
  init_SD_HAL();
  if (serial_sd_begin()) return true;
  SPI.end();
  delay(20);
  init_SD_HAL();
  return serial_sd_begin();
}

// Base64 tables
static const char kB64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const int8_t kB64Decode[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
};

}  // namespace

// -------------------------------------------------------------------
// Construction
// -------------------------------------------------------------------

SerialHandler::SerialHandler(DeviceRegistry& devices,
                             ActivityRegistry& activities,
                             CommandDispatcher& dispatcher,
                             SdBackupService& backup)
    : devices_(devices),
      activities_(activities),
      dispatcher_(dispatcher),
      backup_(backup) {}

// -------------------------------------------------------------------
// JSON helpers (static)
// -------------------------------------------------------------------

std::string SerialHandler::json_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (char c : in) {
    switch (c) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

bool SerialHandler::json_unescape(const std::string& in, std::string* out) {
  if (!out) return false;
  out->clear();
  out->reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '\\') { out->push_back(in[i]); continue; }
    if (i + 1 >= in.size()) return false;
    char esc = in[++i];
    switch (esc) {
      case 'n': out->push_back('\n'); break;
      case 'r': out->push_back('\r'); break;
      case 't': out->push_back('\t'); break;
      case '\\': out->push_back('\\'); break;
      case '"': out->push_back('"'); break;
      default: out->push_back(esc); break;
    }
  }
  return true;
}

std::string SerialHandler::extract_json_string(const std::string& json,
                                                const std::string& key) {
  const std::string pattern = "\"" + key + "\"";
  size_t pos = json.find(pattern);
  if (pos == std::string::npos) return "";
  size_t colon = json.find(':', pos + pattern.size());
  if (colon == std::string::npos) return "";
  size_t q1 = json.find('"', colon + 1);
  if (q1 == std::string::npos) return "";

  std::string encoded;
  bool escape = false;
  for (size_t i = q1 + 1; i < json.size(); ++i) {
    char c = json[i];
    if (escape) {
      encoded.push_back('\\');
      encoded.push_back(c);
      escape = false;
      continue;
    }
    if (c == '\\') { escape = true; continue; }
    if (c == '"') {
      std::string result;
      if (json_unescape(encoded, &result)) return result;
      return "";
    }
    encoded.push_back(c);
  }
  return "";
}

int SerialHandler::extract_json_int(const std::string& json,
                                     const std::string& key,
                                     int default_val) {
  const std::string pattern = "\"" + key + "\"";
  size_t pos = json.find(pattern);
  if (pos == std::string::npos) return default_val;
  size_t colon = json.find(':', pos + pattern.size());
  if (colon == std::string::npos) return default_val;
  size_t start = colon + 1;
  while (start < json.size() && json[start] == ' ') ++start;
  if (start >= json.size()) return default_val;
  char* end_ptr = nullptr;
  long val = strtol(json.c_str() + start, &end_ptr, 10);
  if (end_ptr == json.c_str() + start) return default_val;
  return static_cast<int>(val);
}

bool SerialHandler::extract_json_bool(const std::string& json,
                                       const std::string& key,
                                       bool default_val) {
  const std::string pattern = "\"" + key + "\"";
  size_t pos = json.find(pattern);
  if (pos == std::string::npos) return default_val;
  size_t colon = json.find(':', pos + pattern.size());
  if (colon == std::string::npos) return default_val;
  size_t start = colon + 1;
  while (start < json.size() && json[start] == ' ') ++start;
  if (start >= json.size()) return default_val;
  if (json.compare(start, 4, "true") == 0) return true;
  if (json.compare(start, 5, "false") == 0) return false;
  return default_val;
}

std::vector<std::string> SerialHandler::extract_json_string_array(
    const std::string& json, const std::string& key) {
  std::vector<std::string> result;
  const std::string pattern = "\"" + key + "\"";
  size_t pos = json.find(pattern);
  if (pos == std::string::npos) return result;
  size_t colon = json.find(':', pos + pattern.size());
  if (colon == std::string::npos) return result;
  size_t bracket = json.find('[', colon + 1);
  if (bracket == std::string::npos) return result;

  size_t i = bracket + 1;
  while (i < json.size()) {
    while (i < json.size() && (json[i] == ' ' || json[i] == ',')) ++i;
    if (i >= json.size() || json[i] == ']') break;
    if (json[i] == '"') {
      std::string encoded;
      bool esc = false;
      for (size_t j = i + 1; j < json.size(); ++j) {
        char c = json[j];
        if (esc) {
          encoded.push_back('\\');
          encoded.push_back(c);
          esc = false;
          continue;
        }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') {
          std::string val;
          if (json_unescape(encoded, &val)) result.push_back(val);
          i = j + 1;
          break;
        }
        encoded.push_back(c);
      }
    } else {
      ++i;
    }
  }
  return result;
}

// -------------------------------------------------------------------
// Base64
// -------------------------------------------------------------------

std::string SerialHandler::base64_encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
    out.push_back(kB64Chars[(n >> 18) & 0x3F]);
    out.push_back(kB64Chars[(n >> 12) & 0x3F]);
    out.push_back(i + 1 < len ? kB64Chars[(n >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < len ? kB64Chars[n & 0x3F] : '=');
  }
  return out;
}

std::vector<uint8_t> SerialHandler::base64_decode(const std::string& encoded) {
  std::vector<uint8_t> out;
  out.reserve((encoded.size() / 4) * 3);
  uint32_t buf = 0;
  int bits = 0;
  for (char c : encoded) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    if (static_cast<unsigned char>(c) >= 128) continue;
    int8_t val = kB64Decode[static_cast<unsigned char>(c)];
    if (val < 0) continue;
    buf = (buf << 6) | static_cast<uint32_t>(val);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
    }
  }
  return out;
}

// -------------------------------------------------------------------
// Response helpers
// -------------------------------------------------------------------

void SerialHandler::send_ok(const std::string& cmd, const std::string& id,
                             const std::string& data_json) {
  std::string msg = "@@{\"res\":\"" + cmd + "\",\"ok\":true";
  if (!id.empty()) msg += ",\"id\":\"" + json_escape(id) + "\"";
  msg += ",\"data\":" + data_json + "}\n";
  Serial.print(msg.c_str());
}

void SerialHandler::send_err(const std::string& cmd, const std::string& id,
                              const std::string& err_msg) {
  std::string msg = "@@{\"res\":\"" + cmd + "\",\"ok\":false";
  if (!id.empty()) msg += ",\"id\":\"" + json_escape(id) + "\"";
  msg += ",\"error\":\"" + json_escape(err_msg) + "\"}\n";
  Serial.print(msg.c_str());
}

// -------------------------------------------------------------------
// JSON serialization
// -------------------------------------------------------------------

std::string SerialHandler::device_to_json(const DeviceRecord& d) {
  std::string j = "{";
  j += "\"id\":" + std::to_string(d.id);
  j += ",\"name\":\"" + json_escape(d.name) + "\"";
  j += ",\"type\":\"" + std::string(to_string(d.type)) + "\"";
  j += ",\"transport\":\"" + std::string(to_string(d.transport)) + "\"";
  j += ",\"ir_protocol\":" + std::to_string(d.ir_protocol);
  j += ",\"ir_protocol_name\":\"" + json_escape(protocol_name(d.ir_protocol)) + "\"";
  j += ",\"enabled\":" + std::string(d.enabled ? "true" : "false");
  j += ",\"address\":\"" + json_escape(d.address) + "\"";
  j += ",\"commands\":[";
  for (size_t i = 0; i < d.commands.size(); ++i) {
    if (i > 0) j += ",";
    j += "{\"name\":\"" + json_escape(d.commands[i].name) + "\"";
    j += ",\"payload\":\"" + json_escape(d.commands[i].payload) + "\"}";
  }
  j += "]}";
  return j;
}

std::string SerialHandler::activity_to_json(const ActivityRecord& a) {
  std::string j = "{";
  j += "\"id\":" + std::to_string(a.id);
  j += ",\"name\":\"" + json_escape(a.name) + "\"";
  j += ",\"device_ids\":[";
  for (size_t i = 0; i < a.device_ids.size(); ++i) {
    if (i > 0) j += ",";
    j += std::to_string(a.device_ids[i]);
  }
  j += "],\"key_bindings\":[";
  for (size_t i = 0; i < a.key_bindings.size(); ++i) {
    if (i > 0) j += ",";
    j += "{\"key\":" + std::to_string(static_cast<int>(a.key_bindings[i].key_char));
    j += ",\"device_id\":" + std::to_string(a.key_bindings[i].device_id);
    j += ",\"command_name\":\"" + json_escape(a.key_bindings[i].command_name) + "\"}";
  }
  j += "],\"startup_actions\":[";
  for (size_t i = 0; i < a.startup_actions.size(); ++i) {
    if (i > 0) j += ",";
    j += "{\"device_id\":" + std::to_string(a.startup_actions[i].device_id);
    j += ",\"slot\":\"" + std::string(to_string(a.startup_actions[i].slot)) + "\"}";
  }
  j += "]}";
  return j;
}

bool SerialHandler::json_to_device(const std::string& json, DeviceRecord* out) {
  if (!out) return false;
  out->name = extract_json_string(json, "name");
  if (out->name.empty()) return false;
  std::string type_str = extract_json_string(json, "type");
  if (!type_str.empty()) out->type = device_type_from_string(type_str);
  std::string transport_str = extract_json_string(json, "transport");
  if (!transport_str.empty()) out->transport = transport_type_from_string(transport_str);
  int proto = extract_json_int(json, "ir_protocol", -1);
  if (proto >= 0) out->ir_protocol = proto;
  std::string proto_name = extract_json_string(json, "ir_protocol_name");
  if (!proto_name.empty()) {
    int resolved = protocol_from_name(proto_name);
    if (resolved >= 0) out->ir_protocol = resolved;
  }
  out->enabled = extract_json_bool(json, "enabled", true);
  out->address = extract_json_string(json, "address");

  // Parse commands array
  out->commands.clear();
  size_t cmds_pos = json.find("\"commands\"");
  if (cmds_pos != std::string::npos) {
    size_t arr_start = json.find('[', cmds_pos);
    if (arr_start != std::string::npos) {
      // Find matching ]
      int depth = 1;
      size_t arr_end = arr_start + 1;
      for (; arr_end < json.size() && depth > 0; ++arr_end) {
        if (json[arr_end] == '[') ++depth;
        else if (json[arr_end] == ']') --depth;
      }
      // Parse individual command objects
      size_t pos = arr_start + 1;
      while (pos < arr_end) {
        size_t obj_start = json.find('{', pos);
        if (obj_start == std::string::npos || obj_start >= arr_end) break;
        int obj_depth = 1;
        size_t obj_end = obj_start + 1;
        for (; obj_end < json.size() && obj_depth > 0; ++obj_end) {
          if (json[obj_end] == '{') ++obj_depth;
          else if (json[obj_end] == '}') --obj_depth;
        }
        std::string cmd_json = json.substr(obj_start, obj_end - obj_start);
        std::string cname = extract_json_string(cmd_json, "name");
        std::string cpayload = extract_json_string(cmd_json, "payload");
        if (!cname.empty()) {
          DeviceCommand dc;
          dc.name = cname;
          dc.payload = cpayload;
          out->commands.push_back(dc);
        }
        pos = obj_end;
      }
    }
  }
  return true;
}

bool SerialHandler::json_to_activity(const std::string& json,
                                      ActivityRecord* out) {
  if (!out) return false;
  out->name = extract_json_string(json, "name");
  if (out->name.empty()) return false;

  // device_ids array
  out->device_ids.clear();
  size_t dids_pos = json.find("\"device_ids\"");
  if (dids_pos != std::string::npos) {
    size_t arr_start = json.find('[', dids_pos);
    if (arr_start != std::string::npos) {
      size_t arr_end = json.find(']', arr_start);
      if (arr_end != std::string::npos) {
        std::string inner = json.substr(arr_start + 1, arr_end - arr_start - 1);
        size_t p = 0;
        while (p < inner.size()) {
          while (p < inner.size() && (inner[p] == ' ' || inner[p] == ',')) ++p;
          if (p >= inner.size()) break;
          char* ep = nullptr;
          unsigned long val = strtoul(inner.c_str() + p, &ep, 10);
          if (ep == inner.c_str() + p) break;
          out->device_ids.push_back(static_cast<uint32_t>(val));
          p = static_cast<size_t>(ep - inner.c_str());
        }
      }
    }
  }

  // key_bindings array
  out->key_bindings.clear();
  size_t kb_pos = json.find("\"key_bindings\"");
  if (kb_pos != std::string::npos) {
    size_t arr_start = json.find('[', kb_pos);
    if (arr_start != std::string::npos) {
      int depth = 1;
      size_t arr_end = arr_start + 1;
      for (; arr_end < json.size() && depth > 0; ++arr_end) {
        if (json[arr_end] == '[') ++depth;
        else if (json[arr_end] == ']') --depth;
      }
      size_t pos = arr_start + 1;
      while (pos < arr_end) {
        size_t obj_start = json.find('{', pos);
        if (obj_start == std::string::npos || obj_start >= arr_end) break;
        int obj_depth = 1;
        size_t obj_end = obj_start + 1;
        for (; obj_end < json.size() && obj_depth > 0; ++obj_end) {
          if (json[obj_end] == '{') ++obj_depth;
          else if (json[obj_end] == '}') --obj_depth;
        }
        std::string kb_json = json.substr(obj_start, obj_end - obj_start);
        ActivityKeyBinding kb;
        kb.key_char = static_cast<char>(extract_json_int(kb_json, "key", 0));
        kb.device_id = static_cast<uint32_t>(extract_json_int(kb_json, "device_id", 0));
        kb.command_name = extract_json_string(kb_json, "command_name");
        if (kb.key_char != '\0') out->key_bindings.push_back(kb);
        pos = obj_end;
      }
    }
  }

  // startup_actions array
  out->startup_actions.clear();
  size_t sa_pos = json.find("\"startup_actions\"");
  if (sa_pos != std::string::npos) {
    size_t arr_start = json.find('[', sa_pos);
    if (arr_start != std::string::npos) {
      int depth = 1;
      size_t arr_end = arr_start + 1;
      for (; arr_end < json.size() && depth > 0; ++arr_end) {
        if (json[arr_end] == '[') ++depth;
        else if (json[arr_end] == ']') --depth;
      }
      size_t pos = arr_start + 1;
      while (pos < arr_end) {
        size_t obj_start = json.find('{', pos);
        if (obj_start == std::string::npos || obj_start >= arr_end) break;
        int obj_depth = 1;
        size_t obj_end = obj_start + 1;
        for (; obj_end < json.size() && obj_depth > 0; ++obj_end) {
          if (json[obj_end] == '{') ++obj_depth;
          else if (json[obj_end] == '}') --obj_depth;
        }
        std::string sa_json = json.substr(obj_start, obj_end - obj_start);
        ActivityStartupAction sa;
        sa.device_id = static_cast<uint32_t>(extract_json_int(sa_json, "device_id", 0));
        std::string slot_str = extract_json_string(sa_json, "slot");
        CommandSlot slot;
        if (command_slot_from_string(slot_str, &slot)) {
          sa.slot = slot;
        }
        out->startup_actions.push_back(sa);
        pos = obj_end;
      }
    }
  }
  return true;
}

// -------------------------------------------------------------------
// Poll - non-blocking serial read
// -------------------------------------------------------------------

void SerialHandler::poll() {
  if (!Serial.available()) return;
  // Keep device awake while serial is active.
  setLastActivityTimestamp_HAL();
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      if (!line_buf_.empty()) {
        process_line(line_buf_);
        line_buf_.clear();
      }
      continue;
    }
    if (line_buf_.size() < kMaxLineLen) {
      line_buf_.push_back(static_cast<char>(c));
    }
  }
}

void SerialHandler::process_line(const std::string& line) {
  if (line.size() < 2 || line[0] != '@' || line[1] != '@') return;
  std::string json = line.substr(2);

  std::string cmd = extract_json_string(json, "cmd");
  if (cmd.empty()) return;
  std::string req_id = extract_json_string(json, "id");

  handle_command(json, cmd, req_id);
}

void SerialHandler::handle_command(const std::string& json,
                                    const std::string& cmd,
                                    const std::string& id) {
  if (cmd == "ping") { cmd_ping(id); }
  else if (cmd == "status") { cmd_status(id); }
  else if (cmd == "meta") { cmd_meta(id); }
  else if (cmd == "dev_list") { cmd_dev_list(id); }
  else if (cmd == "dev_get") { cmd_dev_get(json, id); }
  else if (cmd == "dev_add") { cmd_dev_add(json, id); }
  else if (cmd == "dev_update") { cmd_dev_update(json, id); }
  else if (cmd == "dev_delete") { cmd_dev_delete(json, id); }
  else if (cmd == "act_list") { cmd_act_list(id); }
  else if (cmd == "act_get") { cmd_act_get(json, id); }
  else if (cmd == "act_add") { cmd_act_add(json, id); }
  else if (cmd == "act_update") { cmd_act_update(json, id); }
  else if (cmd == "act_delete") { cmd_act_delete(json, id); }
  else if (cmd == "dispatch") { cmd_dispatch(json, id); }
  else if (cmd == "backup_sd") { cmd_backup_sd(id); }
  else if (cmd == "backup_list") { cmd_backup_list(id); }
  else if (cmd == "restore_sd") { cmd_restore_sd(json, id); }
  else if (cmd == "backup_export") { cmd_backup_export(id); }
  else if (cmd == "backup_import") { cmd_backup_import(json, id); }
  else if (cmd == "sd_write_start") { cmd_sd_write_start(json, id); }
  else if (cmd == "sd_write_chunk") { cmd_sd_write_chunk(json, id); }
  else if (cmd == "sd_write_end") { cmd_sd_write_end(id); }
  else if (cmd == "sd_read_start") { cmd_sd_read_start(json, id); }
  else if (cmd == "sd_read_chunk") { cmd_sd_read_chunk(json, id); }
  else if (cmd == "sd_read_end") { cmd_sd_read_end(id); }
  else {
    send_err(cmd, id, "unknown command");
  }
}

// -------------------------------------------------------------------
// Command: ping
// -------------------------------------------------------------------

void SerialHandler::cmd_ping(const std::string& id) {
  send_ok("ping", id, "{\"pong\":true}");
}

// -------------------------------------------------------------------
// Command: status
// -------------------------------------------------------------------

void SerialHandler::cmd_status(const std::string& id) {
  int battery_mv = 0, battery_pct = 0;
  bool battery_charging = false;
  get_battery_status_HAL(&battery_mv, &battery_pct, &battery_charging);

  std::string d = "{";
  d += "\"battery_mv\":" + std::to_string(battery_mv);
  d += ",\"battery_pct\":" + std::to_string(battery_pct);
  d += ",\"battery_charging\":" + std::string(battery_charging ? "true" : "false");
  d += ",\"free_heap\":" + std::to_string(static_cast<unsigned long>(ESP.getFreeHeap()));
  d += ",\"uptime_ms\":" + std::to_string(static_cast<unsigned long>(millis()));
  d += ",\"device_count\":" + std::to_string(devices_.count());
  d += ",\"activity_count\":" + std::to_string(activities_.count());
#if (ENABLE_WIFI_AND_MQTT == 1)
  d += ",\"mqtt_connected\":" + std::string(mqtt_is_connected_HAL() ? "true" : "false");
#else
  d += ",\"mqtt_connected\":false";
#endif
  d += "}";
  send_ok("status", id, d);
}

// -------------------------------------------------------------------
// Command: meta
// -------------------------------------------------------------------

void SerialHandler::cmd_meta(const std::string& id) {
  std::string d = "{";

  // Device types
  d += "\"device_types\":[\"TV\",\"AVR\",\"MediaPlayer\",\"SmartHome\",\"Lighting\",\"Custom\"]";

  // Transport types
  d += ",\"transport_types\":[\"IR\",\"BLE\",\"MQTT\",\"HTTP\"]";

  // IR protocols
  d += ",\"ir_protocols\":[";
  const std::vector<int>& protos = supported_ir_protocols();
  for (size_t i = 0; i < protos.size(); ++i) {
    if (i > 0) d += ",";
    d += "{\"id\":" + std::to_string(protos[i]);
    d += ",\"name\":\"" + json_escape(protocol_name(protos[i])) + "\"}";
  }
  d += "]";

  // Command slots
  d += ",\"command_slots\":[";
  const std::vector<CommandSlot>& slots = all_command_slots();
  for (size_t i = 0; i < slots.size(); ++i) {
    if (i > 0) d += ",";
    d += "\"" + std::string(to_string(slots[i])) + "\"";
  }
  d += "]";

  // Common command names
  d += ",\"common_commands\":[";
  const std::vector<std::string>& common = common_command_names();
  for (size_t i = 0; i < common.size(); ++i) {
    if (i > 0) d += ",";
    d += "\"" + json_escape(common[i]) + "\"";
  }
  d += "]";

  d += "}";
  send_ok("meta", id, d);
}

// -------------------------------------------------------------------
// Device CRUD
// -------------------------------------------------------------------

void SerialHandler::cmd_dev_list(const std::string& id) {
  const auto& all = devices_.all();
  std::string d = "{\"devices\":[";
  for (size_t i = 0; i < all.size(); ++i) {
    if (i > 0) d += ",";
    d += device_to_json(all[i]);
  }
  d += "]}";
  send_ok("dev_list", id, d);
}

void SerialHandler::cmd_dev_get(const std::string& json,
                                 const std::string& id) {
  int dev_id = extract_json_int(json, "dev_id", 0);
  if (dev_id <= 0) {
    // Also try "id" field for convenience
    dev_id = extract_json_int(json, "dev_id", 0);
    std::string dev_id_str = extract_json_string(json, "dev_id");
    if (!dev_id_str.empty()) {
      char* ep = nullptr;
      long v = strtol(dev_id_str.c_str(), &ep, 10);
      if (ep != dev_id_str.c_str()) dev_id = static_cast<int>(v);
    }
  }
  const DeviceRecord* dev = devices_.get_by_id(static_cast<uint32_t>(dev_id));
  if (!dev) {
    send_err("dev_get", id, "device not found");
    return;
  }
  send_ok("dev_get", id, device_to_json(*dev));
}

void SerialHandler::cmd_dev_add(const std::string& json,
                                 const std::string& id) {
  DeviceRecord rec;
  if (!json_to_device(json, &rec)) {
    send_err("dev_add", id, "invalid device data");
    return;
  }
  rec.id = 0;  // Registry assigns ID
  if (!devices_.add(rec)) {
    send_err("dev_add", id, "add failed");
    return;
  }
  devices_.save();
  // Return the newly added device (last in list)
  const auto& all = devices_.all();
  if (!all.empty()) {
    send_ok("dev_add", id, device_to_json(all.back()));
  } else {
    send_ok("dev_add", id);
  }
}

void SerialHandler::cmd_dev_update(const std::string& json,
                                    const std::string& id) {
  int dev_id = extract_json_int(json, "dev_id", 0);
  std::string dev_id_str = extract_json_string(json, "dev_id");
  if (!dev_id_str.empty()) {
    char* ep = nullptr;
    long v = strtol(dev_id_str.c_str(), &ep, 10);
    if (ep != dev_id_str.c_str()) dev_id = static_cast<int>(v);
  }
  if (dev_id <= 0) {
    send_err("dev_update", id, "missing dev_id");
    return;
  }
  DeviceRecord* existing = devices_.get_by_id(static_cast<uint32_t>(dev_id));
  if (!existing) {
    send_err("dev_update", id, "device not found");
    return;
  }

  DeviceRecord updated = *existing;
  // Apply partial updates
  std::string name = extract_json_string(json, "name");
  if (!name.empty()) updated.name = name;
  std::string type_str = extract_json_string(json, "type");
  if (!type_str.empty()) updated.type = device_type_from_string(type_str);
  std::string transport_str = extract_json_string(json, "transport");
  if (!transport_str.empty()) updated.transport = transport_type_from_string(transport_str);
  int proto = extract_json_int(json, "ir_protocol", -1);
  if (proto >= 0) updated.ir_protocol = proto;
  std::string proto_name = extract_json_string(json, "ir_protocol_name");
  if (!proto_name.empty()) {
    int resolved = protocol_from_name(proto_name);
    if (resolved >= 0) updated.ir_protocol = resolved;
  }
  std::string address = extract_json_string(json, "address");
  if (!address.empty()) updated.address = address;

  // Check if "enabled" was explicitly set
  const std::string en_pattern = "\"enabled\"";
  if (json.find(en_pattern) != std::string::npos) {
    updated.enabled = extract_json_bool(json, "enabled", updated.enabled);
  }

  // If commands array is present, replace it
  if (json.find("\"commands\"") != std::string::npos) {
    DeviceRecord temp;
    temp.name = "tmp";
    json_to_device(json, &temp);
    updated.commands = std::move(temp.commands);
  }

  if (!devices_.upsert(updated)) {
    send_err("dev_update", id, "upsert failed");
    return;
  }
  devices_.save();
  const DeviceRecord* refreshed = devices_.get_by_id(static_cast<uint32_t>(dev_id));
  if (refreshed) {
    send_ok("dev_update", id, device_to_json(*refreshed));
  } else {
    send_ok("dev_update", id);
  }
}

void SerialHandler::cmd_dev_delete(const std::string& json,
                                    const std::string& id) {
  int dev_id = extract_json_int(json, "dev_id", 0);
  std::string dev_id_str = extract_json_string(json, "dev_id");
  if (!dev_id_str.empty()) {
    char* ep = nullptr;
    long v = strtol(dev_id_str.c_str(), &ep, 10);
    if (ep != dev_id_str.c_str()) dev_id = static_cast<int>(v);
  }
  if (dev_id <= 0) {
    send_err("dev_delete", id, "missing dev_id");
    return;
  }
  const auto& all = devices_.all();
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i].id == static_cast<uint32_t>(dev_id)) {
      devices_.remove_by_index(i);
      devices_.save();
      send_ok("dev_delete", id);
      return;
    }
  }
  send_err("dev_delete", id, "device not found");
}

// -------------------------------------------------------------------
// Activity CRUD
// -------------------------------------------------------------------

void SerialHandler::cmd_act_list(const std::string& id) {
  const auto& all = activities_.all();
  std::string d = "{\"activities\":[";
  for (size_t i = 0; i < all.size(); ++i) {
    if (i > 0) d += ",";
    d += activity_to_json(all[i]);
  }
  d += "]}";
  send_ok("act_list", id, d);
}

void SerialHandler::cmd_act_get(const std::string& json,
                                 const std::string& id) {
  int act_id = extract_json_int(json, "act_id", 0);
  std::string act_id_str = extract_json_string(json, "act_id");
  if (!act_id_str.empty()) {
    char* ep = nullptr;
    long v = strtol(act_id_str.c_str(), &ep, 10);
    if (ep != act_id_str.c_str()) act_id = static_cast<int>(v);
  }
  const ActivityRecord* act = activities_.get_by_id(static_cast<uint32_t>(act_id));
  if (!act) {
    send_err("act_get", id, "activity not found");
    return;
  }
  send_ok("act_get", id, activity_to_json(*act));
}

void SerialHandler::cmd_act_add(const std::string& json,
                                 const std::string& id) {
  ActivityRecord rec;
  if (!json_to_activity(json, &rec)) {
    send_err("act_add", id, "invalid activity data");
    return;
  }
  rec.id = 0;
  if (!activities_.add(rec)) {
    send_err("act_add", id, "add failed");
    return;
  }
  activities_.save();
  const auto& all = activities_.all();
  if (!all.empty()) {
    send_ok("act_add", id, activity_to_json(all.back()));
  } else {
    send_ok("act_add", id);
  }
}

void SerialHandler::cmd_act_update(const std::string& json,
                                    const std::string& id) {
  int act_id = extract_json_int(json, "act_id", 0);
  std::string act_id_str = extract_json_string(json, "act_id");
  if (!act_id_str.empty()) {
    char* ep = nullptr;
    long v = strtol(act_id_str.c_str(), &ep, 10);
    if (ep != act_id_str.c_str()) act_id = static_cast<int>(v);
  }
  if (act_id <= 0) {
    send_err("act_update", id, "missing act_id");
    return;
  }
  const ActivityRecord* existing = activities_.get_by_id(static_cast<uint32_t>(act_id));
  if (!existing) {
    send_err("act_update", id, "activity not found");
    return;
  }

  ActivityRecord updated = *existing;
  std::string name = extract_json_string(json, "name");
  if (!name.empty()) updated.name = name;

  if (json.find("\"device_ids\"") != std::string::npos) {
    ActivityRecord temp;
    temp.name = "tmp";
    json_to_activity(json, &temp);
    updated.device_ids = std::move(temp.device_ids);
  }
  if (json.find("\"key_bindings\"") != std::string::npos) {
    ActivityRecord temp;
    temp.name = "tmp";
    json_to_activity(json, &temp);
    updated.key_bindings = std::move(temp.key_bindings);
  }
  if (json.find("\"startup_actions\"") != std::string::npos) {
    ActivityRecord temp;
    temp.name = "tmp";
    json_to_activity(json, &temp);
    updated.startup_actions = std::move(temp.startup_actions);
  }

  if (!activities_.upsert(updated)) {
    send_err("act_update", id, "upsert failed");
    return;
  }
  activities_.save();
  const ActivityRecord* refreshed = activities_.get_by_id(static_cast<uint32_t>(act_id));
  if (refreshed) {
    send_ok("act_update", id, activity_to_json(*refreshed));
  } else {
    send_ok("act_update", id);
  }
}

void SerialHandler::cmd_act_delete(const std::string& json,
                                    const std::string& id) {
  int act_id = extract_json_int(json, "act_id", 0);
  std::string act_id_str = extract_json_string(json, "act_id");
  if (!act_id_str.empty()) {
    char* ep = nullptr;
    long v = strtol(act_id_str.c_str(), &ep, 10);
    if (ep != act_id_str.c_str()) act_id = static_cast<int>(v);
  }
  if (act_id <= 0) {
    send_err("act_delete", id, "missing act_id");
    return;
  }
  const auto& all = activities_.all();
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i].id == static_cast<uint32_t>(act_id)) {
      activities_.remove_by_index(i);
      activities_.save();
      send_ok("act_delete", id);
      return;
    }
  }
  send_err("act_delete", id, "activity not found");
}

// -------------------------------------------------------------------
// Command: dispatch
// -------------------------------------------------------------------

void SerialHandler::cmd_dispatch(const std::string& json,
                                  const std::string& id) {
  int dev_id = extract_json_int(json, "device_id", 0);
  std::string dev_id_str = extract_json_string(json, "device_id");
  if (!dev_id_str.empty()) {
    char* ep = nullptr;
    long v = strtol(dev_id_str.c_str(), &ep, 10);
    if (ep != dev_id_str.c_str()) dev_id = static_cast<int>(v);
  }
  std::string command_name = extract_json_string(json, "command");
  if (dev_id <= 0 || command_name.empty()) {
    send_err("dispatch", id, "missing device_id or command");
    return;
  }
  bool ok = dispatcher_.dispatch_device_command(static_cast<uint32_t>(dev_id), command_name);
  const DispatchResult& result = dispatcher_.last_result();
  std::string d = "{";
  d += "\"sent\":" + std::string(ok ? "true" : "false");
  d += ",\"code\":" + std::to_string(static_cast<int>(result.code));
  d += ",\"detail\":\"" + json_escape(result.detail) + "\"";
  d += "}";
  send_ok("dispatch", id, d);
}

// -------------------------------------------------------------------
// Backup / Restore
// -------------------------------------------------------------------

void SerialHandler::cmd_backup_sd(const std::string& id) {
  std::string status;
  bool ok = backup_.backup_to_sd(devices_.all(), activities_.all(), &status);
  if (ok) {
    std::string d = "{\"status\":\"" + json_escape(status) + "\"}";
    send_ok("backup_sd", id, d);
  } else {
    send_err("backup_sd", id, status);
  }
}

void SerialHandler::cmd_backup_list(const std::string& id) {
  std::vector<SdBackupEntry> entries;
  std::string status;
  bool ok = backup_.list_backups(&entries, &status);
  if (!ok) {
    send_err("backup_list", id, status);
    return;
  }
  std::string d = "{\"backups\":[";
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i > 0) d += ",";
    d += "{\"path\":\"" + json_escape(entries[i].path) + "\"";
    d += ",\"label\":\"" + json_escape(entries[i].label) + "\"}";
  }
  d += "]}";
  send_ok("backup_list", id, d);
}

void SerialHandler::cmd_restore_sd(const std::string& json,
                                    const std::string& id) {
  std::string path = extract_json_string(json, "path");
  if (path.empty()) {
    send_err("restore_sd", id, "missing path");
    return;
  }
  std::vector<DeviceRecord> devices;
  std::vector<ActivityRecord> activities;
  std::string status;
  if (!backup_.restore_from_sd(path, &devices, &activities, &status)) {
    send_err("restore_sd", id, status);
    return;
  }
  devices_.replace_all(std::move(devices));
  devices_.save();
  activities_.replace_all(std::move(activities));
  activities_.save();
  std::string d = "{\"status\":\"" + json_escape(status) + "\"";
  d += ",\"device_count\":" + std::to_string(devices_.count());
  d += ",\"activity_count\":" + std::to_string(activities_.count());
  d += "}";
  send_ok("restore_sd", id, d);
}

// -------------------------------------------------------------------
// Backup export / import (text format over serial)
// -------------------------------------------------------------------

void SerialHandler::cmd_backup_export(const std::string& id) {
  std::string payload = SdBackupService::serialize_to_text(devices_.all(), activities_.all());
  // Split into lines and send as JSON array of strings
  std::vector<std::string> lines;
  std::stringstream ss(payload);
  std::string row;
  while (std::getline(ss, row)) {
    if (!row.empty() && row.back() == '\r') row.pop_back();
    lines.push_back(row);
  }

  std::string d = "{\"lines\":[";
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) d += ",";
    d += "\"" + json_escape(lines[i]) + "\"";
  }
  d += "]}";
  send_ok("backup_export", id, d);
}

void SerialHandler::cmd_backup_import(const std::string& json,
                                       const std::string& id) {
  std::vector<std::string> lines = extract_json_string_array(json, "lines");
  if (lines.empty()) {
    send_err("backup_import", id, "missing lines array");
    return;
  }
  // Reassemble text
  std::string text;
  for (const std::string& line : lines) {
    text += line + "\n";
  }

  std::vector<DeviceRecord> devices;
  std::vector<ActivityRecord> activities;
  std::string status;
  if (!SdBackupService::parse_from_text(text, &devices, &activities, &status)) {
    send_err("backup_import", id, status);
    return;
  }
  devices_.replace_all(std::move(devices));
  devices_.save();
  activities_.replace_all(std::move(activities));
  activities_.save();
  std::string d = "{\"status\":\"import complete\"";
  d += ",\"device_count\":" + std::to_string(devices_.count());
  d += ",\"activity_count\":" + std::to_string(activities_.count());
  d += "}";
  send_ok("backup_import", id, d);
}

// -------------------------------------------------------------------
// SD file transfer: write
// -------------------------------------------------------------------

void SerialHandler::cmd_sd_write_start(const std::string& json,
                                        const std::string& id) {
  if (sd_write_open_) {
    g_transfer_file.close();
    sd_write_open_ = false;
  }
  std::string path = extract_json_string(json, "path");
  if (path.empty()) {
    send_err("sd_write_start", id, "missing path");
    return;
  }
  if (!serial_mount_sd()) {
    send_err("sd_write_start", id, "SD mount failed");
    return;
  }
  g_transfer_file = g_serial_sd.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!g_transfer_file) {
    send_err("sd_write_start", id, "open failed");
    return;
  }
  sd_write_open_ = true;
  sd_transfer_path_ = path;
  sd_write_bytes_ = 0;
  send_ok("sd_write_start", id, "{\"path\":\"" + json_escape(path) + "\"}");
}

void SerialHandler::cmd_sd_write_chunk(const std::string& json,
                                        const std::string& id) {
  if (!sd_write_open_) {
    send_err("sd_write_chunk", id, "no write in progress");
    return;
  }
  std::string b64 = extract_json_string(json, "data");
  if (b64.empty()) {
    send_err("sd_write_chunk", id, "missing data");
    return;
  }
  std::vector<uint8_t> decoded = base64_decode(b64);
  if (decoded.empty()) {
    send_err("sd_write_chunk", id, "decode failed");
    return;
  }
  size_t written = g_transfer_file.write(decoded.data(), decoded.size());
  sd_write_bytes_ += written;
  send_ok("sd_write_chunk", id, "{\"bytes\":" + std::to_string(written) + "}");
}

void SerialHandler::cmd_sd_write_end(const std::string& id) {
  if (!sd_write_open_) {
    send_err("sd_write_end", id, "no write in progress");
    return;
  }
  g_transfer_file.close();
  sd_write_open_ = false;
  std::string d = "{\"path\":\"" + json_escape(sd_transfer_path_) + "\"";
  d += ",\"bytes\":" + std::to_string(sd_write_bytes_) + "}";
  send_ok("sd_write_end", id, d);
}

// -------------------------------------------------------------------
// SD file transfer: read
// -------------------------------------------------------------------

void SerialHandler::cmd_sd_read_start(const std::string& json,
                                       const std::string& id) {
  if (sd_read_open_) {
    g_transfer_file.close();
    sd_read_open_ = false;
  }
  std::string path = extract_json_string(json, "path");
  if (path.empty()) {
    send_err("sd_read_start", id, "missing path");
    return;
  }
  if (!serial_mount_sd()) {
    send_err("sd_read_start", id, "SD mount failed");
    return;
  }
  g_transfer_file = g_serial_sd.open(path.c_str(), O_RDONLY);
  if (!g_transfer_file) {
    send_err("sd_read_start", id, "file not found");
    return;
  }
  sd_read_open_ = true;
  sd_transfer_path_ = path;
  sd_read_size_ = static_cast<size_t>(g_transfer_file.size());
  std::string d = "{\"path\":\"" + json_escape(path) + "\"";
  d += ",\"size\":" + std::to_string(sd_read_size_) + "}";
  send_ok("sd_read_start", id, d);
}

void SerialHandler::cmd_sd_read_chunk(const std::string& json,
                                       const std::string& id) {
  if (!sd_read_open_) {
    send_err("sd_read_chunk", id, "no read in progress");
    return;
  }
  int offset = extract_json_int(json, "offset", -1);
  int size = extract_json_int(json, "size", 512);
  if (size <= 0 || size > 4096) size = 512;
  if (offset >= 0) {
    g_transfer_file.seekSet(static_cast<uint64_t>(offset));
  }
  std::vector<uint8_t> buf(static_cast<size_t>(size));
  int bytes_read = g_transfer_file.read(buf.data(), static_cast<size_t>(size));
  if (bytes_read < 0) bytes_read = 0;
  std::string b64 = base64_encode(buf.data(), static_cast<size_t>(bytes_read));
  std::string d = "{\"data\":\"" + b64 + "\"";
  d += ",\"bytes\":" + std::to_string(bytes_read);
  d += ",\"eof\":" + std::string(g_transfer_file.available() ? "false" : "true");
  d += "}";
  send_ok("sd_read_chunk", id, d);
}

void SerialHandler::cmd_sd_read_end(const std::string& id) {
  if (!sd_read_open_) {
    send_err("sd_read_end", id, "no read in progress");
    return;
  }
  g_transfer_file.close();
  sd_read_open_ = false;
  send_ok("sd_read_end", id);
}

}  // namespace omote_v2
