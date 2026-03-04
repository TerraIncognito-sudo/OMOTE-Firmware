#pragma once

#include <unordered_map>
#include <string>
#include <vector>

#include "app/activity_model.h"
#include "app/device_model.h"

namespace omote_v2 {

struct SdBackupEntry {
  std::string path;
  std::string label;
};

class SdBackupService {
 public:
  bool backup_to_sd(const std::vector<DeviceRecord>& devices, const std::vector<ActivityRecord>& activities, std::string* out_status) const;
  bool list_backups(std::vector<SdBackupEntry>* out_backups, std::string* out_status) const;
  bool restore_from_sd(const std::string& backup_path, std::vector<DeviceRecord>* out_devices, std::vector<ActivityRecord>* out_activities,
                       std::string* out_status) const;
  bool load_icon_pack(std::unordered_map<std::string, std::string>* out_icon_overrides, std::string* out_status) const;

  // Serial backup export/import helpers (no SD required).
  static std::string serialize_to_text(const std::vector<DeviceRecord>& devices, const std::vector<ActivityRecord>& activities);
  static bool parse_from_text(const std::string& text, std::vector<DeviceRecord>* out_devices,
                              std::vector<ActivityRecord>* out_activities, std::string* out_status);
};

}  // namespace omote_v2
