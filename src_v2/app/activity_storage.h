#pragma once

#include <vector>

#include "app/activity_model.h"

namespace omote_v2 {

class ActivityStorage {
 public:
  bool load(std::vector<ActivityRecord>& out_records);
  bool save(const std::vector<ActivityRecord>& records);
};

}  // namespace omote_v2
