#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "app/activity_registry.h"
#include "app/device_registry.h"
#include "app/device_model.h"

namespace omote_v2 {

enum class DispatchResultCode : uint8_t {
  Sent = 0,
  NotMapped = 1,
  InvalidPayload = 2,
  TransportUnavailable = 3,
  SendFailed = 4,
  Debounced = 5,
  DeviceUnavailable = 6,
  UnsupportedTransport = 7,
};

struct DispatchResult {
  DispatchResultCode code = DispatchResultCode::SendFailed;
  uint32_t device_id = 0;
  std::string command_name;
  TransportType transport = TransportType::IR;
  std::string detail;
};

class CommandDispatcher {
 public:
  explicit CommandDispatcher(DeviceRegistry& devices);
  bool dispatch(const ActivityRecord& activity, CommandSlot slot);
  bool dispatch_device_command(uint32_t device_id, const std::string& command_name);
  const std::string& last_status() const;
  const DispatchResult& last_result() const;
  const std::vector<DispatchResult>& last_results() const;
  void set_debounce_interval_ms(unsigned long debounce_interval_ms);
  unsigned long debounce_interval_ms() const;

 private:
  void set_last_result(DispatchResultCode code, uint32_t device_id, const std::string& command_name,
                       TransportType transport, const std::string& detail);
  static const char* result_code_text(DispatchResultCode code);
  bool is_debounced(uint32_t device_id, const std::string& command_name);
  void mark_sent(uint32_t device_id, const std::string& command_name);
  std::string debounce_key(uint32_t device_id, const std::string& command_name) const;
  bool dispatch_device(const DeviceRecord& device, CommandSlot slot);
  bool dispatch_device_command(const DeviceRecord& device, const std::string& command_name);

  DeviceRegistry& devices_;
  std::string last_status_;
  DispatchResult last_result_;
  std::vector<DispatchResult> last_results_;
  std::unordered_map<std::string, unsigned long> last_sent_ms_;
  unsigned long debounce_interval_ms_ = 140;
};

}  // namespace omote_v2
