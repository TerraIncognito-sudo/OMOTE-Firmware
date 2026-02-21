#pragma once

#include <cstddef>
#include <vector>

#include "app/device_model.h"
#include "app/device_storage.h"

namespace omote_v2 {

class DeviceRegistry {
 public:
  bool load();
  bool save();

  const std::vector<DeviceRecord>& all() const;
  DeviceRecord* get_by_id(uint32_t id);
  const DeviceRecord* get_by_id(uint32_t id) const;
  bool add(DeviceRecord record);
  bool remove_by_index(size_t index);
  bool upsert(DeviceRecord record);
  bool replace_all(std::vector<DeviceRecord> records);
  size_t count() const;

 private:
  uint32_t next_id();

  DeviceStorage storage_;
  std::vector<DeviceRecord> devices_;
  uint32_t next_id_seed_ = 1;
};

}  // namespace omote_v2
