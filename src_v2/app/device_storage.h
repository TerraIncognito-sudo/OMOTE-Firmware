#pragma once

#include <vector>

#include "app/device_model.h"

namespace omote_v2 {

class DeviceStorage {
 public:
  bool load(std::vector<DeviceRecord>& out_records);
  bool save(const std::vector<DeviceRecord>& records);
};

}  // namespace omote_v2
