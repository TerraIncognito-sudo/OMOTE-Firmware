#pragma once

#include <cstddef>
#include <vector>

#include "app/activity_model.h"
#include "app/activity_storage.h"

namespace omote_v2 {

class ActivityRegistry {
 public:
  bool load();
  bool save();
  const std::vector<ActivityRecord>& all() const;
  bool add(ActivityRecord record);
  bool remove_by_index(size_t index);
  bool upsert(ActivityRecord record);
  bool replace_all(std::vector<ActivityRecord> records);
  const ActivityRecord* get_by_id(uint32_t id) const;
  size_t count() const;

 private:
  uint32_t next_id();

  ActivityStorage storage_;
  std::vector<ActivityRecord> activities_;
  uint32_t next_id_seed_ = 1;
};

}  // namespace omote_v2
