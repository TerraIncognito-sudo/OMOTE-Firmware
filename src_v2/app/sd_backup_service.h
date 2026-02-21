#pragma once

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
};

}  // namespace omote_v2
