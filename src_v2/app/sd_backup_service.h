#pragma once

#include <string>
#include <vector>

#include "app/activity_model.h"
#include "app/device_model.h"

namespace omote_v2 {

class SdBackupService {
 public:
  bool backup_to_sd(const std::vector<DeviceRecord>& devices, const std::vector<ActivityRecord>& activities, std::string* out_status) const;
  bool restore_from_sd(std::vector<DeviceRecord>* out_devices, std::vector<ActivityRecord>* out_activities, std::string* out_status) const;
  bool format_sd_card(std::string* out_status) const;
};

}  // namespace omote_v2
