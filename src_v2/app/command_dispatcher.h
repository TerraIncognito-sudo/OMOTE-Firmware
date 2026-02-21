#pragma once

#include "app/activity_registry.h"
#include "app/device_registry.h"
#include "app/device_model.h"

namespace omote_v2 {

class CommandDispatcher {
 public:
  explicit CommandDispatcher(DeviceRegistry& devices);
  bool dispatch(const ActivityRecord& activity, CommandSlot slot);
  bool dispatch_device_command(uint32_t device_id, const std::string& command_name);

 private:
  bool dispatch_device(const DeviceRecord& device, CommandSlot slot);
  bool dispatch_device_command(const DeviceRecord& device, const std::string& command_name);

  DeviceRegistry& devices_;
};

}  // namespace omote_v2
