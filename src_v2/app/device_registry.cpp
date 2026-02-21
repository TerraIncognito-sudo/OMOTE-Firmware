#include "app/device_registry.h"

#include <utility>

namespace omote_v2 {

bool DeviceRegistry::load() {
  if (!storage_.load(devices_)) {
    devices_.clear();
    return false;
  }

  uint32_t max_id = 0;
  for (const DeviceRecord& d : devices_) {
    if (d.id > max_id) max_id = d.id;
  }
  next_id_seed_ = max_id + 1;
  return true;
}

bool DeviceRegistry::save() {
  return storage_.save(devices_);
}

const std::vector<DeviceRecord>& DeviceRegistry::all() const {
  return devices_;
}

DeviceRecord* DeviceRegistry::get_by_id(uint32_t id) {
  for (DeviceRecord& d : devices_) {
    if (d.id == id) return &d;
  }
  return nullptr;
}

const DeviceRecord* DeviceRegistry::get_by_id(uint32_t id) const {
  for (const DeviceRecord& d : devices_) {
    if (d.id == id) return &d;
  }
  return nullptr;
}

bool DeviceRegistry::add(DeviceRecord record) {
  if (devices_.size() >= 24) {
    return false;
  }
  record.id = next_id();
  devices_.push_back(record);
  save();
  return true;
}

bool DeviceRegistry::remove_by_index(size_t index) {
  if (index >= devices_.size()) {
    return false;
  }
  devices_.erase(devices_.begin() + index);
  save();
  return true;
}

bool DeviceRegistry::upsert(DeviceRecord record) {
  for (DeviceRecord& d : devices_) {
    if (d.id == record.id) {
      d = record;
      save();
      return true;
    }
  }
  return add(record);
}

bool DeviceRegistry::replace_all(std::vector<DeviceRecord> records) {
  devices_ = std::move(records);
  uint32_t max_id = 0;
  for (const DeviceRecord& d : devices_) {
    if (d.id > max_id) max_id = d.id;
  }
  next_id_seed_ = max_id + 1;
  return save();
}

size_t DeviceRegistry::count() const {
  return devices_.size();
}

uint32_t DeviceRegistry::next_id() {
  return next_id_seed_++;
}

}  // namespace omote_v2
