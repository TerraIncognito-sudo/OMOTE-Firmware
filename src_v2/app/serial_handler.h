#pragma once

#include <string>
#include <vector>

#include "app/activity_registry.h"
#include "app/command_dispatcher.h"
#include "app/device_registry.h"
#include "app/sd_backup_service.h"

namespace omote_v2 {

class SerialHandler {
 public:
  SerialHandler(DeviceRegistry& devices, ActivityRegistry& activities,
                CommandDispatcher& dispatcher, SdBackupService& backup);

  void poll();

 private:
  void process_line(const std::string& line);
  void handle_command(const std::string& json, const std::string& cmd,
                      const std::string& req_id);

  // Command handlers
  void cmd_ping(const std::string& id);
  void cmd_status(const std::string& id);
  void cmd_meta(const std::string& id);

  void cmd_dev_list(const std::string& id);
  void cmd_dev_get(const std::string& json, const std::string& id);
  void cmd_dev_add(const std::string& json, const std::string& id);
  void cmd_dev_update(const std::string& json, const std::string& id);
  void cmd_dev_delete(const std::string& json, const std::string& id);

  void cmd_act_list(const std::string& id);
  void cmd_act_get(const std::string& json, const std::string& id);
  void cmd_act_add(const std::string& json, const std::string& id);
  void cmd_act_update(const std::string& json, const std::string& id);
  void cmd_act_delete(const std::string& json, const std::string& id);

  void cmd_dispatch(const std::string& json, const std::string& id);

  void cmd_backup_sd(const std::string& id);
  void cmd_backup_list(const std::string& id);
  void cmd_restore_sd(const std::string& json, const std::string& id);
  void cmd_backup_export(const std::string& id);
  void cmd_backup_import(const std::string& json, const std::string& id);

  void cmd_sd_write_start(const std::string& json, const std::string& id);
  void cmd_sd_write_chunk(const std::string& json, const std::string& id);
  void cmd_sd_write_end(const std::string& id);
  void cmd_sd_read_start(const std::string& json, const std::string& id);
  void cmd_sd_read_chunk(const std::string& json, const std::string& id);
  void cmd_sd_read_end(const std::string& id);

  // JSON helpers
  std::string device_to_json(const DeviceRecord& d);
  std::string activity_to_json(const ActivityRecord& a);
  bool json_to_device(const std::string& json, DeviceRecord* out);
  bool json_to_activity(const std::string& json, ActivityRecord* out);

  // Response helpers
  void send_ok(const std::string& cmd, const std::string& id,
               const std::string& data_json = "{}");
  void send_err(const std::string& cmd, const std::string& id,
                const std::string& msg);

  // JSON field extraction
  static std::string extract_json_string(const std::string& json,
                                         const std::string& key);
  static int extract_json_int(const std::string& json, const std::string& key,
                              int default_val = 0);
  static bool extract_json_bool(const std::string& json,
                                const std::string& key,
                                bool default_val = false);
  static std::string json_escape(const std::string& in);
  static bool json_unescape(const std::string& in, std::string* out);

  // Base64
  static std::string base64_encode(const uint8_t* data, size_t len);
  static std::vector<uint8_t> base64_decode(const std::string& encoded);

  // Extract a JSON array of strings: ["a","b","c"]
  static std::vector<std::string> extract_json_string_array(
      const std::string& json, const std::string& key);

  DeviceRegistry& devices_;
  ActivityRegistry& activities_;
  CommandDispatcher& dispatcher_;
  SdBackupService& backup_;

  std::string line_buf_;
  static constexpr size_t kMaxLineLen = 8192;

  // SD file transfer state
  bool sd_write_open_ = false;
  bool sd_read_open_ = false;
  std::string sd_transfer_path_;
  size_t sd_write_bytes_ = 0;
  size_t sd_read_size_ = 0;
};

}  // namespace omote_v2
