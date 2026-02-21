#include "app/activity_registry.h"

#include <utility>

namespace omote_v2 {

bool ActivityRegistry::load() {
  if (!storage_.load(activities_)) {
    activities_.clear();
    return false;
  }

  uint32_t max_id = 0;
  for (const ActivityRecord& a : activities_) {
    if (a.id > max_id) max_id = a.id;
  }
  next_id_seed_ = max_id + 1;
  return true;
}

bool ActivityRegistry::save() {
  return storage_.save(activities_);
}

const std::vector<ActivityRecord>& ActivityRegistry::all() const {
  return activities_;
}

bool ActivityRegistry::add(ActivityRecord record) {
  if (activities_.size() >= 12) return false;
  record.id = next_id();
  activities_.push_back(record);
  save();
  return true;
}

bool ActivityRegistry::remove_by_index(size_t index) {
  if (index >= activities_.size()) return false;
  activities_.erase(activities_.begin() + index);
  save();
  return true;
}

bool ActivityRegistry::upsert(ActivityRecord record) {
  for (ActivityRecord& activity : activities_) {
    if (activity.id == record.id) {
      activity = record;
      save();
      return true;
    }
  }
  return add(record);
}

bool ActivityRegistry::replace_all(std::vector<ActivityRecord> records) {
  activities_ = std::move(records);
  uint32_t max_id = 0;
  for (const ActivityRecord& a : activities_) {
    if (a.id > max_id) max_id = a.id;
  }
  next_id_seed_ = max_id + 1;
  return save();
}

const ActivityRecord* ActivityRegistry::get_by_id(uint32_t id) const {
  for (const ActivityRecord& a : activities_) {
    if (a.id == id) return &a;
  }
  return nullptr;
}

size_t ActivityRegistry::count() const {
  return activities_.size();
}

uint32_t ActivityRegistry::next_id() {
  return next_id_seed_++;
}

}  // namespace omote_v2
