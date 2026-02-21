#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "app/device_model.h"

namespace omote_v2 {

struct ActivityKeyBinding {
  char key_char = '\0';
  uint32_t device_id = 0;
  std::string command_name;
};

struct ActivityStartupAction {
  uint32_t device_id = 0;
  CommandSlot slot = CommandSlot::Power;
};

struct ActivityRecord {
  uint32_t id = 0;
  std::string name;
  std::vector<uint32_t> device_ids;
  std::vector<ActivityKeyBinding> key_bindings;
  std::vector<ActivityStartupAction> startup_actions;
};

}  // namespace omote_v2
