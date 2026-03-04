#include "ui/setup_ui.h"

#include <Arduino.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <stdio.h>
#include <sys/time.h>

#include <list>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Preferences.h>

#include "hardwareLayer.h"
#include "app/device_model.h"

namespace omote_v2 {

namespace {
const char* kTransportOptions = "IR\nBLE\nMQTT\nHTTP";
const char* kDeviceTypeOptions = "TV\nAVR\nMedia Player\nSmart Home\nLighting\nCustom";
const char* kCommandSlotOptions = "Power\nVolume Up\nVolume Down\nMute\nUp\nDown\nLeft\nRight\nOK\nBack\nHome";
constexpr time_t kMinValidClockEpoch = 1704067200;  // 2024-01-01 00:00:00 UTC
constexpr const char* kUiPrefsNs = "omotev2ui";
constexpr const char* kUiTimezoneKey = "timezone";
constexpr const char* kUiDebounceIntervalKey = "debounce_ms";
constexpr const char* kUiSelectedActivityIdKey = "sel_act_id";
constexpr const char* kUiSelectedTabKey = "sel_tab";
constexpr const char* kDefaultTimezoneValue = "EST5EDT,M3.2.0/2,M11.1.0/2";
SetupUi* g_active_ui = nullptr;

struct TimeZoneOption {
  const char* label;
  const char* value;
};

const TimeZoneOption kTimeZoneOptions[] = {
    {"US Eastern", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"US Central", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"US Mountain", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"US Pacific", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"UTC", "UTC0"},
};

constexpr size_t kTimeZoneOptionCount = sizeof(kTimeZoneOptions) / sizeof(kTimeZoneOptions[0]);

struct PhysicalKeyDef {
  const char* label;
  char key_char;
};

const PhysicalKeyDef kPhysicalKeys[] = {
    {"Power", 'o'},
    {"Back", 'b'},
    {"Home", 's'},
    {"Up", 'u'},
    {"Down", 'd'},
    {"Left", 'l'},
    {"Right", 'r'},
    {"OK", 'k'},
    {"VolUp", '+'},
    {"VolDown", '-'},
    {"Mute", 'm'},
    {"ChannelUp", '^'},
    {"ChannelDown", 'v'},
    {"Play", 'p'},
    {"Rewind", '<'},
    {"Forward", '>'},
    {"Info", 'i'},
    {"Red", '1'},
    {"Green", '2'},
    {"Yellow", '3'},
    {"Blue", '4'},
    {"Help", '?'},
    {"Stop", '='},
    {"Record", 'e'},
};

constexpr size_t kPhysicalKeyCount = sizeof(kPhysicalKeys) / sizeof(kPhysicalKeys[0]);

struct SleepTimeoutOption {
  const char* label;
  uint32_t timeout_ms;
};

const SleepTimeoutOption kSleepTimeoutOptions[] = {
    {"20 sec", 20000},
    {"1 min", 60000},
    {"3 min", 180000},
    {"10 min", 600000},
    {"30 min", 1800000},
};

constexpr size_t kSleepTimeoutOptionCount = sizeof(kSleepTimeoutOptions) / sizeof(kSleepTimeoutOptions[0]);

struct DebounceOption {
  const char* label;
  unsigned long interval_ms;
};

const DebounceOption kDebounceOptions[] = {
    {"Off", 0},
    {"80 ms", 80},
    {"140 ms", 140},
    {"250 ms", 250},
    {"400 ms", 400},
    {"750 ms", 750},
};

constexpr size_t kDebounceOptionCount = sizeof(kDebounceOptions) / sizeof(kDebounceOptions[0]);

struct MqttTemplateDef {
  const char* label;
  const char* command_name;
  const char* topic_suffix;
  const char* payload;
};

const MqttTemplateDef kMqttTemplates[] = {
    {"Select template", "", "", ""},
    {"Power Toggle", "Power", "POWER", "TOGGLE"},
    {"Power On", "Power", "POWER", "ON"},
    {"Power Off", "Power", "POWER", "OFF"},
    {"Volume Up", "Volume Up", "VOLUME", "UP"},
    {"Volume Down", "Volume Down", "VOLUME", "DOWN"},
    {"Mute Toggle", "Mute", "MUTE", "TOGGLE"},
    {"Navigate Up", "Up", "NAV", "UP"},
    {"Navigate Down", "Down", "NAV", "DOWN"},
    {"Navigate Left", "Left", "NAV", "LEFT"},
    {"Navigate Right", "Right", "NAV", "RIGHT"},
    {"Select OK", "OK", "NAV", "OK"},
    {"Back", "Back", "NAV", "BACK"},
    {"Home", "Home", "NAV", "HOME"},
};

constexpr size_t kMqttTemplateCount = sizeof(kMqttTemplates) / sizeof(kMqttTemplates[0]);

std::string trim_copy(const std::string& in) {
  size_t start = 0;
  while (start < in.size() && (in[start] == ' ' || in[start] == '\t')) ++start;
  size_t end = in.size();
  while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\t')) --end;
  return in.substr(start, end - start);
}

std::string lower_copy(const std::string& in) {
  std::string out = in;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return out;
}

bool parse_u64_token(const std::string& token) {
  if (token.empty()) return false;
  errno = 0;
  char* end_ptr = nullptr;
  (void)strtoull(token.c_str(), &end_ptr, 0);
  return errno == 0 && end_ptr != nullptr && *end_ptr == '\0';
}

bool parse_u32_token(const std::string& token) {
  if (token.empty()) return false;
  errno = 0;
  char* end_ptr = nullptr;
  (void)strtoul(token.c_str(), &end_ptr, 0);
  return errno == 0 && end_ptr != nullptr && *end_ptr == '\0';
}

std::string timezone_dropdown_options() {
  std::string options;
  for (size_t i = 0; i < kTimeZoneOptionCount; ++i) {
    if (!options.empty()) options += "\n";
    options += kTimeZoneOptions[i].label;
  }
  return options;
}

uint16_t timezone_index_for_value(const std::string& value) {
  for (uint16_t i = 0; i < kTimeZoneOptionCount; ++i) {
    if (value == kTimeZoneOptions[i].value) return i;
  }
  return 0;
}

const char* timezone_value_for_index(uint16_t index) {
  if (index >= kTimeZoneOptionCount) return kTimeZoneOptions[0].value;
  return kTimeZoneOptions[index].value;
}

const char* timezone_label_for_index(uint16_t index) {
  if (index >= kTimeZoneOptionCount) return kTimeZoneOptions[0].label;
  return kTimeZoneOptions[index].label;
}

std::string sleep_timeout_dropdown_options() {
  std::string options;
  for (size_t i = 0; i < kSleepTimeoutOptionCount; ++i) {
    if (!options.empty()) options += "\n";
    options += kSleepTimeoutOptions[i].label;
  }
  return options;
}

uint16_t sleep_timeout_index_for_value(uint32_t timeout_ms) {
  for (uint16_t i = 0; i < kSleepTimeoutOptionCount; ++i) {
    if (kSleepTimeoutOptions[i].timeout_ms == timeout_ms) return i;
  }
  // Choose nearest by absolute distance if no exact match.
  uint16_t nearest = 0;
  uint32_t best_delta = UINT32_MAX;
  for (uint16_t i = 0; i < kSleepTimeoutOptionCount; ++i) {
    uint32_t option_ms = kSleepTimeoutOptions[i].timeout_ms;
    uint32_t delta = option_ms > timeout_ms ? (option_ms - timeout_ms) : (timeout_ms - option_ms);
    if (delta < best_delta) {
      best_delta = delta;
      nearest = i;
    }
  }
  return nearest;
}

uint32_t sleep_timeout_value_for_index(uint16_t index) {
  if (index >= kSleepTimeoutOptionCount) return kSleepTimeoutOptions[0].timeout_ms;
  return kSleepTimeoutOptions[index].timeout_ms;
}

std::string debounce_dropdown_options() {
  std::string options;
  for (size_t i = 0; i < kDebounceOptionCount; ++i) {
    if (!options.empty()) options += "\n";
    options += kDebounceOptions[i].label;
  }
  return options;
}

uint16_t debounce_index_for_value(unsigned long interval_ms) {
  for (uint16_t i = 0; i < kDebounceOptionCount; ++i) {
    if (kDebounceOptions[i].interval_ms == interval_ms) return i;
  }
  uint16_t nearest = 0;
  unsigned long best_delta = ULONG_MAX;
  for (uint16_t i = 0; i < kDebounceOptionCount; ++i) {
    unsigned long option_ms = kDebounceOptions[i].interval_ms;
    unsigned long delta = option_ms > interval_ms ? (option_ms - interval_ms) : (interval_ms - option_ms);
    if (delta < best_delta) {
      best_delta = delta;
      nearest = i;
    }
  }
  return nearest;
}

unsigned long debounce_value_for_index(uint16_t index) {
  if (index >= kDebounceOptionCount) return kDebounceOptions[2].interval_ms;
  return kDebounceOptions[index].interval_ms;
}

std::string mqtt_template_dropdown_options() {
  std::string options;
  for (size_t i = 0; i < kMqttTemplateCount; ++i) {
    if (!options.empty()) options += "\n";
    options += kMqttTemplates[i].label;
  }
  return options;
}

const MqttTemplateDef* mqtt_template_for_index(uint16_t index) {
  if (index >= kMqttTemplateCount) return nullptr;
  return &kMqttTemplates[index];
}

void apply_timezone_setting(const std::string& timezone_value) {
  const std::string tz = timezone_value.empty() ? std::string(kDefaultTimezoneValue) : timezone_value;
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

std::string load_timezone_setting() {
  Preferences prefs;
  std::string timezone = kDefaultTimezoneValue;
  if (prefs.begin(kUiPrefsNs, true)) {
    String stored = prefs.getString(kUiTimezoneKey, kDefaultTimezoneValue);
    prefs.end();
    timezone = std::string(stored.c_str());
  }
  if (timezone.empty()) timezone = kDefaultTimezoneValue;
  return timezone;
}

bool save_timezone_setting(const std::string& timezone_value) {
  Preferences prefs;
  if (!prefs.begin(kUiPrefsNs, false)) return false;
  prefs.putString(kUiTimezoneKey, timezone_value.c_str());
  prefs.end();
  return true;
}

unsigned long load_debounce_interval_setting(unsigned long fallback_ms) {
  Preferences prefs;
  unsigned long debounce_ms = fallback_ms;
  if (prefs.begin(kUiPrefsNs, true)) {
    debounce_ms = prefs.getULong(kUiDebounceIntervalKey, fallback_ms);
    prefs.end();
  }
  return debounce_ms;
}

bool save_debounce_interval_setting(unsigned long interval_ms) {
  Preferences prefs;
  if (!prefs.begin(kUiPrefsNs, false)) return false;
  prefs.putULong(kUiDebounceIntervalKey, interval_ms);
  prefs.end();
  return true;
}

bool starts_with(const std::string& text, const char* prefix) {
  if (prefix == nullptr) return false;
  return text.rfind(prefix, 0) == 0;
}

bool validate_ir_payload(const std::string& raw_payload, std::string* error_detail) {
  std::string payload = trim_copy(raw_payload);
  if (payload.empty()) return true;

  size_t first_colon = payload.find(':');
  if (first_colon == std::string::npos) {
    if (parse_u64_token(payload)) return true;
    if (error_detail != nullptr) {
      *error_detail = "data must be an integer/hex value";
    }
    return false;
  }

  size_t second_colon = payload.find(':', first_colon + 1);
  if (second_colon == std::string::npos) {
    if (error_detail != nullptr) {
      *error_detail = "use data:bits:repeat";
    }
    return false;
  }
  if (payload.find(':', second_colon + 1) != std::string::npos) {
    if (error_detail != nullptr) {
      *error_detail = "too many ':' separators";
    }
    return false;
  }

  const std::string data = trim_copy(payload.substr(0, first_colon));
  const std::string bits = trim_copy(payload.substr(first_colon + 1, second_colon - first_colon - 1));
  const std::string repeat = trim_copy(payload.substr(second_colon + 1));

  if (!parse_u64_token(data)) {
    if (error_detail != nullptr) {
      *error_detail = "data token is invalid";
    }
    return false;
  }
  if (!parse_u32_token(bits)) {
    if (error_detail != nullptr) {
      *error_detail = "bits token is invalid";
    }
    return false;
  }
  if (!parse_u32_token(repeat)) {
    if (error_detail != nullptr) {
      *error_detail = "repeat token is invalid";
    }
    return false;
  }

  return true;
}

bool validate_mqtt_payload(const std::string& raw_payload, const std::string& default_topic, std::string* error_detail) {
  const std::string payload = trim_copy(raw_payload);
  if (payload.empty()) {
    if (error_detail != nullptr) *error_detail = "payload is empty";
    return false;
  }

  std::string topic;
  const size_t newline_pos = payload.find('\n');
  const size_t pipe_pos = payload.find('|');
  if (newline_pos != std::string::npos) {
    topic = trim_copy(payload.substr(0, newline_pos));
  } else if (pipe_pos != std::string::npos) {
    topic = trim_copy(payload.substr(0, pipe_pos));
  } else {
    topic = trim_copy(default_topic);
  }

  if (topic.empty()) {
    if (error_detail != nullptr) {
      *error_detail = "missing topic (use topic|payload or set device topic)";
    }
    return false;
  }
  return true;
}

bool validate_ble_payload(const std::string& raw_payload, std::string* error_detail) {
  const std::string payload = trim_copy(raw_payload);
  if (payload.empty()) {
    if (error_detail != nullptr) *error_detail = "payload is empty";
    return false;
  }

  std::string body = payload;
  const size_t at_pos = payload.find('@');
  if (at_pos != std::string::npos) {
    const std::string address = trim_copy(payload.substr(0, at_pos));
    if (address.empty()) {
      if (error_detail != nullptr) *error_detail = "address before '@' is empty";
      return false;
    }
    body = trim_copy(payload.substr(at_pos + 1));
  }

  const size_t colon = body.find(':');
  if (colon == std::string::npos) {
    if (error_detail != nullptr) *error_detail = "use key:<name>, media:<name>, or text:<value>";
    return false;
  }
  const std::string kind = lower_copy(trim_copy(body.substr(0, colon)));
  const std::string value = lower_copy(trim_copy(body.substr(colon + 1)));
  if (value.empty()) {
    if (error_detail != nullptr) *error_detail = "payload value is empty";
    return false;
  }
  if (kind == "text") return true;

  if (kind == "key") {
    static const char* kAllowed[] = {"up", "down", "left", "right", "ok", "enter", "back", "home", "menu"};
    for (const char* token : kAllowed) {
      if (value == token) return true;
    }
    if (error_detail != nullptr) *error_detail = "unknown key action";
    return false;
  }
  if (kind == "media") {
    static const char* kAllowed[] = {"back", "home", "prev", "rewind", "rewind_long", "playpause",
                                      "ff", "ff_long", "next", "mute", "volup", "voldown"};
    for (const char* token : kAllowed) {
      if (value == token) return true;
    }
    if (error_detail != nullptr) *error_detail = "unknown media action";
    return false;
  }

  if (error_detail != nullptr) *error_detail = "unknown BLE payload type";
  return false;
}

bool get_valid_local_time(struct tm* out_tm) {
  if (out_tm == nullptr) return false;
  time_t now = time(nullptr);
  if (now < kMinValidClockEpoch) return false;
  localtime_r(&now, out_tm);
  return true;
}

std::string current_time_hhmm_text() {
  struct tm now_tm {};
  if (!get_valid_local_time(&now_tm)) return "--:--";
  char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", &now_tm);
  return std::string(buf);
}

std::string current_time_full_text() {
  struct tm now_tm {};
  if (!get_valid_local_time(&now_tm)) return "2026-01-01 12:00:00";
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &now_tm);
  return std::string(buf);
}

bool parse_manual_datetime(const std::string& input, struct tm* out_tm) {
  if (out_tm == nullptr) return false;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (sscanf(input.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
    return false;
  }
  if (year < 2024 || year > 2099 ||
      month < 1 || month > 12 ||
      day < 1 || day > 31 ||
      hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 ||
      second < 0 || second > 59) {
    return false;
  }

  struct tm parsed {};
  parsed.tm_year = year - 1900;
  parsed.tm_mon = month - 1;
  parsed.tm_mday = day;
  parsed.tm_hour = hour;
  parsed.tm_min = minute;
  parsed.tm_sec = second;
  parsed.tm_isdst = -1;
  *out_tm = parsed;
  return true;
}

void ir_learn_message_cb(std::string message) {
  if (g_active_ui != nullptr) {
    g_active_ui->handle_ir_learned_message(message);
  }
}

}  // namespace

SetupUi::SetupUi(DeviceRegistry& device_registry, ActivityRegistry& activity_registry, CommandDispatcher& dispatcher)
    : device_registry_(device_registry), activity_registry_(activity_registry), dispatcher_(dispatcher) {}

void SetupUi::init() {
  std::vector<DeviceRecord> normalized_devices = device_registry_.all();
  std::vector<ActivityRecord> normalized_activities = activity_registry_.all();
  if (normalize_restored_records(&normalized_devices, &normalized_activities)) {
    (void)device_registry_.replace_all(std::move(normalized_devices));
    (void)activity_registry_.replace_all(std::move(normalized_activities));
  }
  const unsigned long saved_debounce_ms = load_debounce_interval_setting(dispatcher_.debounce_interval_ms());
  dispatcher_.set_debounce_interval_ms(saved_debounce_ms);

  root_ = lv_scr_act();
  lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(root_, lv_color_hex(0x101010), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);

  status_bar_ = lv_obj_create(root_);
  lv_obj_set_size(status_bar_, SCR_WIDTH, 18);
  lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_radius(status_bar_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(status_bar_, 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(status_bar_, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
  lv_obj_clear_flag(status_bar_, LV_OBJ_FLAG_SCROLLABLE);

  wifi_status_label_ = lv_label_create(status_bar_);
  lv_obj_set_style_text_font(wifi_status_label_, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_label_set_text(wifi_status_label_, "WiFi");
  lv_obj_align(wifi_status_label_, LV_ALIGN_LEFT_MID, 2, 0);

  time_status_label_ = lv_label_create(status_bar_);
  lv_obj_set_style_text_font(time_status_label_, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_label_set_text(time_status_label_, "--:--");
  lv_obj_align(time_status_label_, LV_ALIGN_CENTER, 0, 0);

  battery_status_label_ = lv_label_create(status_bar_);
  lv_obj_set_style_text_font(battery_status_label_, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_label_set_text(battery_status_label_, "Batt: --");
  lv_obj_align(battery_status_label_, LV_ALIGN_RIGHT_MID, -2, 0);

  tabview_ = lv_tileview_create(root_);
  lv_obj_set_size(tabview_, SCR_WIDTH, SCR_HEIGHT - 18);
  lv_obj_align(tabview_, LV_ALIGN_TOP_MID, 0, 18);
  lv_obj_clear_flag(tabview_, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_set_scrollbar_mode(tabview_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(tabview_, on_tabview_changed, LV_EVENT_VALUE_CHANGED, this);

  // Linear order: Devices -> Activities -> Remote -> Settings
  devices_page_ = lv_tileview_add_tile(tabview_, 0, 0, LV_DIR_RIGHT);
  activities_page_ = lv_tileview_add_tile(tabview_, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
  remote_page_ = lv_tileview_add_tile(tabview_, 2, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
  settings_page_ = lv_tileview_add_tile(tabview_, 3, 0, LV_DIR_LEFT);

  build_devices_tab();
  build_activities_tab();
  build_remote_tab();
  build_settings_tab();

  rebuild_device_list();
  rebuild_activity_list();
  rebuild_remote_activity_dropdown();
  refresh_remote_icon_overrides();
  rebuild_remote_command_buttons();
  restore_ui_context();
  update_status_bar();
  if (lv_tileview_get_tile_act(tabview_) == nullptr) {
    lv_obj_set_tile(tabview_, activities_page_, LV_ANIM_OFF);
  }
}

void SetupUi::tick() {
  if (millis() - last_status_update_ms_ >= 1000) {
    last_status_update_ms_ = millis();
    update_status_bar();
    refresh_ble_modal_status();
  }
  if (ir_learning_active_) {
    infraredReceiver_loop_HAL();
  }
  lv_timer_handler();
}

void SetupUi::notify_power_event(const std::string& message) {
  last_power_event_ = message;
  if (settings_status_ != nullptr) {
    lv_label_set_text(settings_status_, message.c_str());
  }
}

void SetupUi::save_ui_context() {
  Preferences prefs;
  if (!prefs.begin(kUiPrefsNs, false)) return;

  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  uint32_t selected_activity_id = 0;
  if (selected_activity_index_ >= 0 && selected_activity_index_ < static_cast<int>(activities.size())) {
    selected_activity_id = activities[selected_activity_index_].id;
  }
  prefs.putUInt(kUiSelectedActivityIdKey, selected_activity_id);

  uint8_t selected_tab = 1;
  if (tabview_ != nullptr) {
    lv_obj_t* active_tile = lv_tileview_get_tile_act(tabview_);
    if (active_tile == devices_page_) {
      selected_tab = 0;
    } else if (active_tile == activities_page_) {
      selected_tab = 1;
    } else if (active_tile == remote_page_) {
      selected_tab = 2;
    } else if (active_tile == settings_page_) {
      selected_tab = 3;
    }
  }
  const uint32_t saved_activity_id = prefs.getUInt(kUiSelectedActivityIdKey, UINT32_MAX);
  const uint8_t saved_tab = prefs.getUChar(kUiSelectedTabKey, 0xFF);
  if (saved_activity_id != selected_activity_id) {
    prefs.putUInt(kUiSelectedActivityIdKey, selected_activity_id);
  }
  if (saved_tab != selected_tab) {
    prefs.putUChar(kUiSelectedTabKey, selected_tab);
  }
  prefs.end();
}

void SetupUi::restore_ui_context() {
  uint32_t selected_activity_id = 0;
  uint8_t selected_tab = 1;
  Preferences prefs;
  if (prefs.begin(kUiPrefsNs, true)) {
    selected_activity_id = prefs.getUInt(kUiSelectedActivityIdKey, 0);
    selected_tab = prefs.getUChar(kUiSelectedTabKey, 1);
    prefs.end();
  }

  if (selected_activity_id != 0) {
    const std::vector<ActivityRecord>& activities = activity_registry_.all();
    for (size_t i = 0; i < activities.size(); ++i) {
      if (activities[i].id == selected_activity_id) {
        selected_activity_index_ = static_cast<int>(i);
        break;
      }
    }
  }
  rebuild_activity_list();
  rebuild_remote_activity_dropdown();

  if (tabview_ != nullptr) {
    lv_obj_t* target_tile = activities_page_;
    if (selected_tab == 0 && devices_page_ != nullptr) target_tile = devices_page_;
    if (selected_tab == 1 && activities_page_ != nullptr) target_tile = activities_page_;
    if (selected_tab == 2 && remote_page_ != nullptr) target_tile = remote_page_;
    if (selected_tab == 3 && settings_page_ != nullptr) target_tile = settings_page_;
    lv_obj_set_tile(tabview_, target_tile, LV_ANIM_OFF);
  }
}

void SetupUi::update_status_bar() {
  bool wifi_connected = false;
  bool mqtt_configured = false;
  bool mqtt_connected = false;
#if (ENABLE_WIFI_AND_MQTT == 1)
  wifi_connected = getIsWifiConnected_HAL();
  mqtt_configured = mqtt_is_configured_HAL();
  mqtt_connected = mqtt_is_connected_HAL();
#endif
  if (wifi_connected && mqtt_configured) {
    lv_label_set_text(wifi_status_label_, mqtt_connected ? "WiFi+M" : "WiFi~M");
    lv_obj_set_style_text_color(wifi_status_label_,
                                mqtt_connected ? lv_color_hex(0x3EDC81) : lv_color_hex(0xE6B450),
                                LV_PART_MAIN);
  } else {
    lv_label_set_text(wifi_status_label_, "WiFi");
    lv_obj_set_style_text_color(wifi_status_label_,
                                wifi_connected ? lv_color_hex(0x3EDC81) : lv_color_hex(0xE2574C),
                                LV_PART_MAIN);
  }
  if (time_status_label_ != nullptr) {
    const std::string time_text = current_time_hhmm_text();
    lv_label_set_text(time_status_label_, time_text.c_str());
  }

  if (!last_wifi_connected_known_) {
    last_wifi_connected_ = wifi_connected;
    last_wifi_connected_known_ = true;
  } else {
    if (wifi_connect_attempt_active_) {
      if (wifi_connected) {
        wifi_connect_attempt_active_ = false;
        wifi_request_time_sync_HAL();
        if (settings_status_ != nullptr) {
          if (wifi_connect_target_ssid_.empty()) {
            lv_label_set_text(settings_status_, "WiFi connected, syncing time...");
          } else {
            lv_label_set_text_fmt(settings_status_, "WiFi connected: %s (syncing time...)", wifi_connect_target_ssid_.c_str());
          }
        }
      } else if (millis() - wifi_connect_attempt_started_ms_ > 20000) {
        wifi_connect_attempt_active_ = false;
        if (settings_status_ != nullptr) lv_label_set_text(settings_status_, "WiFi connect timeout");
      }
    } else if (last_wifi_connected_ && !wifi_connected) {
      if (settings_status_ != nullptr) {
        std::string current = lv_label_get_text(settings_status_);
        if (current.empty() || starts_with(current, "WiFi")) {
          lv_label_set_text(settings_status_, "WiFi disconnected");
        }
      }
    }
    last_wifi_connected_ = wifi_connected;
  }

  int battery_mv = 0;
  int battery_pct = 0;
  bool battery_charging = false;
  get_battery_status_HAL(&battery_mv, &battery_pct, &battery_charging);
  if (battery_mv <= 0) {
    lv_label_set_text(battery_status_label_, "Batt: --");
    lv_obj_set_style_text_color(battery_status_label_, lv_color_hex(0xBBBBBB), LV_PART_MAIN);
    return;
  }
  if (battery_charging) {
    lv_label_set_text_fmt(battery_status_label_, "Batt: %d%%+", battery_pct);
    lv_obj_set_style_text_color(battery_status_label_, lv_color_hex(0x3EDC81), LV_PART_MAIN);
  } else if (battery_is_charge_control_available_HAL() && battery_pct >= 98) {
    lv_label_set_text_fmt(battery_status_label_, "Batt: %d%%=", battery_pct);
    lv_obj_set_style_text_color(battery_status_label_, lv_color_hex(0xE6B450), LV_PART_MAIN);
  } else {
    lv_label_set_text_fmt(battery_status_label_, "Batt: %d%%", battery_pct);
    lv_obj_set_style_text_color(battery_status_label_, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  }
}

void SetupUi::build_devices_tab() {
  lv_obj_t* tab = devices_page_;
  if (tab == nullptr) return;
  lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(tab, 6, LV_PART_MAIN);
  const int tab_w = SCR_WIDTH - 12;
  const int footer_h = 34;
  const int reorder_h = 24;
  const int reorder_gap = 4;
  const int list_top = 38;
  const int list_h = SCR_HEIGHT - 44 - list_top - footer_h - reorder_h - reorder_gap - 16;

  lv_obj_t* title = lv_label_create(tab);
  lv_label_set_text(title, "Devices");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  device_move_up_btn_ = lv_btn_create(tab);
  lv_obj_set_size(device_move_up_btn_, (tab_w - 8) / 3, reorder_h);
  lv_obj_align(device_move_up_btn_, LV_ALIGN_BOTTOM_LEFT, 0, -(footer_h + reorder_gap));
  lv_obj_add_event_cb(device_move_up_btn_, on_device_move_up_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* up_label = lv_label_create(device_move_up_btn_);
  lv_label_set_text(up_label, "Up");
  lv_obj_set_style_text_font(up_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(up_label);

  device_move_down_btn_ = lv_btn_create(tab);
  lv_obj_set_size(device_move_down_btn_, (tab_w - 8) / 3, reorder_h);
  lv_obj_align(device_move_down_btn_, LV_ALIGN_BOTTOM_MID, 0, -(footer_h + reorder_gap));
  lv_obj_add_event_cb(device_move_down_btn_, on_device_move_down_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* down_label = lv_label_create(device_move_down_btn_);
  lv_label_set_text(down_label, "Down");
  lv_obj_set_style_text_font(down_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(down_label);

  device_duplicate_btn_ = lv_btn_create(tab);
  lv_obj_set_size(device_duplicate_btn_, (tab_w - 8) / 3, reorder_h);
  lv_obj_align(device_duplicate_btn_, LV_ALIGN_BOTTOM_RIGHT, 0, -(footer_h + reorder_gap));
  lv_obj_add_event_cb(device_duplicate_btn_, on_device_duplicate_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* dup_label = lv_label_create(device_duplicate_btn_);
  lv_label_set_text(dup_label, "Dup");
  lv_obj_set_style_text_font(dup_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(dup_label);

  selected_device_label_ = lv_label_create(tab);
  lv_label_set_text(selected_device_label_, "Selected: none");
  lv_obj_align(selected_device_label_, LV_ALIGN_TOP_LEFT, 0, 16);

  device_list_ = lv_list_create(tab);
  lv_obj_set_size(device_list_, tab_w, list_h);
  lv_obj_align(device_list_, LV_ALIGN_TOP_LEFT, 0, list_top);

  const int footer_btn_w = (tab_w - 12) / 4;
  device_add_btn_ = lv_btn_create(tab);
  lv_obj_set_size(device_add_btn_, footer_btn_w, footer_h);
  lv_obj_align(device_add_btn_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_add_event_cb(device_add_btn_, on_device_add_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* add_label = lv_label_create(device_add_btn_);
  lv_label_set_text(add_label, "Add");
  lv_obj_set_style_text_font(add_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(add_label);

  device_remove_btn_ = lv_btn_create(tab);
  lv_obj_set_size(device_remove_btn_, footer_btn_w, footer_h);
  lv_obj_align(device_remove_btn_, LV_ALIGN_BOTTOM_LEFT, footer_btn_w + 4, 0);
  lv_obj_add_event_cb(device_remove_btn_, on_device_remove_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* remove_label = lv_label_create(device_remove_btn_);
  lv_label_set_text(remove_label, "Remove");
  lv_obj_set_style_text_font(remove_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(remove_label);

  device_rename_btn_ = lv_btn_create(tab);
  lv_obj_set_size(device_rename_btn_, footer_btn_w, footer_h);
  lv_obj_align(device_rename_btn_, LV_ALIGN_BOTTOM_LEFT, (2 * footer_btn_w) + 8, 0);
  lv_obj_add_event_cb(device_rename_btn_, on_device_rename_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* rename_label = lv_label_create(device_rename_btn_);
  lv_label_set_text(rename_label, "Rename");
  lv_obj_set_style_text_font(rename_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(rename_label);

  device_command_btn_ = lv_btn_create(tab);
  lv_obj_set_size(device_command_btn_, footer_btn_w, footer_h);
  lv_obj_align(device_command_btn_, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_event_cb(device_command_btn_, on_device_command_edit, LV_EVENT_CLICKED, this);
  lv_obj_t* cmd_label = lv_label_create(device_command_btn_);
  lv_label_set_text(cmd_label, "Edit");
  lv_obj_set_style_text_font(cmd_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(cmd_label);
}

void SetupUi::build_activities_tab() {
  lv_obj_t* tab = activities_page_;
  if (tab == nullptr) return;
  lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(tab, 6, LV_PART_MAIN);
  const int tab_w = SCR_WIDTH - 12;
  const int footer_h = 34;
  const int reorder_h = 24;
  const int reorder_gap = 4;
  const int list_top = 44;
  const int list_h = SCR_HEIGHT - 44 - list_top - footer_h - reorder_h - reorder_gap - 16;

  lv_obj_t* title = lv_label_create(tab);
  lv_label_set_text(title, "Activities");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  activity_move_up_btn_ = lv_btn_create(tab);
  lv_obj_set_size(activity_move_up_btn_, (tab_w - 8) / 3, reorder_h);
  lv_obj_align(activity_move_up_btn_, LV_ALIGN_BOTTOM_LEFT, 0, -(footer_h + reorder_gap));
  lv_obj_add_event_cb(activity_move_up_btn_, on_activity_move_up_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* aup_label = lv_label_create(activity_move_up_btn_);
  lv_label_set_text(aup_label, "Up");
  lv_obj_set_style_text_font(aup_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(aup_label);

  activity_move_down_btn_ = lv_btn_create(tab);
  lv_obj_set_size(activity_move_down_btn_, (tab_w - 8) / 3, reorder_h);
  lv_obj_align(activity_move_down_btn_, LV_ALIGN_BOTTOM_MID, 0, -(footer_h + reorder_gap));
  lv_obj_add_event_cb(activity_move_down_btn_, on_activity_move_down_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* adown_label = lv_label_create(activity_move_down_btn_);
  lv_label_set_text(adown_label, "Down");
  lv_obj_set_style_text_font(adown_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(adown_label);

  activity_duplicate_btn_ = lv_btn_create(tab);
  lv_obj_set_size(activity_duplicate_btn_, (tab_w - 8) / 3, reorder_h);
  lv_obj_align(activity_duplicate_btn_, LV_ALIGN_BOTTOM_RIGHT, 0, -(footer_h + reorder_gap));
  lv_obj_add_event_cb(activity_duplicate_btn_, on_activity_duplicate_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* adup_label = lv_label_create(activity_duplicate_btn_);
  lv_label_set_text(adup_label, "Dup");
  lv_obj_set_style_text_font(adup_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(adup_label);

  activity_builder_btn_ = lv_btn_create(tab);
  lv_obj_set_size(activity_builder_btn_, 64, 24);
  lv_obj_align(activity_builder_btn_, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_add_event_cb(activity_builder_btn_, on_activity_builder_open, LV_EVENT_CLICKED, this);
  lv_obj_t* builder_label = lv_label_create(activity_builder_btn_);
  lv_label_set_text(builder_label, "Builder");
  lv_obj_set_style_text_font(builder_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(builder_label);

  selected_activity_label_ = lv_label_create(tab);
  lv_label_set_text(selected_activity_label_, "Selected: none");
  lv_obj_align(selected_activity_label_, LV_ALIGN_TOP_LEFT, 0, 16);

  activity_list_ = lv_list_create(tab);
  lv_obj_set_size(activity_list_, tab_w, list_h);
  lv_obj_align(activity_list_, LV_ALIGN_TOP_LEFT, 0, list_top);

  const int footer_btn_w = (tab_w - 12) / 4;
  activity_add_btn_ = lv_btn_create(tab);
  lv_obj_set_size(activity_add_btn_, footer_btn_w, footer_h);
  lv_obj_align(activity_add_btn_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_add_event_cb(activity_add_btn_, on_activity_add_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* add_label = lv_label_create(activity_add_btn_);
  lv_label_set_text(add_label, "Add");
  lv_obj_set_style_text_font(add_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(add_label);

  activity_remove_btn_ = lv_btn_create(tab);
  lv_obj_set_size(activity_remove_btn_, footer_btn_w, footer_h);
  lv_obj_align(activity_remove_btn_, LV_ALIGN_BOTTOM_LEFT, footer_btn_w + 4, 0);
  lv_obj_add_event_cb(activity_remove_btn_, on_activity_remove_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* remove_label = lv_label_create(activity_remove_btn_);
  lv_label_set_text(remove_label, "Remove");
  lv_obj_set_style_text_font(remove_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(remove_label);

  activity_rename_btn_ = lv_btn_create(tab);
  lv_obj_set_size(activity_rename_btn_, footer_btn_w, footer_h);
  lv_obj_align(activity_rename_btn_, LV_ALIGN_BOTTOM_LEFT, (2 * footer_btn_w) + 8, 0);
  lv_obj_add_event_cb(activity_rename_btn_, on_activity_rename_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* rename_label = lv_label_create(activity_rename_btn_);
  lv_label_set_text(rename_label, "Rename");
  lv_obj_set_style_text_font(rename_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(rename_label);

  activity_keymap_btn_ = lv_btn_create(tab);
  lv_obj_set_size(activity_keymap_btn_, footer_btn_w, footer_h);
  lv_obj_align(activity_keymap_btn_, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_event_cb(activity_keymap_btn_, on_activity_keymap_open, LV_EVENT_CLICKED, this);
  lv_obj_t* keymap_label = lv_label_create(activity_keymap_btn_);
  lv_label_set_text(keymap_label, "Edit");
  lv_obj_set_style_text_font(keymap_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(keymap_label);
}

void SetupUi::build_remote_tab() {
  lv_obj_t* tab = remote_page_;
  if (tab == nullptr) return;
  lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(tab, LV_DIR_VER);
  lv_obj_set_style_pad_all(tab, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(tab, 10, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(tab);
  lv_label_set_text(title, "Remote");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t* dd_label = lv_label_create(tab);
  lv_label_set_text(dd_label, "Device");
  lv_obj_align(dd_label, LV_ALIGN_TOP_LEFT, 0, 14);

  remote_activity_dd_ = lv_dropdown_create(tab);
  lv_obj_set_width(remote_activity_dd_, SCR_WIDTH - 12);
  lv_obj_align(remote_activity_dd_, LV_ALIGN_TOP_LEFT, 0, 30);
  lv_obj_add_event_cb(remote_activity_dd_, on_remote_activity_changed, LV_EVENT_VALUE_CHANGED, this);

  remote_status_ = lv_label_create(tab);
  lv_label_set_text(remote_status_, "Select a device");
  lv_obj_align(remote_status_, LV_ALIGN_TOP_LEFT, 0, 62);

  remote_page_prev_btn_ = lv_btn_create(tab);
  lv_obj_set_size(remote_page_prev_btn_, 40, 20);
  lv_obj_align(remote_page_prev_btn_, LV_ALIGN_TOP_LEFT, 0, 84);
  lv_obj_add_event_cb(remote_page_prev_btn_, on_remote_page_prev, LV_EVENT_CLICKED, this);
  lv_obj_t* prev_text = lv_label_create(remote_page_prev_btn_);
  lv_label_set_text(prev_text, "<");
  lv_obj_center(prev_text);

  remote_page_next_btn_ = lv_btn_create(tab);
  lv_obj_set_size(remote_page_next_btn_, 40, 20);
  lv_obj_align(remote_page_next_btn_, LV_ALIGN_TOP_RIGHT, 0, 84);
  lv_obj_add_event_cb(remote_page_next_btn_, on_remote_page_next, LV_EVENT_CLICKED, this);
  lv_obj_t* next_text = lv_label_create(remote_page_next_btn_);
  lv_label_set_text(next_text, ">");
  lv_obj_center(next_text);

  remote_page_label_ = lv_label_create(tab);
  lv_label_set_text(remote_page_label_, "Page 1/1");
  lv_obj_align(remote_page_label_, LV_ALIGN_TOP_MID, 0, 88);
}

void SetupUi::build_settings_tab() {
  lv_obj_t* tab = settings_page_;
  if (tab == nullptr) return;
  lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(tab, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(tab, 28, LV_PART_MAIN);
  const int btn_h = 34;

  lv_obj_t* title = lv_label_create(tab);
  lv_label_set_text(title, "Settings");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  settings_backup_btn_ = lv_btn_create(tab);
  lv_obj_set_size(settings_backup_btn_, SCR_WIDTH - 16, btn_h);
  lv_obj_align(settings_backup_btn_, LV_ALIGN_TOP_LEFT, 0, 46);
  lv_obj_add_flag(settings_backup_btn_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_event_cb(settings_backup_btn_, on_settings_backup_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* backup_label = lv_label_create(settings_backup_btn_);
  lv_label_set_text(backup_label, "Backup to SD");
  lv_obj_center(backup_label);

  settings_restore_btn_ = lv_btn_create(tab);
  lv_obj_set_size(settings_restore_btn_, SCR_WIDTH - 16, btn_h);
  lv_obj_align(settings_restore_btn_, LV_ALIGN_TOP_LEFT, 0, 86);
  lv_obj_add_flag(settings_restore_btn_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_event_cb(settings_restore_btn_, on_settings_restore_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* restore_label = lv_label_create(settings_restore_btn_);
  lv_label_set_text(restore_label, "Restore from SD...");
  lv_obj_center(restore_label);

  settings_wifi_btn_ = lv_btn_create(tab);
  lv_obj_set_size(settings_wifi_btn_, SCR_WIDTH - 16, btn_h);
  lv_obj_align(settings_wifi_btn_, LV_ALIGN_TOP_LEFT, 0, 126);
  lv_obj_add_flag(settings_wifi_btn_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_event_cb(settings_wifi_btn_, on_settings_wifi_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* wifi_label = lv_label_create(settings_wifi_btn_);
  lv_label_set_text(wifi_label, "WiFi Settings");
  lv_obj_center(wifi_label);

  settings_ble_btn_ = lv_btn_create(tab);
  lv_obj_set_size(settings_ble_btn_, SCR_WIDTH - 16, btn_h);
  lv_obj_align(settings_ble_btn_, LV_ALIGN_TOP_LEFT, 0, 166);
  lv_obj_add_flag(settings_ble_btn_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_event_cb(settings_ble_btn_, on_settings_ble_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* ble_label = lv_label_create(settings_ble_btn_);
  lv_label_set_text(ble_label, "BLE Settings");
  lv_obj_center(ble_label);

  settings_mqtt_btn_ = lv_btn_create(tab);
  lv_obj_set_size(settings_mqtt_btn_, SCR_WIDTH - 16, btn_h);
  lv_obj_align(settings_mqtt_btn_, LV_ALIGN_TOP_LEFT, 0, 206);
  lv_obj_add_flag(settings_mqtt_btn_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_event_cb(settings_mqtt_btn_, on_settings_mqtt_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* mqtt_label = lv_label_create(settings_mqtt_btn_);
  lv_label_set_text(mqtt_label, "MQTT Settings");
  lv_obj_center(mqtt_label);

  settings_icons_btn_ = lv_btn_create(tab);
  lv_obj_set_size(settings_icons_btn_, SCR_WIDTH - 16, btn_h);
  lv_obj_align(settings_icons_btn_, LV_ALIGN_TOP_LEFT, 0, 246);
  lv_obj_add_flag(settings_icons_btn_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_event_cb(settings_icons_btn_, on_settings_icons_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* icons_label = lv_label_create(settings_icons_btn_);
  lv_label_set_text(icons_label, "Reload Icon Pack");
  lv_obj_center(icons_label);

  settings_set_time_btn_ = lv_btn_create(tab);
  lv_obj_set_size(settings_set_time_btn_, SCR_WIDTH - 16, btn_h);
  lv_obj_align(settings_set_time_btn_, LV_ALIGN_TOP_LEFT, 0, 286);
  lv_obj_add_flag(settings_set_time_btn_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_event_cb(settings_set_time_btn_, on_settings_set_time_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* set_time_label = lv_label_create(settings_set_time_btn_);
  lv_label_set_text(set_time_label, "Set Time");
  lv_obj_center(set_time_label);

  settings_power_btn_ = lv_btn_create(tab);
  lv_obj_set_size(settings_power_btn_, SCR_WIDTH - 16, btn_h);
  lv_obj_align(settings_power_btn_, LV_ALIGN_TOP_LEFT, 0, 326);
  lv_obj_add_flag(settings_power_btn_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_event_cb(settings_power_btn_, on_settings_power_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* power_label = lv_label_create(settings_power_btn_);
  lv_label_set_text(power_label, "Power Settings");
  lv_obj_center(power_label);

  settings_status_ = lv_label_create(tab);
  lv_label_set_text(settings_status_, "Status: idle");
  lv_obj_set_width(settings_status_, SCR_WIDTH - 16);
  lv_label_set_long_mode(settings_status_, LV_LABEL_LONG_WRAP);
  lv_obj_align(settings_status_, LV_ALIGN_TOP_LEFT, 0, 366);
}

void SetupUi::rebuild_device_list() {
  lv_obj_clean(device_list_);
  device_row_buttons_.clear();

  const std::vector<DeviceRecord>& devices = device_registry_.all();
  for (size_t i = 0; i < devices.size(); ++i) {
    const DeviceRecord& d = devices[i];
    char row[160];
    snprintf(row, sizeof(row), "%s [%s/%s]", d.name.c_str(), to_string(d.type), to_string(d.transport));
    lv_obj_t* btn = lv_list_add_btn(device_list_, nullptr, row);
    device_row_buttons_.push_back(btn);
    lv_obj_add_event_cb(btn, on_device_clicked, LV_EVENT_CLICKED, this);
  }

  if (selected_device_index_ >= static_cast<int>(devices.size())) selected_device_index_ = -1;
  if (selected_device_index_ >= 0) {
    lv_label_set_text_fmt(selected_device_label_, "Selected: %s", devices[selected_device_index_].name.c_str());
  } else {
    lv_label_set_text(selected_device_label_, "Selected: none");
  }
}

void SetupUi::rebuild_activity_list() {
  lv_obj_clean(activity_list_);
  activity_row_buttons_.clear();

  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  for (size_t i = 0; i < activities.size(); ++i) {
    const ActivityRecord& a = activities[i];
    lv_obj_t* btn = lv_list_add_btn(activity_list_, nullptr, a.name.c_str());
    activity_row_buttons_.push_back(btn);
    lv_obj_add_event_cb(btn, on_activity_clicked, LV_EVENT_CLICKED, this);
  }

  if (selected_activity_index_ >= static_cast<int>(activities.size())) selected_activity_index_ = -1;
  if (selected_activity_index_ >= 0) {
    lv_label_set_text_fmt(selected_activity_label_, "Selected: %s", activities[selected_activity_index_].name.c_str());
  } else {
    lv_label_set_text(selected_activity_label_, "Selected: none");
  }
}

void SetupUi::rebuild_remote_activity_dropdown() {
  if (remote_activity_dd_ == nullptr) return;
  std::string options;
  const std::vector<DeviceRecord>& devices = device_registry_.all();
  remote_device_ids_.clear();
  for (size_t i = 0; i < devices.size(); ++i) {
    if (i > 0) options.append("\n");
    options.append(devices[i].name);
    remote_device_ids_.push_back(devices[i].id);
  }

  if (options.empty()) {
    options = "No device";
  }
  lv_dropdown_set_options(remote_activity_dd_, options.c_str());

  int selected = selected_device_index_;
  if (selected < 0 || selected >= static_cast<int>(devices.size())) {
    selected = devices.empty() ? -1 : 0;
  }
  selected_device_index_ = selected;
  if (selected >= 0) {
    lv_dropdown_set_selected(remote_activity_dd_, static_cast<uint16_t>(selected));
  }
  remote_command_page_index_ = 0;
  rebuild_remote_command_buttons();
}

void SetupUi::refresh_remote_icon_overrides() {
  remote_icon_overrides_.clear();
  std::string icon_status;
  if (!sd_backup_.load_icon_pack(&remote_icon_overrides_, &icon_status)) {
    return;
  }
  if (settings_status_ != nullptr && !icon_status.empty()) {
    lv_label_set_text(settings_status_, icon_status.c_str());
  }
}

void SetupUi::rebuild_remote_command_buttons() {
  if (remote_page_ == nullptr) return;
  for (lv_obj_t* button : remote_command_buttons_) {
    if (button != nullptr) lv_obj_del(button);
  }
  remote_command_buttons_.clear();
  remote_command_names_.clear();

  if (remote_activity_dd_ == nullptr || remote_status_ == nullptr) return;
  if (remote_device_ids_.empty()) {
    lv_label_set_text(remote_status_, "No devices available");
    if (remote_page_label_ != nullptr) lv_label_set_text(remote_page_label_, "Page 0/0");
    if (remote_page_prev_btn_ != nullptr) lv_obj_add_flag(remote_page_prev_btn_, LV_OBJ_FLAG_HIDDEN);
    if (remote_page_next_btn_ != nullptr) lv_obj_add_flag(remote_page_next_btn_, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  const uint16_t selected = lv_dropdown_get_selected(remote_activity_dd_);
  if (selected >= remote_device_ids_.size()) {
    lv_label_set_text(remote_status_, "Select a device");
    if (remote_page_label_ != nullptr) lv_label_set_text(remote_page_label_, "Page 0/0");
    if (remote_page_prev_btn_ != nullptr) lv_obj_add_flag(remote_page_prev_btn_, LV_OBJ_FLAG_HIDDEN);
    if (remote_page_next_btn_ != nullptr) lv_obj_add_flag(remote_page_next_btn_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  const DeviceRecord* device = device_registry_.get_by_id(remote_device_ids_[selected]);
  if (device == nullptr) {
    lv_label_set_text(remote_status_, "Selected device unavailable");
    if (remote_page_label_ != nullptr) lv_label_set_text(remote_page_label_, "Page 0/0");
    if (remote_page_prev_btn_ != nullptr) lv_obj_add_flag(remote_page_prev_btn_, LV_OBJ_FLAG_HIDDEN);
    if (remote_page_next_btn_ != nullptr) lv_obj_add_flag(remote_page_next_btn_, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  std::vector<std::string> command_names;
  command_names.reserve(device->commands.size());
  for (const DeviceCommand& command : device->commands) {
    if (trim_copy(command.name).empty() || trim_copy(command.payload).empty()) continue;
    command_names.push_back(command.name);
  }

  std::unordered_map<std::string, int> command_rank;
  const std::vector<std::string>& common = common_command_names();
  for (size_t i = 0; i < common.size(); ++i) {
    command_rank[lower_copy(common[i])] = static_cast<int>(i);
  }
  std::sort(command_names.begin(), command_names.end(),
            [&](const std::string& a, const std::string& b) {
              const std::string la = lower_copy(a);
              const std::string lb = lower_copy(b);
              const auto ra_it = command_rank.find(la);
              const auto rb_it = command_rank.find(lb);
              const bool a_ranked = (ra_it != command_rank.end());
              const bool b_ranked = (rb_it != command_rank.end());
              if (a_ranked && b_ranked && ra_it->second != rb_it->second) return ra_it->second < rb_it->second;
              if (a_ranked != b_ranked) return a_ranked;
              return la < lb;
            });

  constexpr int kPageSize = 12;
  const int command_total = static_cast<int>(command_names.size());
  const int page_count = std::max(1, (command_total + kPageSize - 1) / kPageSize);
  if (remote_command_page_index_ < 0) remote_command_page_index_ = 0;
  if (remote_command_page_index_ >= page_count) remote_command_page_index_ = page_count - 1;

  if (remote_page_label_ != nullptr) {
    lv_label_set_text_fmt(remote_page_label_, "Page %d/%d", remote_command_page_index_ + 1, page_count);
  }
  if (remote_page_prev_btn_ != nullptr) {
    if (page_count > 1) {
      lv_obj_clear_flag(remote_page_prev_btn_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(remote_page_prev_btn_, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (remote_page_next_btn_ != nullptr) {
    if (page_count > 1) {
      lv_obj_clear_flag(remote_page_next_btn_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(remote_page_next_btn_, LV_OBJ_FLAG_HIDDEN);
    }
  }

  const int pad = 4;
  const int btn_w = (SCR_WIDTH - 12 - (2 * pad)) / 3;
  const int btn_h = 30;
  const int y_start = 108;
  int button_count = 0;
  const int start_index = remote_command_page_index_ * kPageSize;
  const int end_index = std::min(command_total, start_index + kPageSize);
  for (int i = start_index; i < end_index; ++i) {
    const std::string& command_name = command_names[static_cast<size_t>(i)];
    const int col = button_count % 3;
    const int row = button_count / 3;
    const int x = col * (btn_w + pad);
    const int y = y_start + row * (btn_h + pad);

    lv_obj_t* btn = lv_btn_create(remote_page_);
    lv_obj_set_size(btn, btn_w, btn_h);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_t* label = lv_label_create(btn);
    const std::string command_key = lower_copy(trim_copy(command_name));
    auto icon_it = remote_icon_overrides_.find(command_key);
    if (icon_it != remote_icon_overrides_.end() && !icon_it->second.empty()) {
      lv_label_set_text(label, icon_it->second.c_str());
    } else {
      lv_label_set_text(label, command_name.c_str());
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, btn_w - 6);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, on_remote_command_clicked, LV_EVENT_CLICKED, this);

    remote_command_buttons_.push_back(btn);
    remote_command_names_.push_back(command_name);
    ++button_count;
  }

  if (button_count == 0) {
    lv_label_set_text_fmt(remote_status_, "No mapped commands for %s", device->name.c_str());
  } else {
    lv_label_set_text_fmt(remote_status_, "Device: %s (%d commands)", device->name.c_str(), command_total);
  }
}

void SetupUi::open_device_modal() {
  if (device_modal_ != nullptr) return;

  device_modal_ = lv_obj_create(root_);
  lv_obj_set_size(device_modal_, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_center(device_modal_);
  lv_obj_set_style_bg_color(device_modal_, lv_color_hex(0x202020), LV_PART_MAIN);
  lv_obj_set_style_pad_all(device_modal_, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(device_modal_, LV_DIR_VER);
  lv_obj_set_style_pad_bottom(device_modal_, 148, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(device_modal_);
  lv_label_set_text(title, "Add Device");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  dd_type_ = lv_dropdown_create(device_modal_);
  lv_dropdown_set_options(dd_type_, kDeviceTypeOptions);
  lv_obj_set_width(dd_type_, SCR_WIDTH - 12);
  lv_obj_align(dd_type_, LV_ALIGN_TOP_LEFT, 0, 24);

  dd_transport_ = lv_dropdown_create(device_modal_);
  lv_dropdown_set_options(dd_transport_, kTransportOptions);
  lv_obj_set_width(dd_transport_, SCR_WIDTH - 12);
  lv_obj_align(dd_transport_, LV_ALIGN_TOP_LEFT, 0, 56);

  ta_device_name_ = lv_textarea_create(device_modal_);
  lv_textarea_set_one_line(ta_device_name_, true);
  lv_textarea_set_placeholder_text(ta_device_name_, "Device Name");
  lv_obj_set_width(ta_device_name_, SCR_WIDTH - 12);
  lv_obj_align(ta_device_name_, LV_ALIGN_TOP_LEFT, 0, 88);
  lv_obj_add_event_cb(ta_device_name_, on_textarea_focus, LV_EVENT_FOCUSED, this);

  lv_obj_t* cancel_btn = lv_btn_create(device_modal_);
  lv_obj_set_size(cancel_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 0, 122);
  lv_obj_add_event_cb(cancel_btn, on_device_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* cancel_text = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_text, "Cancel");
  lv_obj_center(cancel_text);

  lv_obj_t* save_btn = lv_btn_create(device_modal_);
  lv_obj_set_size(save_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, 0, 122);
  lv_obj_add_event_cb(save_btn, on_device_modal_save, LV_EVENT_CLICKED, this);
  lv_obj_t* save_text = lv_label_create(save_btn);
  lv_label_set_text(save_text, "Save");
  lv_obj_center(save_text);

  keyboard_ = lv_keyboard_create(root_);
  lv_obj_set_size(keyboard_, SCR_WIDTH, 128);
  lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard_, on_keyboard_apply, LV_EVENT_READY, this);
}

void SetupUi::close_device_modal() {
  if (device_modal_ == nullptr) return;
  lv_obj_del(device_modal_);
  device_modal_ = nullptr;
  dd_type_ = nullptr;
  dd_transport_ = nullptr;
  ta_device_name_ = nullptr;
  if (keyboard_ != nullptr) {
    lv_obj_del(keyboard_);
    keyboard_ = nullptr;
  }
}

bool SetupUi::save_device_modal() {
  if (dd_type_ == nullptr || dd_transport_ == nullptr || ta_device_name_ == nullptr) {
    lv_label_set_text(selected_device_label_, "Add failed: modal fields unavailable");
    return false;
  }

  char type_buf[32];
  char transport_buf[16];
  lv_dropdown_get_selected_str(dd_type_, type_buf, sizeof(type_buf));
  lv_dropdown_get_selected_str(dd_transport_, transport_buf, sizeof(transport_buf));

  DeviceRecord record;
  record.type = device_type_from_string(type_buf);
  record.transport = transport_type_from_string(transport_buf);
  record.ir_protocol = 3;
  record.name = lv_textarea_get_text(ta_device_name_);
  if (record.name.empty()) record.name = "New Device";
  record.address = "";
  record.enabled = true;

  const size_t before = device_registry_.count();
  if (device_registry_.add(record)) {
    const size_t after = device_registry_.count();
    selected_device_index_ = static_cast<int>(device_registry_.count()) - 1;
    rebuild_device_list();
    lv_label_set_text_fmt(selected_device_label_, "Added device (%u -> %u)", static_cast<unsigned>(before), static_cast<unsigned>(after));
    rebuild_remote_activity_dropdown();
    return true;
  }
  lv_label_set_text(selected_device_label_, "Add failed: registry rejected");
  return false;
}

void SetupUi::open_command_modal() {
  const DeviceRecord* device = selected_device();
  if (device == nullptr || command_modal_ != nullptr) return;
  command_editor_device_id_ = device->id;

  command_modal_ = lv_obj_create(root_);
  lv_obj_set_size(command_modal_, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_center(command_modal_);
  lv_obj_set_style_bg_color(command_modal_, lv_color_hex(0x202020), LV_PART_MAIN);
  lv_obj_set_style_pad_all(command_modal_, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(command_modal_, LV_DIR_VER);
  lv_obj_set_style_pad_bottom(command_modal_, 196, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(command_modal_);
  lv_label_set_text_fmt(title, "Edit: %s", device->name.c_str());
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  dd_command_device_type_ = lv_dropdown_create(command_modal_);
  lv_dropdown_set_options(dd_command_device_type_, kDeviceTypeOptions);
  lv_obj_set_width(dd_command_device_type_, SCR_WIDTH - 12);
  lv_obj_align(dd_command_device_type_, LV_ALIGN_TOP_LEFT, 0, 30);
  switch (device->type) {
    case DeviceType::TV:
      lv_dropdown_set_selected(dd_command_device_type_, 0);
      break;
    case DeviceType::AVR:
      lv_dropdown_set_selected(dd_command_device_type_, 1);
      break;
    case DeviceType::MediaPlayer:
      lv_dropdown_set_selected(dd_command_device_type_, 2);
      break;
    case DeviceType::SmartHome:
      lv_dropdown_set_selected(dd_command_device_type_, 3);
      break;
    case DeviceType::Lighting:
      lv_dropdown_set_selected(dd_command_device_type_, 4);
      break;
    case DeviceType::Custom:
    default:
      lv_dropdown_set_selected(dd_command_device_type_, 5);
      break;
  }

  dd_command_transport_ = lv_dropdown_create(command_modal_);
  lv_dropdown_set_options(dd_command_transport_, kTransportOptions);
  lv_obj_set_width(dd_command_transport_, SCR_WIDTH - 12);
  lv_obj_align(dd_command_transport_, LV_ALIGN_TOP_LEFT, 0, 62);
  lv_obj_add_event_cb(dd_command_transport_, on_command_transport_changed, LV_EVENT_VALUE_CHANGED, this);
  switch (device->transport) {
    case TransportType::IR:
      lv_dropdown_set_selected(dd_command_transport_, 0);
      break;
    case TransportType::BLE:
      lv_dropdown_set_selected(dd_command_transport_, 1);
      break;
    case TransportType::MQTT:
      lv_dropdown_set_selected(dd_command_transport_, 2);
      break;
    case TransportType::HTTP:
    default:
      lv_dropdown_set_selected(dd_command_transport_, 3);
      break;
  }

  dd_command_name_ = lv_dropdown_create(command_modal_);
  lv_obj_set_width(dd_command_name_, SCR_WIDTH - 12);
  lv_obj_align(dd_command_name_, LV_ALIGN_TOP_LEFT, 0, 94);
  lv_obj_add_event_cb(dd_command_name_, on_command_name_changed, LV_EVENT_VALUE_CHANGED, this);

  ta_command_payload_ = lv_textarea_create(command_modal_);
  lv_obj_set_width(ta_command_payload_, SCR_WIDTH - 12);
  lv_obj_set_height(ta_command_payload_, 64);
  lv_obj_align(ta_command_payload_, LV_ALIGN_TOP_LEFT, 0, 128);
  lv_obj_add_event_cb(ta_command_payload_, on_textarea_focus, LV_EVENT_FOCUSED, this);

  command_hint_label_ = lv_label_create(command_modal_);
  lv_obj_set_width(command_hint_label_, SCR_WIDTH - 12);
  lv_label_set_long_mode(command_hint_label_, LV_LABEL_LONG_WRAP);
  lv_obj_align(command_hint_label_, LV_ALIGN_TOP_LEFT, 0, 196);

  command_template_label_ = lv_label_create(command_modal_);
  lv_label_set_text(command_template_label_, "MQTT template");
  lv_obj_align(command_template_label_, LV_ALIGN_TOP_LEFT, 0, 228);

  dd_command_template_ = lv_dropdown_create(command_modal_);
  lv_obj_set_width(dd_command_template_, SCR_WIDTH - 12);
  lv_obj_align(dd_command_template_, LV_ALIGN_TOP_LEFT, 0, 246);
  lv_dropdown_set_options(dd_command_template_, mqtt_template_dropdown_options().c_str());
  lv_dropdown_set_selected(dd_command_template_, 0);

  dd_command_import_mode_ = lv_dropdown_create(command_modal_);
  lv_obj_set_width(dd_command_import_mode_, (SCR_WIDTH - 18) / 2);
  lv_obj_align(dd_command_import_mode_, LV_ALIGN_TOP_LEFT, 0, 280);
  lv_dropdown_set_options(dd_command_import_mode_, "Merge\nReplace");
  lv_dropdown_set_selected(dd_command_import_mode_, 0);

  command_template_apply_btn_ = lv_btn_create(command_modal_);
  lv_obj_set_size(command_template_apply_btn_, (SCR_WIDTH - 18) / 2, 30);
  lv_obj_align(command_template_apply_btn_, LV_ALIGN_TOP_RIGHT, 0, 280);
  lv_obj_add_event_cb(command_template_apply_btn_, on_command_template_apply, LV_EVENT_CLICKED, this);
  lv_obj_t* template_apply_text = lv_label_create(command_template_apply_btn_);
  lv_label_set_text(template_apply_text, "Apply Template");
  lv_obj_center(template_apply_text);

  lv_obj_t* cancel_btn = lv_btn_create(command_modal_);
  lv_obj_set_size(cancel_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 0, 318);
  lv_obj_add_event_cb(cancel_btn, on_command_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* cancel_text = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_text, "Close");
  lv_obj_center(cancel_text);

  lv_obj_t* save_btn = lv_btn_create(command_modal_);
  lv_obj_set_size(save_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(save_btn, LV_ALIGN_TOP_MID, 0, 318);
  lv_obj_add_event_cb(save_btn, on_command_modal_save, LV_EVENT_CLICKED, this);
  lv_obj_t* save_text = lv_label_create(save_btn);
  lv_label_set_text(save_text, "Save");
  lv_obj_center(save_text);

  lv_obj_t* remove_btn = lv_btn_create(command_modal_);
  lv_obj_set_size(remove_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(remove_btn, LV_ALIGN_TOP_RIGHT, 0, 318);
  lv_obj_add_event_cb(remove_btn, on_command_modal_remove, LV_EVENT_CLICKED, this);
  lv_obj_t* remove_text = lv_label_create(remove_btn);
  lv_label_set_text(remove_text, "Remove");
  lv_obj_center(remove_text);

  lv_obj_t* test_btn = lv_btn_create(command_modal_);
  lv_obj_set_size(test_btn, (SCR_WIDTH - 18) / 2, 28);
  lv_obj_align(test_btn, LV_ALIGN_TOP_LEFT, 0, 356);
  lv_obj_add_event_cb(test_btn, on_command_modal_test, LV_EVENT_CLICKED, this);
  lv_obj_t* test_text = lv_label_create(test_btn);
  lv_label_set_text(test_text, "Test Command");
  lv_obj_center(test_text);

  command_learn_btn_ = lv_btn_create(command_modal_);
  lv_obj_set_size(command_learn_btn_, (SCR_WIDTH - 18) / 2, 28);
  lv_obj_align(command_learn_btn_, LV_ALIGN_TOP_RIGHT, 0, 356);
  lv_obj_add_event_cb(command_learn_btn_, on_command_modal_learn, LV_EVENT_CLICKED, this);
  lv_obj_t* learn_text = lv_label_create(command_learn_btn_);
  lv_label_set_text(learn_text, "Learn IR");
  lv_obj_center(learn_text);

  command_learn_status_ = lv_label_create(command_modal_);
  lv_label_set_text(command_learn_status_, "Ready");
  lv_obj_align(command_learn_status_, LV_ALIGN_TOP_LEFT, 0, 390);

  keyboard_ = lv_keyboard_create(root_);
  lv_obj_set_size(keyboard_, SCR_WIDTH, 136);
  lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard_, on_keyboard_apply, LV_EVENT_READY, this);

  refresh_command_name_dropdown();
  update_command_payload_for_selected_name();
  refresh_command_modal_transport_ui();
}

void SetupUi::close_command_modal() {
  stop_ir_learning();
  close_command_name_modal();
  if (command_modal_ == nullptr) return;
  lv_obj_del(command_modal_);
  command_modal_ = nullptr;
  dd_command_device_type_ = nullptr;
  dd_command_transport_ = nullptr;
  dd_command_name_ = nullptr;
  ta_command_payload_ = nullptr;
  command_hint_label_ = nullptr;
  command_template_label_ = nullptr;
  dd_command_template_ = nullptr;
  dd_command_import_mode_ = nullptr;
  command_template_apply_btn_ = nullptr;
  command_learn_btn_ = nullptr;
  command_learn_status_ = nullptr;
  if (keyboard_ != nullptr) {
    lv_obj_del(keyboard_);
    keyboard_ = nullptr;
  }
}

void SetupUi::refresh_command_modal_transport_ui() {
  if (dd_command_transport_ == nullptr) return;

  char transport_buf[16];
  lv_dropdown_get_selected_str(dd_command_transport_, transport_buf, sizeof(transport_buf));
  const TransportType selected_transport = transport_type_from_string(transport_buf);

  if (ta_command_payload_ != nullptr) {
    if (selected_transport == TransportType::MQTT) {
      lv_textarea_set_placeholder_text(ta_command_payload_, "MQTT: topic|payload");
    } else if (selected_transport == TransportType::BLE) {
      lv_textarea_set_placeholder_text(ta_command_payload_, "BLE: key:up / media:playpause / text:hello");
    } else if (selected_transport == TransportType::HTTP) {
      lv_textarea_set_placeholder_text(ta_command_payload_, "HTTP command payload (future support)");
    } else {
      lv_textarea_set_placeholder_text(ta_command_payload_, "IR payload: data:bits:repeat (e.g. 0x20DF10EF:32:0)");
    }
  }

  if (command_hint_label_ != nullptr) {
    if (selected_transport == TransportType::MQTT) {
      lv_label_set_text(command_hint_label_, "Use template import or enter topic|payload manually.");
    } else if (selected_transport == TransportType::BLE) {
      lv_label_set_text(command_hint_label_, "Use key:<name>, media:<name>, or text:<value>. Optional target: AA:BB:..@key:up");
    } else if (selected_transport == TransportType::HTTP) {
      lv_label_set_text(command_hint_label_, "HTTP command dispatch is not enabled yet in this target.");
    } else {
      lv_label_set_text(command_hint_label_, "Select 'Add New' in the list to create a command name.");
    }
  }

  const bool mqtt_mode = selected_transport == TransportType::MQTT;
  const bool ir_mode = selected_transport == TransportType::IR;

  if (command_template_label_ != nullptr) {
    if (mqtt_mode) {
      lv_obj_clear_flag(command_template_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(command_template_label_, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (dd_command_template_ != nullptr) {
    if (mqtt_mode) {
      lv_obj_clear_flag(dd_command_template_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(dd_command_template_, LV_OBJ_FLAG_HIDDEN);
      lv_dropdown_set_selected(dd_command_template_, 0);
    }
  }
  if (dd_command_import_mode_ != nullptr) {
    if (mqtt_mode) {
      lv_obj_clear_flag(dd_command_import_mode_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(dd_command_import_mode_, LV_OBJ_FLAG_HIDDEN);
      lv_dropdown_set_selected(dd_command_import_mode_, 0);
    }
  }
  if (command_template_apply_btn_ != nullptr) {
    if (mqtt_mode) {
      lv_obj_clear_flag(command_template_apply_btn_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(command_template_apply_btn_, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (command_learn_btn_ != nullptr) {
    if (ir_mode) {
      lv_obj_clear_flag(command_learn_btn_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(command_learn_btn_, LV_OBJ_FLAG_HIDDEN);
      stop_ir_learning();
    }
  }
  if (command_learn_status_ != nullptr) {
    if (selected_transport == TransportType::MQTT) {
      lv_label_set_text(command_learn_status_, "MQTT format: topic|payload");
    } else if (selected_transport == TransportType::BLE) {
      lv_label_set_text(command_learn_status_, "BLE format: key/media/text");
    } else if (selected_transport == TransportType::HTTP) {
      lv_label_set_text(command_learn_status_, "HTTP format: pending runtime support");
    } else {
      lv_label_set_text(command_learn_status_, "Learn: idle");
    }
  }
}

bool SetupUi::apply_command_template() {
  if (command_editor_device_id_ == 0 || dd_command_transport_ == nullptr || dd_command_template_ == nullptr ||
      ta_command_payload_ == nullptr || dd_command_name_ == nullptr) {
    return false;
  }

  DeviceRecord* device = device_registry_.get_by_id(command_editor_device_id_);
  if (device == nullptr) return false;

  char transport_buf[16];
  lv_dropdown_get_selected_str(dd_command_transport_, transport_buf, sizeof(transport_buf));
  const TransportType selected_transport = transport_type_from_string(transport_buf);
  if (selected_transport != TransportType::MQTT) {
    if (command_learn_status_ != nullptr) {
      lv_label_set_text(command_learn_status_, "Templates apply to MQTT commands only");
    }
    return false;
  }

  const uint16_t template_index = lv_dropdown_get_selected(dd_command_template_);
  const MqttTemplateDef* tmpl = mqtt_template_for_index(template_index);
  if (tmpl == nullptr || tmpl->command_name == nullptr || std::string(tmpl->command_name).empty()) {
    if (command_learn_status_ != nullptr) lv_label_set_text(command_learn_status_, "Select a template first");
    return false;
  }

  if (selected_command_name().empty()) {
    if (find_command_by_name(*device, tmpl->command_name) == nullptr) {
      upsert_command(device, tmpl->command_name, "");
      device_registry_.save();
      refresh_command_name_dropdown();
    }
    for (uint16_t i = 0; i < device->commands.size(); ++i) {
      if (device->commands[i].name == tmpl->command_name) {
        lv_dropdown_set_selected(dd_command_name_, i);
        break;
      }
    }
    update_command_payload_for_selected_name();
  }

  std::string topic_base = trim_copy(device->address);
  if (topic_base.empty()) topic_base = "cmnd/device";
  std::string topic = topic_base;
  if (tmpl->topic_suffix != nullptr && tmpl->topic_suffix[0] != '\0') {
    if (!topic.empty() && topic.back() != '/') topic.push_back('/');
    topic += tmpl->topic_suffix;
  }
  std::string template_payload = topic;
  template_payload += "|";
  template_payload += (tmpl->payload == nullptr ? "" : tmpl->payload);

  const std::string existing_payload = trim_copy(lv_textarea_get_text(ta_command_payload_));
  const uint16_t import_mode = dd_command_import_mode_ != nullptr ? lv_dropdown_get_selected(dd_command_import_mode_) : 0;
  const bool replace_mode = (import_mode == 1);
  if (!replace_mode && !existing_payload.empty() && existing_payload != template_payload) {
    if (command_learn_status_ != nullptr) {
      lv_label_set_text(command_learn_status_, "Template skipped (merge): payload already set");
    }
    return false;
  }

  lv_textarea_set_text(ta_command_payload_, template_payload.c_str());
  if (command_learn_status_ != nullptr) {
    lv_label_set_text_fmt(command_learn_status_, "Template applied: %s", tmpl->label);
  }
  return true;
}

bool SetupUi::save_command_modal() {
  if (dd_command_name_ == nullptr || ta_command_payload_ == nullptr || dd_command_device_type_ == nullptr ||
      dd_command_transport_ == nullptr || command_editor_device_id_ == 0) return false;
  DeviceRecord* device = device_registry_.get_by_id(command_editor_device_id_);
  if (device == nullptr) return false;

  char type_buf[32];
  lv_dropdown_get_selected_str(dd_command_device_type_, type_buf, sizeof(type_buf));
  const DeviceType selected_type = device_type_from_string(type_buf);

  char transport_buf[16];
  lv_dropdown_get_selected_str(dd_command_transport_, transport_buf, sizeof(transport_buf));
  const TransportType selected_transport = transport_type_from_string(transport_buf);

  const bool type_changed = (device->type != selected_type);
  const bool transport_changed = (device->transport != selected_transport);
  if (type_changed) {
    device->type = selected_type;
  }
  if (transport_changed) {
    device->transport = selected_transport;
  }

  const std::string command_name = selected_command_name();
  if (command_name.empty()) {
    if (type_changed || transport_changed) {
      device_registry_.save();
      rebuild_device_list();
      rebuild_remote_command_buttons();
      lv_label_set_text_fmt(remote_status_, "Saved device: %s / %s", to_string(device->type), to_string(device->transport));
      return true;
    }
    lv_label_set_text(remote_status_, "Select or add a command name");
    return false;
  }
  std::string payload = lv_textarea_get_text(ta_command_payload_);
  const DeviceCommand* existing_command = find_command_by_name(*device, command_name);
  const bool payload_changed = (existing_command == nullptr) || (existing_command->payload != payload);

  if (!payload_changed && (type_changed || transport_changed)) {
    device_registry_.save();
    rebuild_device_list();
    rebuild_remote_command_buttons();
    lv_label_set_text_fmt(remote_status_, "Saved device: %s / %s", to_string(device->type), to_string(device->transport));
    refresh_command_name_dropdown();
    return true;
  }

  std::string payload_error;
  if (selected_transport == TransportType::IR) {
    if (!validate_ir_payload(payload, &payload_error)) {
      if (command_learn_status_ != nullptr) {
        lv_label_set_text_fmt(command_learn_status_, "IR format error: %s", payload_error.c_str());
      }
      lv_label_set_text_fmt(remote_status_, "Invalid IR payload: %s", payload_error.c_str());
      return false;
    }
  } else if (selected_transport == TransportType::MQTT) {
    if (!validate_mqtt_payload(payload, device->address, &payload_error)) {
      if (command_learn_status_ != nullptr) {
        lv_label_set_text_fmt(command_learn_status_, "MQTT format error: %s", payload_error.c_str());
      }
      lv_label_set_text_fmt(remote_status_, "Invalid MQTT payload: %s", payload_error.c_str());
      return false;
    }
  } else if (selected_transport == TransportType::BLE) {
    if (!validate_ble_payload(payload, &payload_error)) {
      if (command_learn_status_ != nullptr) {
        lv_label_set_text_fmt(command_learn_status_, "BLE format error: %s", payload_error.c_str());
      }
      lv_label_set_text_fmt(remote_status_, "Invalid BLE payload: %s", payload_error.c_str());
      return false;
    }
  }
  upsert_command(device, command_name, payload);
  device_registry_.save();
  if (type_changed || transport_changed) {
    rebuild_device_list();
  }
  rebuild_remote_command_buttons();
  if (command_learn_status_ != nullptr) {
    if (selected_transport == TransportType::IR) {
      lv_label_set_text(command_learn_status_, "IR format: valid");
    } else if (selected_transport == TransportType::MQTT) {
      lv_label_set_text(command_learn_status_, "MQTT format: valid");
    } else if (selected_transport == TransportType::BLE) {
      lv_label_set_text(command_learn_status_, "BLE format: valid");
    }
  }
  lv_label_set_text_fmt(remote_status_, "Saved command: %s", command_name.c_str());
  rebuild_remote_command_buttons();
  refresh_command_name_dropdown();
  return true;
}

bool SetupUi::remove_command_modal() {
  if (dd_command_name_ == nullptr || command_editor_device_id_ == 0) return false;
  DeviceRecord* device = device_registry_.get_by_id(command_editor_device_id_);
  if (device == nullptr) return false;

  const std::string command_name = selected_command_name();
  if (command_name.empty()) {
    lv_label_set_text(remote_status_, "Select a command to remove");
    return false;
  }

  for (auto it = device->commands.begin(); it != device->commands.end(); ++it) {
    if (it->name == command_name) {
      device->commands.erase(it);
      device_registry_.save();
      if (!remove_command_references_from_activities(command_editor_device_id_, command_name) && settings_status_ != nullptr) {
        lv_label_set_text(settings_status_, "Warning: failed to clean key bindings");
      }
      refresh_command_name_dropdown();
      update_command_payload_for_selected_name();
      rebuild_activity_list();
      if (activity_keymap_modal_ != nullptr) {
        refresh_activity_key_options();
        refresh_activity_keymap_binding_hint();
      }
      lv_label_set_text_fmt(remote_status_, "Removed command: %s", command_name.c_str());
      rebuild_remote_command_buttons();
      return true;
    }
  }
  return false;
}

std::string SetupUi::selected_command_name() const {
  if (dd_command_name_ == nullptr) return "";
  char name_buf[64];
  lv_dropdown_get_selected_str(dd_command_name_, name_buf, sizeof(name_buf));
  std::string selected = trim_copy(name_buf);
  if (selected == "No commands" || selected == "Add New") return "";
  return selected;
}

void SetupUi::refresh_command_name_dropdown() {
  if (dd_command_name_ == nullptr || command_editor_device_id_ == 0) return;
  const DeviceRecord* device = device_registry_.get_by_id(command_editor_device_id_);
  if (device == nullptr) return;

  std::string previous = selected_command_name();
  std::string options;
  for (size_t i = 0; i < device->commands.size(); ++i) {
    if (i > 0) options += "\n";
    options += device->commands[i].name;
  }
  if (!options.empty()) options += "\n";
  options += "Add New";
  lv_dropdown_set_options(dd_command_name_, options.c_str());

  if (previous.empty()) return;
  for (uint16_t i = 0; i < device->commands.size(); ++i) {
    if (device->commands[i].name == previous) {
      lv_dropdown_set_selected(dd_command_name_, i);
      break;
    }
  }
}

void SetupUi::update_command_payload_for_selected_name() {
  if (dd_command_name_ == nullptr || ta_command_payload_ == nullptr || command_editor_device_id_ == 0) return;
  const DeviceRecord* device = device_registry_.get_by_id(command_editor_device_id_);
  if (device == nullptr) return;

  std::string command_name = selected_command_name();
  const DeviceCommand* command = find_command_by_name(*device, command_name);
  lv_textarea_set_text(ta_command_payload_, command != nullptr ? command->payload.c_str() : "");
}

void SetupUi::open_command_name_modal() {
  if (command_name_modal_ != nullptr || command_modal_ == nullptr) return;
  command_name_modal_ = lv_obj_create(command_modal_);
  lv_obj_set_size(command_name_modal_, SCR_WIDTH - 20, 124);
  lv_obj_center(command_name_modal_);
  lv_obj_set_style_bg_color(command_name_modal_, lv_color_hex(0x303030), LV_PART_MAIN);
  lv_obj_set_style_pad_all(command_name_modal_, 6, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(command_name_modal_);
  lv_label_set_text(title, "Add New Command");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  std::string common_options;
  const std::vector<std::string>& common = common_command_names();
  for (size_t i = 0; i < common.size(); ++i) {
    if (i > 0) common_options += "\n";
    common_options += common[i];
  }
  common_options += "\nCustom";

  dd_new_command_common_ = lv_dropdown_create(command_name_modal_);
  lv_dropdown_set_options(dd_new_command_common_, common_options.c_str());
  lv_obj_set_width(dd_new_command_common_, SCR_WIDTH - 36);
  lv_obj_align(dd_new_command_common_, LV_ALIGN_TOP_LEFT, 0, 20);

  ta_new_command_custom_ = lv_textarea_create(command_name_modal_);
  lv_textarea_set_one_line(ta_new_command_custom_, true);
  lv_textarea_set_placeholder_text(ta_new_command_custom_, "Custom name (if Custom selected)");
  lv_obj_set_width(ta_new_command_custom_, SCR_WIDTH - 36);
  lv_obj_align(ta_new_command_custom_, LV_ALIGN_TOP_LEFT, 0, 52);
  lv_obj_add_event_cb(ta_new_command_custom_, on_textarea_focus, LV_EVENT_FOCUSED, this);

  lv_obj_t* close_btn = lv_btn_create(command_name_modal_);
  lv_obj_set_size(close_btn, (SCR_WIDTH - 46) / 2, 26);
  lv_obj_align(close_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_add_event_cb(close_btn, on_command_name_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* close_text = lv_label_create(close_btn);
  lv_label_set_text(close_text, "Close");
  lv_obj_center(close_text);

  lv_obj_t* add_btn = lv_btn_create(command_name_modal_);
  lv_obj_set_size(add_btn, (SCR_WIDTH - 46) / 2, 26);
  lv_obj_align(add_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_event_cb(add_btn, on_command_name_modal_save, LV_EVENT_CLICKED, this);
  lv_obj_t* add_text = lv_label_create(add_btn);
  lv_label_set_text(add_text, "Add");
  lv_obj_center(add_text);
}

void SetupUi::close_command_name_modal() {
  if (command_name_modal_ == nullptr) return;
  lv_obj_del(command_name_modal_);
  command_name_modal_ = nullptr;
  dd_new_command_common_ = nullptr;
  ta_new_command_custom_ = nullptr;
  if (dd_command_name_ != nullptr) {
    char selected[64];
    lv_dropdown_get_selected_str(dd_command_name_, selected, sizeof(selected));
    if (trim_copy(selected) == "Add New") {
      lv_dropdown_set_selected(dd_command_name_, 0);
      update_command_payload_for_selected_name();
    }
  }
}

bool SetupUi::save_command_name_modal() {
  if (command_editor_device_id_ == 0 || dd_new_command_common_ == nullptr) return false;
  DeviceRecord* device = device_registry_.get_by_id(command_editor_device_id_);
  if (device == nullptr) return false;

  char selected_buf[48];
  lv_dropdown_get_selected_str(dd_new_command_common_, selected_buf, sizeof(selected_buf));
  std::string command_name = trim_copy(selected_buf);
  if (command_name == "Custom") {
    if (ta_new_command_custom_ == nullptr) return false;
    command_name = trim_copy(lv_textarea_get_text(ta_new_command_custom_));
  }
  if (command_name.empty()) return false;

  if (find_command_by_name(*device, command_name) == nullptr) {
    upsert_command(device, command_name, "");
    device_registry_.save();
  }
  refresh_command_name_dropdown();
  for (uint16_t i = 0; i < device->commands.size(); ++i) {
    if (device->commands[i].name == command_name) {
      lv_dropdown_set_selected(dd_command_name_, i);
      break;
    }
  }
  update_command_payload_for_selected_name();
  return true;
}

void SetupUi::open_rename_modal(bool rename_activity) {
  if (rename_modal_ != nullptr) return;
  rename_activity_target_ = rename_activity;

  std::string current_name;
  if (rename_activity) {
    const std::vector<ActivityRecord>& activities = activity_registry_.all();
    if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) {
      lv_label_set_text(selected_activity_label_, "Selected: choose activity first");
      return;
    }
    current_name = activities[selected_activity_index_].name;
  } else {
    const DeviceRecord* device = selected_device();
    if (device == nullptr) {
      lv_label_set_text(selected_device_label_, "Selected: choose a device first");
      return;
    }
    current_name = device->name;
  }

  rename_modal_ = lv_obj_create(root_);
  lv_obj_set_size(rename_modal_, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_center(rename_modal_);
  lv_obj_set_style_bg_color(rename_modal_, lv_color_hex(0x202020), LV_PART_MAIN);
  lv_obj_set_style_pad_all(rename_modal_, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(rename_modal_, LV_DIR_VER);
  lv_obj_set_style_pad_bottom(rename_modal_, 148, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(rename_modal_);
  lv_label_set_text(title, rename_activity ? "Rename Activity" : "Rename Device");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  ta_rename_name_ = lv_textarea_create(rename_modal_);
  lv_textarea_set_one_line(ta_rename_name_, true);
  lv_obj_set_width(ta_rename_name_, SCR_WIDTH - 12);
  lv_textarea_set_text(ta_rename_name_, current_name.c_str());
  lv_obj_align(ta_rename_name_, LV_ALIGN_TOP_LEFT, 0, 30);
  lv_obj_add_event_cb(ta_rename_name_, on_textarea_focus, LV_EVENT_FOCUSED, this);

  lv_obj_t* cancel_btn = lv_btn_create(rename_modal_);
  lv_obj_set_size(cancel_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 0, 68);
  lv_obj_add_event_cb(cancel_btn, on_rename_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* cancel_text = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_text, "Close");
  lv_obj_center(cancel_text);

  lv_obj_t* save_btn = lv_btn_create(rename_modal_);
  lv_obj_set_size(save_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, 0, 68);
  lv_obj_add_event_cb(save_btn, on_rename_modal_save, LV_EVENT_CLICKED, this);
  lv_obj_t* save_text = lv_label_create(save_btn);
  lv_label_set_text(save_text, "Save");
  lv_obj_center(save_text);

  keyboard_ = lv_keyboard_create(root_);
  lv_obj_set_size(keyboard_, SCR_WIDTH, 136);
  lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard_, on_keyboard_apply, LV_EVENT_READY, this);
}

void SetupUi::close_rename_modal() {
  if (rename_modal_ == nullptr) return;
  lv_obj_del(rename_modal_);
  rename_modal_ = nullptr;
  ta_rename_name_ = nullptr;
  if (keyboard_ != nullptr) {
    lv_obj_del(keyboard_);
    keyboard_ = nullptr;
  }
}

bool SetupUi::save_rename_modal() {
  if (ta_rename_name_ == nullptr) return false;
  std::string name = trim_copy(lv_textarea_get_text(ta_rename_name_));
  if (name.empty()) {
    name = rename_activity_target_ ? "New Activity" : "New Device";
  }

  if (rename_activity_target_) {
    const std::vector<ActivityRecord>& activities = activity_registry_.all();
    if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) return false;
    ActivityRecord updated = activities[selected_activity_index_];
    updated.name = name;
    if (!activity_registry_.upsert(updated)) return false;
    rebuild_activity_list();
    rebuild_remote_activity_dropdown();
    lv_label_set_text_fmt(selected_activity_label_, "Renamed: %s", updated.name.c_str());
    if (remote_status_ != nullptr) {
      lv_label_set_text_fmt(remote_status_, "Activity: %s", updated.name.c_str());
    }
    return true;
  }

  const DeviceRecord* current = selected_device();
  if (current == nullptr) return false;
  DeviceRecord updated = *current;
  updated.name = name;
  if (!device_registry_.upsert(updated)) return false;
  rebuild_device_list();
  lv_label_set_text_fmt(selected_device_label_, "Renamed: %s", updated.name.c_str());
  return true;
}

void SetupUi::test_current_command() {
  if (command_editor_device_id_ == 0) return;
  if (!save_command_modal()) return;
  const std::string command_name = selected_command_name();
  if (command_name.empty()) return;
  if (dispatcher_.dispatch_device_command(command_editor_device_id_, command_name)) {
    if (remote_status_ != nullptr) {
      lv_label_set_text_fmt(remote_status_, "Test sent: %s", command_name.c_str());
    }
  } else if (remote_status_ != nullptr) {
    lv_label_set_text_fmt(remote_status_, "Test failed: %s", dispatcher_.last_status().c_str());
  }
}

void SetupUi::start_ir_learning() {
  if (ir_learning_active_) return;
  ir_learning_active_ = true;
  g_active_ui = this;
  set_announceNewIRmessage_cb_HAL(&ir_learn_message_cb);
  set_irReceiverEnabled_HAL(true);
  start_infraredReceiver_HAL();
  if (command_learn_status_ != nullptr) {
    lv_label_set_text(command_learn_status_, "Learn: waiting for remote...");
  }
}

void SetupUi::stop_ir_learning() {
  if (!ir_learning_active_) return;
  ir_learning_active_ = false;
  set_announceNewIRmessage_cb_HAL(nullptr);
  set_irReceiverEnabled_HAL(false);
  shutdown_infraredReceiver_HAL();
  if (g_active_ui == this) g_active_ui = nullptr;
  if (command_learn_status_ != nullptr) {
    lv_label_set_text(command_learn_status_, "Learn: idle");
  }
}

void SetupUi::handle_ir_learned_message(const std::string& message) {
  if (!ir_learning_active_ || ta_command_payload_ == nullptr || command_learn_status_ == nullptr) return;

  size_t sep = message.find(' ');
  if (sep == std::string::npos) {
    lv_label_set_text(command_learn_status_, "Learn: parse failed");
    return;
  }
  std::string protocol = message.substr(0, sep);
  std::string payload = trim_copy(message.substr(sep + 1));
  std::string payload_error;
  if (!validate_ir_payload(payload, &payload_error)) {
    lv_label_set_text_fmt(command_learn_status_, "Learn: bad payload (%s)", payload_error.c_str());
    return;
  }

  lv_textarea_set_text(ta_command_payload_, payload.c_str());
  DeviceRecord* device = device_registry_.get_by_id(command_editor_device_id_);
  if (device != nullptr) {
    int learned_protocol = protocol_from_name(protocol);
    if (learned_protocol > 0) {
      device->ir_protocol = learned_protocol;
    }
  }
  lv_label_set_text_fmt(command_learn_status_, "Learned: %s", protocol.c_str());
  stop_ir_learning();
}

void SetupUi::open_activity_modal() {
  if (activity_modal_ != nullptr) return;

  activity_modal_ = lv_obj_create(root_);
  lv_obj_set_size(activity_modal_, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_center(activity_modal_);
  lv_obj_set_style_bg_color(activity_modal_, lv_color_hex(0x202020), LV_PART_MAIN);
  lv_obj_set_style_pad_all(activity_modal_, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(activity_modal_, LV_DIR_VER);
  lv_obj_set_style_pad_bottom(activity_modal_, 148, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(activity_modal_);
  lv_label_set_text(title, "Add Activity");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  ta_activity_name_ = lv_textarea_create(activity_modal_);
  lv_textarea_set_one_line(ta_activity_name_, true);
  lv_textarea_set_placeholder_text(ta_activity_name_, "Activity Name");
  lv_obj_set_width(ta_activity_name_, SCR_WIDTH - 12);
  lv_obj_align(ta_activity_name_, LV_ALIGN_TOP_LEFT, 0, 24);
  lv_obj_add_event_cb(ta_activity_name_, on_textarea_focus, LV_EVENT_FOCUSED, this);

  lv_obj_t* note = lv_label_create(activity_modal_);
  lv_label_set_text(note, "Devices are mapped in Edit.");
  lv_obj_align(note, LV_ALIGN_TOP_LEFT, 0, 58);

  lv_obj_t* cancel_btn = lv_btn_create(activity_modal_);
  lv_obj_set_size(cancel_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 0, 84);
  lv_obj_add_event_cb(cancel_btn, on_activity_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* cancel_text = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_text, "Cancel");
  lv_obj_center(cancel_text);

  lv_obj_t* save_btn = lv_btn_create(activity_modal_);
  lv_obj_set_size(save_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, 0, 84);
  lv_obj_add_event_cb(save_btn, on_activity_modal_save, LV_EVENT_CLICKED, this);
  lv_obj_t* save_text = lv_label_create(save_btn);
  lv_label_set_text(save_text, "Save");
  lv_obj_center(save_text);

  keyboard_ = lv_keyboard_create(root_);
  lv_obj_set_size(keyboard_, SCR_WIDTH, 126);
  lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard_, on_keyboard_apply, LV_EVENT_READY, this);
}

void SetupUi::close_activity_modal() {
  if (activity_modal_ == nullptr) return;
  lv_obj_del(activity_modal_);
  activity_modal_ = nullptr;
  ta_activity_name_ = nullptr;
  if (keyboard_ != nullptr) {
    lv_obj_del(keyboard_);
    keyboard_ = nullptr;
  }
}

bool SetupUi::save_activity_modal() {
  if (ta_activity_name_ == nullptr) {
    lv_label_set_text(selected_activity_label_, "Add failed: modal unavailable");
    return false;
  }

  ActivityRecord activity;
  activity.name = lv_textarea_get_text(ta_activity_name_);
  if (activity.name.empty()) activity.name = "New Activity";

  const size_t before = activity_registry_.count();
  if (activity_registry_.add(activity)) {
    const size_t after = activity_registry_.count();
    selected_activity_index_ = static_cast<int>(activity_registry_.count()) - 1;
    rebuild_activity_list();
    lv_label_set_text_fmt(selected_activity_label_, "Added activity (%u -> %u)", static_cast<unsigned>(before), static_cast<unsigned>(after));
    rebuild_remote_activity_dropdown();
    return true;
  }
  lv_label_set_text(selected_activity_label_, "Add failed: registry rejected");
  return false;
}

void SetupUi::open_activity_keymap_modal() {
  if (activity_keymap_modal_ != nullptr) return;
  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) {
    lv_label_set_text(selected_activity_label_, "Selected: choose activity first");
    return;
  }

  activity_keymap_modal_ = lv_obj_create(root_);
  lv_obj_set_size(activity_keymap_modal_, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_center(activity_keymap_modal_);
  lv_obj_set_style_bg_color(activity_keymap_modal_, lv_color_hex(0x202020), LV_PART_MAIN);
  lv_obj_set_style_pad_all(activity_keymap_modal_, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(activity_keymap_modal_, LV_DIR_VER);
  lv_obj_set_style_pad_bottom(activity_keymap_modal_, 148, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(activity_keymap_modal_);
  lv_label_set_text_fmt(title, "Edit: %s", activities[selected_activity_index_].name.c_str());
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  dd_keymap_key_ = lv_dropdown_create(activity_keymap_modal_);
  lv_obj_set_width(dd_keymap_key_, SCR_WIDTH - 12);
  lv_obj_align(dd_keymap_key_, LV_ALIGN_TOP_LEFT, 0, 30);
  lv_obj_add_event_cb(dd_keymap_key_, on_activity_keymap_command_changed, LV_EVENT_VALUE_CHANGED, this);

  ta_keymap_device_filter_ = lv_textarea_create(activity_keymap_modal_);
  lv_textarea_set_one_line(ta_keymap_device_filter_, true);
  lv_textarea_set_placeholder_text(ta_keymap_device_filter_, "Filter devices");
  lv_obj_set_width(ta_keymap_device_filter_, SCR_WIDTH - 12);
  lv_obj_align(ta_keymap_device_filter_, LV_ALIGN_TOP_LEFT, 0, 62);
  lv_obj_add_event_cb(ta_keymap_device_filter_, on_textarea_focus, LV_EVENT_FOCUSED, this);
  lv_obj_add_event_cb(ta_keymap_device_filter_, on_activity_keymap_device_filter_changed, LV_EVENT_VALUE_CHANGED, this);

  dd_keymap_device_ = lv_dropdown_create(activity_keymap_modal_);
  lv_obj_set_width(dd_keymap_device_, SCR_WIDTH - 12);
  lv_obj_align(dd_keymap_device_, LV_ALIGN_TOP_LEFT, 0, 92);
  lv_obj_add_event_cb(dd_keymap_device_, on_activity_keymap_device_changed, LV_EVENT_VALUE_CHANGED, this);

  ta_keymap_command_filter_ = lv_textarea_create(activity_keymap_modal_);
  lv_textarea_set_one_line(ta_keymap_command_filter_, true);
  lv_textarea_set_placeholder_text(ta_keymap_command_filter_, "Filter commands");
  lv_obj_set_width(ta_keymap_command_filter_, SCR_WIDTH - 12);
  lv_obj_align(ta_keymap_command_filter_, LV_ALIGN_TOP_LEFT, 0, 124);
  lv_obj_add_event_cb(ta_keymap_command_filter_, on_textarea_focus, LV_EVENT_FOCUSED, this);
  lv_obj_add_event_cb(ta_keymap_command_filter_, on_activity_keymap_command_filter_changed, LV_EVENT_VALUE_CHANGED, this);

  dd_keymap_command_ = lv_dropdown_create(activity_keymap_modal_);
  lv_obj_set_width(dd_keymap_command_, SCR_WIDTH - 12);
  lv_obj_align(dd_keymap_command_, LV_ALIGN_TOP_LEFT, 0, 154);
  lv_obj_add_event_cb(dd_keymap_command_, on_activity_keymap_command_changed, LV_EVENT_VALUE_CHANGED, this);

  keymap_status_label_ = lv_label_create(activity_keymap_modal_);
  lv_label_set_text(keymap_status_label_, "Pick key + device + command, then Save");
  lv_obj_align(keymap_status_label_, LV_ALIGN_TOP_LEFT, 0, 186);

  keymap_slot_hint_label_ = lv_label_create(activity_keymap_modal_);
  lv_obj_set_width(keymap_slot_hint_label_, SCR_WIDTH - 12);
  lv_label_set_long_mode(keymap_slot_hint_label_, LV_LABEL_LONG_WRAP);
  lv_obj_align(keymap_slot_hint_label_, LV_ALIGN_TOP_LEFT, 0, 202);

  lv_obj_t* close_btn = lv_btn_create(activity_keymap_modal_);
  lv_obj_set_size(close_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 0, 250);
  lv_obj_add_event_cb(close_btn, on_activity_keymap_close, LV_EVENT_CLICKED, this);
  lv_obj_t* close_text = lv_label_create(close_btn);
  lv_label_set_text(close_text, "Close");
  lv_obj_center(close_text);

  lv_obj_t* save_btn = lv_btn_create(activity_keymap_modal_);
  lv_obj_set_size(save_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, 0, 250);
  lv_obj_add_event_cb(save_btn, on_activity_keymap_save, LV_EVENT_CLICKED, this);
  lv_obj_t* save_text = lv_label_create(save_btn);
  lv_label_set_text(save_text, "Save Mapping");
  lv_obj_center(save_text);

  keyboard_ = lv_keyboard_create(root_);
  lv_obj_set_size(keyboard_, SCR_WIDTH, 128);
  lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard_, on_keyboard_apply, LV_EVENT_READY, this);

  refresh_activity_key_options();
  refresh_activity_keymap_device_dropdown();
  refresh_activity_keymap_command_dropdown();
  refresh_activity_keymap_binding_hint();
}

void SetupUi::close_activity_keymap_modal() {
  close_activity_keymap_overwrite_modal();
  if (activity_keymap_modal_ == nullptr) return;
  lv_obj_del(activity_keymap_modal_);
  activity_keymap_modal_ = nullptr;
  dd_keymap_key_ = nullptr;
  ta_keymap_device_filter_ = nullptr;
  dd_keymap_device_ = nullptr;
  ta_keymap_command_filter_ = nullptr;
  dd_keymap_command_ = nullptr;
  keymap_status_label_ = nullptr;
  keymap_slot_hint_label_ = nullptr;
  keymap_device_ids_.clear();
  pending_keymap_key_char_ = '\0';
  pending_keymap_device_id_ = 0;
  pending_keymap_command_name_.clear();
  if (keyboard_ != nullptr) {
    lv_obj_del(keyboard_);
    keyboard_ = nullptr;
  }
}

void SetupUi::save_activity_keymap_modal() {
  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) return;
  if (dd_keymap_key_ == nullptr || dd_keymap_device_ == nullptr || dd_keymap_command_ == nullptr || keymap_status_label_ == nullptr) return;

  char command_buf[80];
  lv_dropdown_get_selected_str(dd_keymap_command_, command_buf, sizeof(command_buf));
  uint16_t key_idx = lv_dropdown_get_selected(dd_keymap_key_);
  if (key_idx >= kPhysicalKeyCount) {
    lv_label_set_text(keymap_status_label_, "Invalid mapping");
    return;
  }
  const char key_char = kPhysicalKeys[key_idx].key_char;

  const uint16_t selected_device_idx = lv_dropdown_get_selected(dd_keymap_device_);
  const uint32_t device_id = (selected_device_idx < keymap_device_ids_.size()) ? keymap_device_ids_[selected_device_idx] : 0;
  const DeviceRecord* selected_device = device_registry_.get_by_id(device_id);
  std::string command_name = trim_copy(command_buf);
  if (device_id == 0 || command_name.empty() || command_name == "No commands") {
    lv_label_set_text(keymap_status_label_, "Invalid device/command");
    return;
  }

  const ActivityRecord& activity = activities[selected_activity_index_];
  for (const ActivityKeyBinding& binding : activity.key_bindings) {
    if (binding.key_char != key_char) continue;
    if (binding.device_id == device_id && binding.command_name == command_name) {
      lv_label_set_text_fmt(keymap_status_label_, "No change: %c already mapped", key_char);
      return;
    }
    open_activity_keymap_overwrite_modal(binding, device_id, command_name);
    return;
  }

  pending_keymap_key_char_ = key_char;
  pending_keymap_device_id_ = device_id;
  pending_keymap_command_name_ = command_name;
  apply_activity_keymap_pending_overwrite();
  if (selected_device != nullptr) {
    lv_label_set_text_fmt(keymap_status_label_, "Saved: %c -> %s (%s)", key_char, command_name.c_str(), selected_device->name.c_str());
  }
}

void SetupUi::refresh_activity_key_options() {
  if (dd_keymap_key_ == nullptr) return;
  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) return;

  const uint16_t current = lv_dropdown_get_selected(dd_keymap_key_);
  const ActivityRecord& activity = activities[selected_activity_index_];

  std::string options;
  for (size_t i = 0; i < kPhysicalKeyCount; ++i) {
    bool mapped = false;
    for (const ActivityKeyBinding& binding : activity.key_bindings) {
      if (binding.key_char == kPhysicalKeys[i].key_char && !binding.command_name.empty()) {
        mapped = true;
        break;
      }
    }
    if (!options.empty()) options += "\n";
    options += kPhysicalKeys[i].label;
    options += mapped ? "(*)" : "(none)";
  }
  lv_dropdown_set_options(dd_keymap_key_, options.c_str());
  if (current < kPhysicalKeyCount) {
    lv_dropdown_set_selected(dd_keymap_key_, current);
  }
}

void SetupUi::refresh_activity_keymap_device_dropdown() {
  if (dd_keymap_device_ == nullptr) return;

  std::string selected_before;
  {
    char selected_buf[80];
    lv_dropdown_get_selected_str(dd_keymap_device_, selected_buf, sizeof(selected_buf));
    selected_before = trim_copy(selected_buf);
  }
  const std::string filter = ta_keymap_device_filter_ != nullptr ? lower_copy(trim_copy(lv_textarea_get_text(ta_keymap_device_filter_))) : "";

  std::string options;
  keymap_device_ids_.clear();
  const std::vector<DeviceRecord>& devices = device_registry_.all();
  for (const DeviceRecord& device : devices) {
    const DeviceRecord* d = &device;
    if (d == nullptr) continue;
    if (!filter.empty() && lower_copy(d->name).find(filter) == std::string::npos) continue;
    if (!options.empty()) options += "\n";
    options += d->name;
    keymap_device_ids_.push_back(d->id);
  }
  if (options.empty()) options = "No devices";
  lv_dropdown_set_options(dd_keymap_device_, options.c_str());

  if (!selected_before.empty()) {
    for (uint16_t i = 0; i < keymap_device_ids_.size(); ++i) {
      const DeviceRecord* d = device_registry_.get_by_id(keymap_device_ids_[i]);
      if (d != nullptr && d->name == selected_before) {
        lv_dropdown_set_selected(dd_keymap_device_, i);
        break;
      }
    }
  }
}

void SetupUi::refresh_activity_keymap_command_dropdown() {
  if (dd_keymap_device_ == nullptr || dd_keymap_command_ == nullptr) return;

  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) {
    return;
  }
  std::string selected_before;
  {
    char selected_buf[80];
    lv_dropdown_get_selected_str(dd_keymap_command_, selected_buf, sizeof(selected_buf));
    selected_before = trim_copy(selected_buf);
  }
  const uint16_t selected_idx = lv_dropdown_get_selected(dd_keymap_device_);
  const DeviceRecord* selected_device = (selected_idx < keymap_device_ids_.size()) ? device_registry_.get_by_id(keymap_device_ids_[selected_idx]) : nullptr;
  const std::string filter = ta_keymap_command_filter_ != nullptr ? lower_copy(trim_copy(lv_textarea_get_text(ta_keymap_command_filter_))) : "";

  std::string options;
  if (selected_device != nullptr) {
    for (size_t i = 0; i < selected_device->commands.size(); ++i) {
      const std::string& command_name = selected_device->commands[i].name;
      if (!filter.empty() && lower_copy(command_name).find(filter) == std::string::npos) continue;
      if (!options.empty()) options += "\n";
      options += command_name;
    }
  }
  if (options.empty()) options = "No commands";
  lv_dropdown_set_options(dd_keymap_command_, options.c_str());

  if (!selected_before.empty() && selected_before != "No commands") {
    uint16_t idx = 0;
    if (selected_device != nullptr) {
      for (const DeviceCommand& command : selected_device->commands) {
        if (!filter.empty() && lower_copy(command.name).find(filter) == std::string::npos) continue;
        if (command.name == selected_before) {
          lv_dropdown_set_selected(dd_keymap_command_, idx);
          break;
        }
        ++idx;
      }
    }
  }
}

void SetupUi::refresh_activity_keymap_binding_hint() {
  if (keymap_slot_hint_label_ == nullptr || dd_keymap_key_ == nullptr) return;
  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) {
    lv_label_set_text(keymap_slot_hint_label_, "");
    return;
  }

  uint16_t key_idx = lv_dropdown_get_selected(dd_keymap_key_);
  if (key_idx >= kPhysicalKeyCount) {
    lv_label_set_text(keymap_slot_hint_label_, "");
    return;
  }
  const char key_char = kPhysicalKeys[key_idx].key_char;
  const ActivityRecord& activity = activities[selected_activity_index_];
  for (const ActivityKeyBinding& binding : activity.key_bindings) {
    if (binding.key_char != key_char) continue;
    const DeviceRecord* device = device_registry_.get_by_id(binding.device_id);
    lv_label_set_text_fmt(keymap_slot_hint_label_, "Current mapping: %c -> %s (%s)",
                          key_char,
                          binding.command_name.c_str(),
                          device != nullptr ? device->name.c_str() : "Unknown device");
    return;
  }
  lv_label_set_text(keymap_slot_hint_label_, "Current mapping: none");
}

void SetupUi::open_activity_keymap_overwrite_modal(const ActivityKeyBinding& existing_binding, uint32_t new_device_id,
                                                   const std::string& new_command_name) {
  if (activity_keymap_modal_ == nullptr) return;
  close_activity_keymap_overwrite_modal();

  pending_keymap_key_char_ = existing_binding.key_char;
  pending_keymap_device_id_ = new_device_id;
  pending_keymap_command_name_ = new_command_name;

  const DeviceRecord* existing_device = device_registry_.get_by_id(existing_binding.device_id);
  const DeviceRecord* new_device = device_registry_.get_by_id(new_device_id);

  keymap_overwrite_modal_ = lv_obj_create(activity_keymap_modal_);
  lv_obj_set_size(keymap_overwrite_modal_, SCR_WIDTH - 24, 148);
  lv_obj_center(keymap_overwrite_modal_);
  lv_obj_set_style_bg_color(keymap_overwrite_modal_, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
  lv_obj_set_style_pad_all(keymap_overwrite_modal_, 6, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(keymap_overwrite_modal_);
  lv_label_set_text(title, "Overwrite Mapping?");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t* body = lv_label_create(keymap_overwrite_modal_);
  lv_obj_set_width(body, SCR_WIDTH - 40);
  lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
  lv_label_set_text_fmt(body,
                        "%c is mapped to %s (%s).\nReplace with %s (%s)?",
                        existing_binding.key_char,
                        existing_binding.command_name.c_str(),
                        existing_device != nullptr ? existing_device->name.c_str() : "Unknown",
                        new_command_name.c_str(),
                        new_device != nullptr ? new_device->name.c_str() : "Unknown");
  lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 20);

  lv_obj_t* cancel_btn = lv_btn_create(keymap_overwrite_modal_);
  lv_obj_set_size(cancel_btn, (SCR_WIDTH - 38) / 2, 32);
  lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_add_event_cb(cancel_btn, on_activity_keymap_overwrite_cancel, LV_EVENT_CLICKED, this);
  lv_obj_t* cancel_text = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_text, "Cancel");
  lv_obj_center(cancel_text);

  lv_obj_t* overwrite_btn = lv_btn_create(keymap_overwrite_modal_);
  lv_obj_set_size(overwrite_btn, (SCR_WIDTH - 38) / 2, 32);
  lv_obj_align(overwrite_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_event_cb(overwrite_btn, on_activity_keymap_overwrite_confirm, LV_EVENT_CLICKED, this);
  lv_obj_t* overwrite_text = lv_label_create(overwrite_btn);
  lv_label_set_text(overwrite_text, "Overwrite");
  lv_obj_center(overwrite_text);
}

void SetupUi::close_activity_keymap_overwrite_modal() {
  if (keymap_overwrite_modal_ != nullptr) {
    lv_obj_del(keymap_overwrite_modal_);
    keymap_overwrite_modal_ = nullptr;
  }
  pending_keymap_key_char_ = '\0';
  pending_keymap_device_id_ = 0;
  pending_keymap_command_name_.clear();
}

void SetupUi::apply_activity_keymap_pending_overwrite() {
  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) return;
  if (pending_keymap_key_char_ == '\0' || pending_keymap_device_id_ == 0 || pending_keymap_command_name_.empty()) return;

  ActivityRecord updated = activities[selected_activity_index_];
  bool replaced = false;
  for (ActivityKeyBinding& binding : updated.key_bindings) {
    if (binding.key_char == pending_keymap_key_char_) {
      binding.device_id = pending_keymap_device_id_;
      binding.command_name = pending_keymap_command_name_;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    ActivityKeyBinding binding;
    binding.key_char = pending_keymap_key_char_;
    binding.device_id = pending_keymap_device_id_;
    binding.command_name = pending_keymap_command_name_;
    updated.key_bindings.push_back(binding);
  }
  if (!activity_registry_.upsert(updated)) return;
  rebuild_activity_list();
  refresh_activity_key_options();
  refresh_activity_keymap_device_dropdown();
  refresh_activity_keymap_command_dropdown();
  refresh_activity_keymap_binding_hint();
  close_activity_keymap_overwrite_modal();
}

bool SetupUi::move_device_selection(int delta) {
  if (selected_device_index_ < 0) return false;
  std::vector<DeviceRecord> updated = device_registry_.all();
  const int from = selected_device_index_;
  const int to = from + delta;
  if (to < 0 || to >= static_cast<int>(updated.size())) return false;
  std::swap(updated[from], updated[to]);
  if (!device_registry_.replace_all(std::move(updated))) return false;
  selected_device_index_ = to;
  rebuild_device_list();
  return true;
}

bool SetupUi::move_activity_selection(int delta) {
  if (selected_activity_index_ < 0) return false;
  std::vector<ActivityRecord> updated = activity_registry_.all();
  const int from = selected_activity_index_;
  const int to = from + delta;
  if (to < 0 || to >= static_cast<int>(updated.size())) return false;
  std::swap(updated[from], updated[to]);
  if (!activity_registry_.replace_all(std::move(updated))) return false;
  selected_activity_index_ = to;
  rebuild_activity_list();
  rebuild_remote_activity_dropdown();
  return true;
}

bool SetupUi::duplicate_selected_device() {
  const DeviceRecord* device = selected_device();
  if (device == nullptr) return false;

  DeviceRecord copy = *device;
  copy.id = 0;
  copy.name = copy.name + " Copy";
  if (!device_registry_.add(copy)) return false;
  selected_device_index_ = static_cast<int>(device_registry_.count()) - 1;
  rebuild_device_list();
  rebuild_remote_activity_dropdown();
  return true;
}

bool SetupUi::duplicate_selected_activity() {
  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) return false;
  ActivityRecord copy = activities[selected_activity_index_];
  copy.id = 0;
  copy.name = copy.name + " Copy";
  if (!activity_registry_.add(copy)) return false;
  selected_activity_index_ = static_cast<int>(activity_registry_.count()) - 1;
  rebuild_activity_list();
  rebuild_remote_activity_dropdown();
  return true;
}

void SetupUi::open_activity_builder_modal() {
  if (activity_builder_modal_ != nullptr) return;
  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) {
    lv_label_set_text(selected_activity_label_, "Selected: choose activity first");
    return;
  }

  const ActivityRecord& selected = activities[selected_activity_index_];
  activity_builder_actions_ = selected.startup_actions;

  activity_builder_modal_ = lv_obj_create(root_);
  lv_obj_set_size(activity_builder_modal_, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_center(activity_builder_modal_);
  lv_obj_set_style_bg_color(activity_builder_modal_, lv_color_hex(0x202020), LV_PART_MAIN);
  lv_obj_set_style_pad_all(activity_builder_modal_, 6, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(activity_builder_modal_);
  lv_label_set_text_fmt(title, "Builder: %s", selected.name.c_str());
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  dd_builder_device_ = lv_dropdown_create(activity_builder_modal_);
  lv_obj_set_width(dd_builder_device_, SCR_WIDTH - 12);
  lv_obj_align(dd_builder_device_, LV_ALIGN_TOP_LEFT, 0, 28);
  std::string device_options;
  activity_builder_device_ids_.clear();
  const std::vector<DeviceRecord>& devices = device_registry_.all();
  for (const DeviceRecord& device : devices) {
    if (!device_options.empty()) device_options += "\n";
    device_options += device.name;
    activity_builder_device_ids_.push_back(device.id);
  }
  if (device_options.empty()) device_options = "No devices";
  lv_dropdown_set_options(dd_builder_device_, device_options.c_str());

  dd_builder_slot_ = lv_dropdown_create(activity_builder_modal_);
  lv_dropdown_set_options(dd_builder_slot_, kCommandSlotOptions);
  lv_obj_set_width(dd_builder_slot_, SCR_WIDTH - 12);
  lv_obj_align(dd_builder_slot_, LV_ALIGN_TOP_LEFT, 0, 62);

  builder_status_label_ = lv_label_create(activity_builder_modal_);
  lv_label_set_text(builder_status_label_, "Add startup steps in order");
  lv_obj_align(builder_status_label_, LV_ALIGN_TOP_LEFT, 0, 94);

  builder_preview_label_ = lv_label_create(activity_builder_modal_);
  lv_obj_set_width(builder_preview_label_, SCR_WIDTH - 12);
  lv_label_set_long_mode(builder_preview_label_, LV_LABEL_LONG_WRAP);
  lv_obj_align(builder_preview_label_, LV_ALIGN_TOP_LEFT, 0, 112);

  lv_obj_t* add_btn = lv_btn_create(activity_builder_modal_);
  lv_obj_set_size(add_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(add_btn, LV_ALIGN_TOP_LEFT, 0, 196);
  lv_obj_add_event_cb(add_btn, on_activity_builder_add_step, LV_EVENT_CLICKED, this);
  lv_obj_t* add_text = lv_label_create(add_btn);
  lv_label_set_text(add_text, "Add Step");
  lv_obj_center(add_text);

  lv_obj_t* clear_btn = lv_btn_create(activity_builder_modal_);
  lv_obj_set_size(clear_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(clear_btn, LV_ALIGN_TOP_MID, 0, 196);
  lv_obj_add_event_cb(clear_btn, on_activity_builder_clear, LV_EVENT_CLICKED, this);
  lv_obj_t* clear_text = lv_label_create(clear_btn);
  lv_label_set_text(clear_text, "Clear");
  lv_obj_center(clear_text);

  lv_obj_t* save_btn = lv_btn_create(activity_builder_modal_);
  lv_obj_set_size(save_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, 0, 196);
  lv_obj_add_event_cb(save_btn, on_activity_builder_save, LV_EVENT_CLICKED, this);
  lv_obj_t* save_text = lv_label_create(save_btn);
  lv_label_set_text(save_text, "Save");
  lv_obj_center(save_text);

  lv_obj_t* close_btn = lv_btn_create(activity_builder_modal_);
  lv_obj_set_size(close_btn, SCR_WIDTH - 12, 30);
  lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_add_event_cb(close_btn, on_activity_builder_close, LV_EVENT_CLICKED, this);
  lv_obj_t* close_text = lv_label_create(close_btn);
  lv_label_set_text(close_text, "Close");
  lv_obj_center(close_text);

  refresh_activity_builder_preview();
}

void SetupUi::close_activity_builder_modal() {
  if (activity_builder_modal_ == nullptr) return;
  lv_obj_del(activity_builder_modal_);
  activity_builder_modal_ = nullptr;
  dd_builder_device_ = nullptr;
  dd_builder_slot_ = nullptr;
  builder_status_label_ = nullptr;
  builder_preview_label_ = nullptr;
  activity_builder_device_ids_.clear();
  activity_builder_actions_.clear();
}

void SetupUi::activity_builder_add_step() {
  if (dd_builder_device_ == nullptr || dd_builder_slot_ == nullptr || builder_status_label_ == nullptr) return;
  if (activity_builder_device_ids_.empty()) {
    lv_label_set_text(builder_status_label_, "No devices available");
    return;
  }

  uint16_t device_idx = lv_dropdown_get_selected(dd_builder_device_);
  if (device_idx >= activity_builder_device_ids_.size()) return;
  char slot_buf[24];
  lv_dropdown_get_selected_str(dd_builder_slot_, slot_buf, sizeof(slot_buf));
  CommandSlot slot;
  if (!command_slot_from_string(slot_buf, &slot)) {
    lv_label_set_text(builder_status_label_, "Invalid slot");
    return;
  }

  ActivityStartupAction action;
  action.device_id = activity_builder_device_ids_[device_idx];
  action.slot = slot;
  activity_builder_actions_.push_back(action);
  lv_label_set_text(builder_status_label_, "Step added");
  refresh_activity_builder_preview();
}

void SetupUi::activity_builder_clear() {
  activity_builder_actions_.clear();
  if (builder_status_label_ != nullptr) lv_label_set_text(builder_status_label_, "All steps cleared");
  refresh_activity_builder_preview();
}

void SetupUi::activity_builder_save() {
  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) return;
  ActivityRecord updated = activities[selected_activity_index_];
  updated.startup_actions = activity_builder_actions_;
  activity_registry_.upsert(updated);
  if (builder_status_label_ != nullptr) {
    lv_label_set_text_fmt(builder_status_label_, "Saved %u startup steps", static_cast<unsigned>(updated.startup_actions.size()));
  }
  rebuild_activity_list();
}

void SetupUi::refresh_activity_builder_preview() {
  if (builder_preview_label_ == nullptr) return;
  if (activity_builder_actions_.empty()) {
    lv_label_set_text(builder_preview_label_, "Startup steps: none");
    return;
  }

  std::stringstream ss;
  ss << "Startup steps:\n";
  for (size_t i = 0; i < activity_builder_actions_.size(); ++i) {
    const ActivityStartupAction& action = activity_builder_actions_[i];
    const DeviceRecord* device = device_registry_.get_by_id(action.device_id);
    ss << (i + 1) << ". "
       << (device != nullptr ? device->name : "Unknown")
       << " -> " << to_string(action.slot);
    if (i + 1 < activity_builder_actions_.size()) ss << "\n";
  }
  lv_label_set_text(builder_preview_label_, ss.str().c_str());
}

void SetupUi::execute_activity_startup_actions(const ActivityRecord& activity) {
  for (const ActivityStartupAction& action : activity.startup_actions) {
    ActivityRecord single_device;
    single_device.device_ids.push_back(action.device_id);
    dispatcher_.dispatch(single_device, action.slot);
    delay(80);
  }
}

void SetupUi::perform_sd_backup() {
  std::string status;
  if (sd_backup_.backup_to_sd(device_registry_.all(), activity_registry_.all(), &status)) {
    if (settings_status_ != nullptr) lv_label_set_text(settings_status_, status.c_str());
    return;
  }
  if (settings_status_ != nullptr) lv_label_set_text(settings_status_, status.empty() ? "SD backup failed" : status.c_str());
}

void SetupUi::open_sd_restore_modal() {
  if (restore_modal_ != nullptr) return;

  std::vector<SdBackupEntry> backups;
  std::string status;
  if (!sd_backup_.list_backups(&backups, &status)) {
    if (settings_status_ != nullptr) lv_label_set_text(settings_status_, status.empty() ? "SD backup list failed" : status.c_str());
    return;
  }
  if (backups.empty()) {
    if (settings_status_ != nullptr) lv_label_set_text(settings_status_, "No backups available on SD");
    return;
  }

  restore_backup_paths_.clear();
  std::string options;
  for (size_t i = 0; i < backups.size(); ++i) {
    if (!options.empty()) options += "\n";
    options += backups[i].label;
    restore_backup_paths_.push_back(backups[i].path);
  }

  restore_modal_ = lv_obj_create(root_);
  lv_obj_set_size(restore_modal_, SCR_WIDTH - 12, 172);
  lv_obj_center(restore_modal_);
  lv_obj_set_style_bg_color(restore_modal_, lv_color_hex(0x202020), LV_PART_MAIN);
  lv_obj_set_style_pad_all(restore_modal_, 6, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(restore_modal_);
  lv_label_set_text(title, "Restore Backup");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  dd_restore_backup_ = lv_dropdown_create(restore_modal_);
  lv_obj_set_width(dd_restore_backup_, SCR_WIDTH - 28);
  lv_dropdown_set_options(dd_restore_backup_, options.c_str());
  lv_obj_align(dd_restore_backup_, LV_ALIGN_TOP_LEFT, 0, 22);

  lv_obj_t* hint = lv_label_create(restore_modal_);
  lv_label_set_text(hint, "Choose backup, then Restore.");
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 62);

  lv_obj_t* close_btn = lv_btn_create(restore_modal_);
  lv_obj_set_size(close_btn, (SCR_WIDTH - 34) / 2, 32);
  lv_obj_align(close_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_add_event_cb(close_btn, on_settings_restore_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* close_text = lv_label_create(close_btn);
  lv_label_set_text(close_text, "Close");
  lv_obj_center(close_text);

  lv_obj_t* restore_btn = lv_btn_create(restore_modal_);
  lv_obj_set_size(restore_btn, (SCR_WIDTH - 34) / 2, 32);
  lv_obj_align(restore_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_event_cb(restore_btn, on_settings_restore_modal_apply, LV_EVENT_CLICKED, this);
  lv_obj_t* restore_text = lv_label_create(restore_btn);
  lv_label_set_text(restore_text, "Restore");
  lv_obj_center(restore_text);
}

void SetupUi::close_sd_restore_modal() {
  if (restore_modal_ == nullptr) return;
  lv_obj_del(restore_modal_);
  restore_modal_ = nullptr;
  dd_restore_backup_ = nullptr;
  restore_backup_paths_.clear();
}

void SetupUi::perform_sd_restore() {
  if (dd_restore_backup_ == nullptr) return;
  const uint16_t selected = lv_dropdown_get_selected(dd_restore_backup_);
  if (selected >= restore_backup_paths_.size()) {
    if (settings_status_ != nullptr) lv_label_set_text(settings_status_, "SD restore failed: invalid selection");
    return;
  }
  const std::string backup_path = restore_backup_paths_[selected];

  std::vector<DeviceRecord> devices;
  std::vector<ActivityRecord> activities;
  std::string status;
  if (!sd_backup_.restore_from_sd(backup_path, &devices, &activities, &status)) {
    if (settings_status_ != nullptr) lv_label_set_text(settings_status_, status.empty() ? "SD restore failed" : status.c_str());
    return;
  }

  const bool normalized = normalize_restored_records(&devices, &activities);

  if (!device_registry_.replace_all(std::move(devices))) {
    if (settings_status_ != nullptr) lv_label_set_text(settings_status_, "SD restore failed: device save");
    return;
  }
  if (!activity_registry_.replace_all(std::move(activities))) {
    if (settings_status_ != nullptr) lv_label_set_text(settings_status_, "SD restore failed: activity save");
    return;
  }

  selected_device_index_ = -1;
  selected_activity_index_ = -1;
  rebuild_device_list();
  rebuild_activity_list();
  rebuild_remote_activity_dropdown();
  refresh_remote_icon_overrides();
  save_ui_context();
  close_sd_restore_modal();
  if (normalized) {
    status += " (normalized)";
  }
  if (settings_status_ != nullptr) lv_label_set_text(settings_status_, status.c_str());
}

void SetupUi::open_power_modal() {
  if (power_modal_ != nullptr) return;

  power_modal_ = lv_obj_create(root_);
  lv_obj_set_size(power_modal_, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_center(power_modal_);
  lv_obj_set_style_bg_color(power_modal_, lv_color_hex(0x202020), LV_PART_MAIN);
  lv_obj_set_style_pad_all(power_modal_, 6, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(power_modal_);
  lv_label_set_text(title, "Power Settings");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  lv_obj_t* timeout_label = lv_label_create(power_modal_);
  lv_label_set_text(timeout_label, "Sleep timeout");
  lv_obj_align(timeout_label, LV_ALIGN_TOP_LEFT, 0, 30);

  dd_power_sleep_timeout_ = lv_dropdown_create(power_modal_);
  lv_obj_set_width(dd_power_sleep_timeout_, SCR_WIDTH - 12);
  lv_obj_align(dd_power_sleep_timeout_, LV_ALIGN_TOP_LEFT, 0, 48);
  lv_dropdown_set_options(dd_power_sleep_timeout_, sleep_timeout_dropdown_options().c_str());
  lv_dropdown_set_selected(dd_power_sleep_timeout_, sleep_timeout_index_for_value(get_sleepTimeout_HAL()));

  lv_obj_t* debounce_label = lv_label_create(power_modal_);
  lv_label_set_text(debounce_label, "Command debounce");
  lv_obj_align(debounce_label, LV_ALIGN_TOP_LEFT, 0, 86);

  dd_power_debounce_ = lv_dropdown_create(power_modal_);
  lv_obj_set_width(dd_power_debounce_, SCR_WIDTH - 12);
  lv_obj_align(dd_power_debounce_, LV_ALIGN_TOP_LEFT, 0, 104);
  lv_dropdown_set_options(dd_power_debounce_, debounce_dropdown_options().c_str());
  lv_dropdown_set_selected(dd_power_debounce_, debounce_index_for_value(dispatcher_.debounce_interval_ms()));

  lv_obj_t* wake_label = lv_label_create(power_modal_);
  lv_label_set_text(wake_label, "Lift to wake (motion)");
  lv_obj_align(wake_label, LV_ALIGN_TOP_LEFT, 0, 142);

  sw_power_wakeup_imu_ = lv_switch_create(power_modal_);
  lv_obj_align(sw_power_wakeup_imu_, LV_ALIGN_TOP_RIGHT, 0, 138);
  if (get_wakeupByIMUEnabled_HAL()) {
    lv_obj_add_state(sw_power_wakeup_imu_, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(sw_power_wakeup_imu_, LV_STATE_CHECKED);
  }

  lv_obj_t* note = lv_label_create(power_modal_);
  lv_obj_set_width(note, SCR_WIDTH - 12);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_label_set_text(note, "Changes are persisted and used after reboot/wake.");
  lv_obj_align(note, LV_ALIGN_TOP_LEFT, 0, 176);

  lv_obj_t* close_btn = lv_btn_create(power_modal_);
  lv_obj_set_size(close_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 0, 206);
  lv_obj_add_event_cb(close_btn, on_power_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* close_text = lv_label_create(close_btn);
  lv_label_set_text(close_text, "Close");
  lv_obj_center(close_text);

  lv_obj_t* apply_btn = lv_btn_create(power_modal_);
  lv_obj_set_size(apply_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(apply_btn, LV_ALIGN_TOP_RIGHT, 0, 206);
  lv_obj_add_event_cb(apply_btn, on_power_modal_apply, LV_EVENT_CLICKED, this);
  lv_obj_t* apply_text = lv_label_create(apply_btn);
  lv_label_set_text(apply_text, "Apply");
  lv_obj_center(apply_text);
}

void SetupUi::close_power_modal() {
  if (power_modal_ == nullptr) return;
  lv_obj_del(power_modal_);
  power_modal_ = nullptr;
  dd_power_sleep_timeout_ = nullptr;
  dd_power_debounce_ = nullptr;
  sw_power_wakeup_imu_ = nullptr;
}

bool SetupUi::apply_power_modal() {
  if (dd_power_sleep_timeout_ == nullptr || dd_power_debounce_ == nullptr || sw_power_wakeup_imu_ == nullptr) return false;
  const uint16_t timeout_index = lv_dropdown_get_selected(dd_power_sleep_timeout_);
  const uint32_t timeout_ms = sleep_timeout_value_for_index(timeout_index);
  const uint16_t debounce_index = lv_dropdown_get_selected(dd_power_debounce_);
  const unsigned long debounce_ms = debounce_value_for_index(debounce_index);
  const bool wake_by_imu = lv_obj_has_state(sw_power_wakeup_imu_, LV_STATE_CHECKED);

  set_sleepTimeout_HAL(timeout_ms);
  set_wakeupByIMUEnabled_HAL(wake_by_imu);
  dispatcher_.set_debounce_interval_ms(debounce_ms);
  const bool debounce_saved = save_debounce_interval_setting(debounce_ms);
  save_preferences_HAL();
  setLastActivityTimestamp_HAL();
  update_status_bar();

  if (settings_status_ != nullptr) {
    lv_label_set_text_fmt(settings_status_, "Power updated: %lu sec timeout, debounce %lums, lift-to-wake %s%s",
                          static_cast<unsigned long>(timeout_ms / 1000),
                          debounce_ms,
                          wake_by_imu ? "on" : "off",
                          debounce_saved ? "" : " (debounce save failed)");
  }
  return true;
}

bool SetupUi::remove_device_references_from_activities(uint32_t device_id) {
  const std::vector<ActivityRecord>& source = activity_registry_.all();
  std::vector<ActivityRecord> updated = source;
  bool changed = false;

  for (ActivityRecord& activity : updated) {
    const size_t before_devices = activity.device_ids.size();
    activity.device_ids.erase(
        std::remove(activity.device_ids.begin(), activity.device_ids.end(), device_id),
        activity.device_ids.end());
    if (activity.device_ids.size() != before_devices) changed = true;

    const size_t before_bindings = activity.key_bindings.size();
    activity.key_bindings.erase(
        std::remove_if(activity.key_bindings.begin(), activity.key_bindings.end(),
                       [&](const ActivityKeyBinding& binding) { return binding.device_id == device_id; }),
        activity.key_bindings.end());
    if (activity.key_bindings.size() != before_bindings) changed = true;

    const size_t before_actions = activity.startup_actions.size();
    activity.startup_actions.erase(
        std::remove_if(activity.startup_actions.begin(), activity.startup_actions.end(),
                       [&](const ActivityStartupAction& action) { return action.device_id == device_id; }),
        activity.startup_actions.end());
    if (activity.startup_actions.size() != before_actions) changed = true;
  }

  return changed ? activity_registry_.replace_all(std::move(updated)) : true;
}

bool SetupUi::remove_command_references_from_activities(uint32_t device_id, const std::string& command_name) {
  const std::vector<ActivityRecord>& source = activity_registry_.all();
  std::vector<ActivityRecord> updated = source;
  bool changed = false;

  for (ActivityRecord& activity : updated) {
    const size_t before_bindings = activity.key_bindings.size();
    activity.key_bindings.erase(
        std::remove_if(activity.key_bindings.begin(), activity.key_bindings.end(),
                       [&](const ActivityKeyBinding& binding) {
                         return binding.device_id == device_id && binding.command_name == command_name;
                       }),
        activity.key_bindings.end());
    if (activity.key_bindings.size() != before_bindings) changed = true;
  }

  return changed ? activity_registry_.replace_all(std::move(updated)) : true;
}

bool SetupUi::normalize_restored_records(std::vector<DeviceRecord>* devices, std::vector<ActivityRecord>* activities) const {
  if (devices == nullptr || activities == nullptr) return false;
  bool changed = false;

  std::unordered_set<uint32_t> used_device_ids;
  std::unordered_map<uint32_t, uint32_t> device_id_remap;
  uint32_t next_device_id = 1;
  for (DeviceRecord& device : *devices) {
    if (device.name.empty()) {
      device.name = "Restored Device";
      changed = true;
    }

    const uint32_t original_id = device.id;
    uint32_t normalized_id = original_id;
    if (normalized_id == 0 || used_device_ids.find(normalized_id) != used_device_ids.end()) {
      while (used_device_ids.find(next_device_id) != used_device_ids.end()) ++next_device_id;
      normalized_id = next_device_id++;
      changed = true;
    }
    used_device_ids.insert(normalized_id);
    if (original_id != 0 && device_id_remap.find(original_id) == device_id_remap.end()) {
      device_id_remap[original_id] = normalized_id;
    }
    device.id = normalized_id;
  }

  std::unordered_map<uint32_t, std::unordered_set<std::string>> command_index;
  for (const DeviceRecord& device : *devices) {
    std::unordered_set<std::string>& commands = command_index[device.id];
    for (const DeviceCommand& command : device.commands) {
      if (!command.name.empty()) commands.insert(command.name);
    }
  }

  std::unordered_set<uint32_t> used_activity_ids;
  uint32_t next_activity_id = 1;
  for (ActivityRecord& activity : *activities) {
    if (activity.name.empty()) {
      activity.name = "Restored Activity";
      changed = true;
    }

    const uint32_t original_id = activity.id;
    uint32_t normalized_id = original_id;
    if (normalized_id == 0 || used_activity_ids.find(normalized_id) != used_activity_ids.end()) {
      while (used_activity_ids.find(next_activity_id) != used_activity_ids.end()) ++next_activity_id;
      normalized_id = next_activity_id++;
      changed = true;
    }
    used_activity_ids.insert(normalized_id);
    activity.id = normalized_id;

    std::vector<uint32_t> normalized_device_ids;
    std::unordered_set<uint32_t> seen_activity_device_ids;
    for (uint32_t id : activity.device_ids) {
      auto mapped_it = device_id_remap.find(id);
      if (mapped_it != device_id_remap.end()) id = mapped_it->second;
      if (used_device_ids.find(id) == used_device_ids.end()) {
        changed = true;
        continue;
      }
      if (seen_activity_device_ids.insert(id).second) {
        normalized_device_ids.push_back(id);
      } else {
        changed = true;
      }
    }
    activity.device_ids = std::move(normalized_device_ids);

    std::map<int, ActivityKeyBinding> latest_binding_by_key;
    for (const ActivityKeyBinding& binding : activity.key_bindings) {
      if (binding.command_name.empty()) {
        changed = true;
        continue;
      }
      uint32_t normalized_device = binding.device_id;
      auto mapped_it = device_id_remap.find(normalized_device);
      if (mapped_it != device_id_remap.end()) normalized_device = mapped_it->second;
      if (used_device_ids.find(normalized_device) == used_device_ids.end()) {
        changed = true;
        continue;
      }
      auto command_it = command_index.find(normalized_device);
      if (command_it == command_index.end() || command_it->second.find(binding.command_name) == command_it->second.end()) {
        changed = true;
        continue;
      }

      ActivityKeyBinding normalized_binding = binding;
      normalized_binding.device_id = normalized_device;
      latest_binding_by_key[static_cast<unsigned char>(normalized_binding.key_char)] = normalized_binding;
    }
    std::vector<ActivityKeyBinding> normalized_bindings;
    normalized_bindings.reserve(latest_binding_by_key.size());
    for (const auto& kv : latest_binding_by_key) {
      normalized_bindings.push_back(kv.second);
    }
    if (normalized_bindings.size() != activity.key_bindings.size()) changed = true;
    activity.key_bindings = std::move(normalized_bindings);

    std::vector<ActivityStartupAction> normalized_actions;
    normalized_actions.reserve(activity.startup_actions.size());
    for (const ActivityStartupAction& action : activity.startup_actions) {
      uint32_t normalized_device = action.device_id;
      auto mapped_it = device_id_remap.find(normalized_device);
      if (mapped_it != device_id_remap.end()) normalized_device = mapped_it->second;
      if (used_device_ids.find(normalized_device) == used_device_ids.end()) {
        changed = true;
        continue;
      }
      const int slot_value = static_cast<int>(action.slot);
      if (slot_value < 0 || slot_value >= static_cast<int>(CommandSlot::Count)) {
        changed = true;
        continue;
      }
      ActivityStartupAction normalized_action = action;
      normalized_action.device_id = normalized_device;
      normalized_actions.push_back(normalized_action);
    }
    if (normalized_actions.size() != activity.startup_actions.size()) changed = true;
    activity.startup_actions = std::move(normalized_actions);
  }

  return changed;
}

void SetupUi::open_manual_time_modal() {
  if (time_modal_ != nullptr) return;

  const std::string timezone_value = load_timezone_setting();
  apply_timezone_setting(timezone_value);

  time_modal_ = lv_obj_create(root_);
  lv_obj_set_size(time_modal_, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_center(time_modal_);
  lv_obj_set_style_bg_color(time_modal_, lv_color_hex(0x202020), LV_PART_MAIN);
  lv_obj_set_style_pad_all(time_modal_, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(time_modal_, LV_DIR_VER);
  lv_obj_set_style_pad_bottom(time_modal_, 148, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(time_modal_);
  lv_label_set_text(title, "Set Time");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  lv_obj_t* hint = lv_label_create(time_modal_);
  lv_label_set_text(hint, "Format: YYYY-MM-DD HH:MM:SS");
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 28);

  ta_manual_time_ = lv_textarea_create(time_modal_);
  lv_textarea_set_one_line(ta_manual_time_, true);
  lv_obj_set_width(ta_manual_time_, SCR_WIDTH - 12);
  lv_obj_align(ta_manual_time_, LV_ALIGN_TOP_LEFT, 0, 52);
  lv_obj_add_event_cb(ta_manual_time_, on_textarea_focus, LV_EVENT_FOCUSED, this);
  manual_time_initial_text_ = current_time_full_text();
  lv_textarea_set_text(ta_manual_time_, manual_time_initial_text_.c_str());

  lv_obj_t* tz_label = lv_label_create(time_modal_);
  lv_label_set_text(tz_label, "Timezone");
  lv_obj_align(tz_label, LV_ALIGN_TOP_LEFT, 0, 84);

  dd_manual_timezone_ = lv_dropdown_create(time_modal_);
  lv_obj_set_width(dd_manual_timezone_, SCR_WIDTH - 12);
  lv_dropdown_set_options(dd_manual_timezone_, timezone_dropdown_options().c_str());
  lv_obj_align(dd_manual_timezone_, LV_ALIGN_TOP_LEFT, 0, 102);
  lv_dropdown_set_selected(dd_manual_timezone_, timezone_index_for_value(timezone_value));

  lv_obj_t* close_btn = lv_btn_create(time_modal_);
  lv_obj_set_size(close_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 0, 140);
  lv_obj_add_event_cb(close_btn, on_settings_time_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* close_text = lv_label_create(close_btn);
  lv_label_set_text(close_text, "Close");
  lv_obj_center(close_text);

  lv_obj_t* apply_btn = lv_btn_create(time_modal_);
  lv_obj_set_size(apply_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(apply_btn, LV_ALIGN_TOP_RIGHT, 0, 140);
  lv_obj_add_event_cb(apply_btn, on_settings_time_modal_apply, LV_EVENT_CLICKED, this);
  lv_obj_t* apply_text = lv_label_create(apply_btn);
  lv_label_set_text(apply_text, "Apply");
  lv_obj_center(apply_text);

  keyboard_ = lv_keyboard_create(root_);
  lv_obj_set_size(keyboard_, SCR_WIDTH, 128);
  lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard_, on_keyboard_apply, LV_EVENT_READY, this);
}

void SetupUi::close_manual_time_modal() {
  if (time_modal_ == nullptr) return;
  lv_obj_del(time_modal_);
  time_modal_ = nullptr;
  ta_manual_time_ = nullptr;
  dd_manual_timezone_ = nullptr;
  manual_time_initial_text_.clear();
  if (keyboard_ != nullptr) {
    lv_obj_del(keyboard_);
    keyboard_ = nullptr;
  }
}

bool SetupUi::apply_manual_time_modal() {
  if (ta_manual_time_ == nullptr || dd_manual_timezone_ == nullptr) return false;

  const uint16_t tz_index = lv_dropdown_get_selected(dd_manual_timezone_);
  const std::string timezone_value = timezone_value_for_index(tz_index);
  apply_timezone_setting(timezone_value);
  if (!save_timezone_setting(timezone_value)) {
    if (settings_status_ != nullptr) lv_label_set_text(settings_status_, "Timezone save failed");
    return false;
  }

  const std::string raw = trim_copy(lv_textarea_get_text(ta_manual_time_));
  const bool time_changed = raw != manual_time_initial_text_;
  if (!time_changed) {
    update_status_bar();
    if (settings_status_ != nullptr) {
      lv_label_set_text_fmt(settings_status_, "Timezone set: %s", timezone_label_for_index(tz_index));
    }
    return true;
  }

  struct tm parsed {};
  if (!parse_manual_datetime(raw, &parsed)) {
    if (settings_status_ != nullptr) lv_label_set_text(settings_status_, "Manual time failed: invalid format");
    return false;
  }
  time_t epoch = mktime(&parsed);
  if (epoch <= 0) {
    if (settings_status_ != nullptr) lv_label_set_text(settings_status_, "Manual time failed: invalid date/time");
    return false;
  }
  struct timeval tv {};
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) != 0) {
    if (settings_status_ != nullptr) lv_label_set_text(settings_status_, "Manual time failed: settimeofday");
    return false;
  }
  update_status_bar();
  if (settings_status_ != nullptr) {
    const std::string status = "Time/timezone set: " + raw;
    lv_label_set_text(settings_status_, status.c_str());
  }
  return true;
}

void SetupUi::open_wifi_modal() {
  if (wifi_modal_ != nullptr) return;

  wifi_modal_ = lv_obj_create(root_);
  lv_obj_set_size(wifi_modal_, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_center(wifi_modal_);
  lv_obj_set_style_bg_color(wifi_modal_, lv_color_hex(0x202020), LV_PART_MAIN);
  lv_obj_set_style_pad_all(wifi_modal_, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(wifi_modal_, LV_DIR_VER);
  lv_obj_set_style_pad_bottom(wifi_modal_, 148, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(wifi_modal_);
  lv_label_set_text(title, "WiFi Settings");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  lv_obj_t* network_label = lv_label_create(wifi_modal_);
  lv_label_set_text(network_label, "Network");
  lv_obj_align(network_label, LV_ALIGN_TOP_LEFT, 0, 28);

  dd_wifi_network_ = lv_dropdown_create(wifi_modal_);
  lv_obj_set_width(dd_wifi_network_, SCR_WIDTH - 12);
  lv_obj_align(dd_wifi_network_, LV_ALIGN_TOP_LEFT, 0, 46);
  lv_dropdown_set_options(dd_wifi_network_, "Scanning...");

  lv_obj_t* password_label = lv_label_create(wifi_modal_);
  lv_label_set_text(password_label, "Password");
  lv_obj_align(password_label, LV_ALIGN_TOP_LEFT, 0, 84);

  ta_wifi_password_ = lv_textarea_create(wifi_modal_);
  lv_textarea_set_one_line(ta_wifi_password_, true);
  lv_obj_set_width(ta_wifi_password_, SCR_WIDTH - 12);
  lv_textarea_set_placeholder_text(ta_wifi_password_, "WiFi password");
  lv_obj_align(ta_wifi_password_, LV_ALIGN_TOP_LEFT, 0, 102);
  lv_obj_add_event_cb(ta_wifi_password_, on_textarea_focus, LV_EVENT_FOCUSED, this);

  lv_obj_t* scan_btn = lv_btn_create(wifi_modal_);
  lv_obj_set_size(scan_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(scan_btn, LV_ALIGN_TOP_LEFT, 0, 138);
  lv_obj_add_event_cb(scan_btn, on_wifi_modal_scan, LV_EVENT_CLICKED, this);
  lv_obj_t* scan_text = lv_label_create(scan_btn);
  lv_label_set_text(scan_text, "Scan");
  lv_obj_center(scan_text);

  lv_obj_t* connect_btn = lv_btn_create(wifi_modal_);
  lv_obj_set_size(connect_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(connect_btn, LV_ALIGN_TOP_MID, 0, 138);
  lv_obj_add_event_cb(connect_btn, on_wifi_modal_connect, LV_EVENT_CLICKED, this);
  lv_obj_t* connect_text = lv_label_create(connect_btn);
  lv_label_set_text(connect_text, "Connect");
  lv_obj_center(connect_text);

  lv_obj_t* forget_btn = lv_btn_create(wifi_modal_);
  lv_obj_set_size(forget_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(forget_btn, LV_ALIGN_TOP_RIGHT, 0, 138);
  lv_obj_add_event_cb(forget_btn, on_wifi_modal_forget, LV_EVENT_CLICKED, this);
  lv_obj_t* forget_text = lv_label_create(forget_btn);
  lv_label_set_text(forget_text, "Forget");
  lv_obj_center(forget_text);

  lv_obj_t* close_btn = lv_btn_create(wifi_modal_);
  lv_obj_set_size(close_btn, SCR_WIDTH - 12, 32);
  lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 0, 176);
  lv_obj_add_event_cb(close_btn, on_wifi_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* close_text = lv_label_create(close_btn);
  lv_label_set_text(close_text, "Close");
  lv_obj_center(close_text);

  wifi_modal_status_ = lv_label_create(wifi_modal_);
  lv_obj_set_width(wifi_modal_status_, SCR_WIDTH - 12);
  lv_label_set_long_mode(wifi_modal_status_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(wifi_modal_status_, "Scan and connect to a network.");
  lv_obj_align(wifi_modal_status_, LV_ALIGN_TOP_LEFT, 0, 214);

  keyboard_ = lv_keyboard_create(root_);
  lv_obj_set_size(keyboard_, SCR_WIDTH, 128);
  lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard_, on_keyboard_apply, LV_EVENT_READY, this);

  refresh_wifi_scan_list();

  std::string saved_ssid;
  std::string saved_password;
#if (ENABLE_WIFI_AND_MQTT == 1)
  if (wifi_get_saved_credentials_HAL(&saved_ssid, &saved_password)) {
    lv_textarea_set_text(ta_wifi_password_, saved_password.c_str());
    if (wifi_modal_status_ != nullptr) {
      lv_label_set_text_fmt(wifi_modal_status_, "Saved network: %s", saved_ssid.c_str());
    }
  }
#else
  if (wifi_modal_status_ != nullptr) {
    lv_label_set_text(wifi_modal_status_, "WiFi is disabled in build flags");
  }
#endif
}

void SetupUi::close_wifi_modal() {
  if (wifi_modal_ == nullptr) return;
  lv_obj_del(wifi_modal_);
  wifi_modal_ = nullptr;
  dd_wifi_network_ = nullptr;
  ta_wifi_password_ = nullptr;
  wifi_modal_status_ = nullptr;
  wifi_scan_results_.clear();
  if (keyboard_ != nullptr) {
    lv_obj_del(keyboard_);
    keyboard_ = nullptr;
  }
}

void SetupUi::refresh_wifi_scan_list() {
  if (dd_wifi_network_ == nullptr) return;

#if (ENABLE_WIFI_AND_MQTT == 1)
  std::vector<std::string> scanned;
  if (!wifi_scan_networks_HAL(&scanned)) {
    if (wifi_modal_status_ != nullptr) {
      lv_label_set_text(wifi_modal_status_, "WiFi scan failed");
    }
    lv_dropdown_set_options(dd_wifi_network_, "No networks");
    wifi_scan_results_.clear();
    return;
  }

  std::string saved_ssid;
  std::string saved_password;
  if (wifi_get_saved_credentials_HAL(&saved_ssid, &saved_password) && !saved_ssid.empty()) {
    if (std::find(scanned.begin(), scanned.end(), saved_ssid) == scanned.end()) {
      scanned.insert(scanned.begin(), saved_ssid);
    }
  }

  wifi_scan_results_ = scanned;
  std::string options;
  for (size_t i = 0; i < wifi_scan_results_.size(); ++i) {
    if (!options.empty()) options += "\n";
    options += wifi_scan_results_[i];
  }
  if (options.empty()) {
    options = "No networks";
  }
  lv_dropdown_set_options(dd_wifi_network_, options.c_str());

  if (!saved_ssid.empty()) {
    for (uint16_t i = 0; i < wifi_scan_results_.size(); ++i) {
      if (wifi_scan_results_[i] == saved_ssid) {
        lv_dropdown_set_selected(dd_wifi_network_, i);
        break;
      }
    }
  }

  if (wifi_modal_status_ != nullptr) {
    lv_label_set_text_fmt(wifi_modal_status_, "Found %u network(s)", static_cast<unsigned>(wifi_scan_results_.size()));
  }
#else
  lv_dropdown_set_options(dd_wifi_network_, "WiFi disabled");
  wifi_scan_results_.clear();
  if (wifi_modal_status_ != nullptr) {
    lv_label_set_text(wifi_modal_status_, "WiFi is disabled in build flags");
  }
#endif
}

bool SetupUi::connect_wifi_from_modal() {
  if (dd_wifi_network_ == nullptr || ta_wifi_password_ == nullptr) return false;

#if (ENABLE_WIFI_AND_MQTT == 1)
  char ssid_buf[80];
  lv_dropdown_get_selected_str(dd_wifi_network_, ssid_buf, sizeof(ssid_buf));
  const std::string ssid = trim_copy(ssid_buf);
  if (ssid.empty() || ssid == "No networks") {
    if (wifi_modal_status_ != nullptr) lv_label_set_text(wifi_modal_status_, "Pick a WiFi network first");
    return false;
  }

  const std::string password = lv_textarea_get_text(ta_wifi_password_);
  if (!wifi_set_credentials_HAL(ssid, password)) {
    if (wifi_modal_status_ != nullptr) lv_label_set_text(wifi_modal_status_, "Failed to save/connect WiFi");
    if (settings_status_ != nullptr) lv_label_set_text(settings_status_, "WiFi connect failed");
    wifi_connect_attempt_active_ = false;
    return false;
  }

  if (wifi_modal_status_ != nullptr) {
    lv_label_set_text_fmt(wifi_modal_status_, "Connecting to %s...", ssid.c_str());
  }
  if (settings_status_ != nullptr) {
    lv_label_set_text_fmt(settings_status_, "WiFi connecting: %s", ssid.c_str());
  }
  wifi_connect_attempt_active_ = true;
  wifi_connect_attempt_started_ms_ = millis();
  wifi_connect_target_ssid_ = ssid;
  update_status_bar();
  return true;
#else
  if (wifi_modal_status_ != nullptr) lv_label_set_text(wifi_modal_status_, "WiFi is disabled in build flags");
  return false;
#endif
}

void SetupUi::forget_wifi_credentials() {
#if (ENABLE_WIFI_AND_MQTT == 1)
  if (!wifi_clear_saved_credentials_HAL()) {
    if (wifi_modal_status_ != nullptr) lv_label_set_text(wifi_modal_status_, "Failed to forget WiFi credentials");
    return;
  }
  if (ta_wifi_password_ != nullptr) lv_textarea_set_text(ta_wifi_password_, "");
  if (wifi_modal_status_ != nullptr) lv_label_set_text(wifi_modal_status_, "Saved WiFi credentials removed");
  if (settings_status_ != nullptr) lv_label_set_text(settings_status_, "WiFi credentials removed");
  wifi_connect_attempt_active_ = false;
  wifi_connect_target_ssid_.clear();
  update_status_bar();
#else
  if (wifi_modal_status_ != nullptr) lv_label_set_text(wifi_modal_status_, "WiFi is disabled in build flags");
#endif
}

void SetupUi::open_ble_modal() {
  if (ble_modal_ != nullptr) return;

  ble_modal_ = lv_obj_create(root_);
  lv_obj_set_size(ble_modal_, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_center(ble_modal_);
  lv_obj_set_style_bg_color(ble_modal_, lv_color_hex(0x202020), LV_PART_MAIN);
  lv_obj_set_style_pad_all(ble_modal_, 6, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(ble_modal_);
  lv_label_set_text(title, "BLE Settings");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  const int bw = (SCR_WIDTH - 18) / 2;
  const int bh = 32;

  lv_obj_t* advertise_btn = lv_btn_create(ble_modal_);
  lv_obj_set_size(advertise_btn, bw, bh);
  lv_obj_align(advertise_btn, LV_ALIGN_TOP_LEFT, 0, 32);
  lv_obj_add_event_cb(advertise_btn, on_ble_modal_advertise, LV_EVENT_CLICKED, this);
  lv_obj_t* advertise_text = lv_label_create(advertise_btn);
  lv_label_set_text(advertise_text, "Advertise");
  lv_obj_center(advertise_text);

  lv_obj_t* stop_btn = lv_btn_create(ble_modal_);
  lv_obj_set_size(stop_btn, bw, bh);
  lv_obj_align(stop_btn, LV_ALIGN_TOP_RIGHT, 0, 32);
  lv_obj_add_event_cb(stop_btn, on_ble_modal_stop, LV_EVENT_CLICKED, this);
  lv_obj_t* stop_text = lv_label_create(stop_btn);
  lv_label_set_text(stop_text, "Stop");
  lv_obj_center(stop_text);

  lv_obj_t* disconnect_btn = lv_btn_create(ble_modal_);
  lv_obj_set_size(disconnect_btn, bw, bh);
  lv_obj_align(disconnect_btn, LV_ALIGN_TOP_LEFT, 0, 68);
  lv_obj_add_event_cb(disconnect_btn, on_ble_modal_disconnect, LV_EVENT_CLICKED, this);
  lv_obj_t* disconnect_text = lv_label_create(disconnect_btn);
  lv_label_set_text(disconnect_text, "Disconnect");
  lv_obj_center(disconnect_text);

  lv_obj_t* list_bonds_btn = lv_btn_create(ble_modal_);
  lv_obj_set_size(list_bonds_btn, bw, bh);
  lv_obj_align(list_bonds_btn, LV_ALIGN_TOP_RIGHT, 0, 68);
  lv_obj_add_event_cb(list_bonds_btn, on_ble_modal_list_bonds, LV_EVENT_CLICKED, this);
  lv_obj_t* list_bonds_text = lv_label_create(list_bonds_btn);
  lv_label_set_text(list_bonds_text, "List Bonds");
  lv_obj_center(list_bonds_text);

  lv_obj_t* clear_bonds_btn = lv_btn_create(ble_modal_);
  lv_obj_set_size(clear_bonds_btn, SCR_WIDTH - 12, bh);
  lv_obj_align(clear_bonds_btn, LV_ALIGN_TOP_LEFT, 0, 104);
  lv_obj_add_event_cb(clear_bonds_btn, on_ble_modal_clear_bonds, LV_EVENT_CLICKED, this);
  lv_obj_t* clear_bonds_text = lv_label_create(clear_bonds_btn);
  lv_label_set_text(clear_bonds_text, "Clear Bonds");
  lv_obj_center(clear_bonds_text);

  lv_obj_t* close_btn = lv_btn_create(ble_modal_);
  lv_obj_set_size(close_btn, SCR_WIDTH - 12, bh);
  lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 0, 140);
  lv_obj_add_event_cb(close_btn, on_ble_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* close_text = lv_label_create(close_btn);
  lv_label_set_text(close_text, "Close");
  lv_obj_center(close_text);

  ble_modal_status_ = lv_label_create(ble_modal_);
  lv_obj_set_width(ble_modal_status_, SCR_WIDTH - 12);
  lv_label_set_long_mode(ble_modal_status_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(ble_modal_status_, "BLE status");
  lv_obj_align(ble_modal_status_, LV_ALIGN_TOP_LEFT, 0, 178);

  refresh_ble_modal_status();
}

void SetupUi::close_ble_modal() {
  if (ble_modal_ == nullptr) return;
  lv_obj_del(ble_modal_);
  ble_modal_ = nullptr;
  ble_modal_status_ = nullptr;
}

void SetupUi::refresh_ble_modal_status() {
  if (ble_modal_status_ == nullptr) return;
#if (ENABLE_KEYBOARD_BLE == 1)
  const bool connected = keyboardBLE_isConnected_HAL();
  const bool advertising = keyboardBLE_isAdvertising_HAL();
  if (connected) {
    lv_label_set_text(ble_modal_status_, "BLE connected");
  } else if (advertising) {
    lv_label_set_text(ble_modal_status_, "BLE advertising");
  } else {
    lv_label_set_text(ble_modal_status_, "BLE idle");
  }
#else
  lv_label_set_text(ble_modal_status_, "BLE not enabled in this firmware. Build/flash omote-v2-esp32-s3.");
#endif
}

void SetupUi::open_mqtt_modal() {
  if (mqtt_modal_ != nullptr) return;

  mqtt_modal_ = lv_obj_create(root_);
  lv_obj_set_size(mqtt_modal_, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_center(mqtt_modal_);
  lv_obj_set_style_bg_color(mqtt_modal_, lv_color_hex(0x202020), LV_PART_MAIN);
  lv_obj_set_style_pad_all(mqtt_modal_, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(mqtt_modal_, LV_DIR_VER);
  lv_obj_set_style_pad_bottom(mqtt_modal_, 148, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(mqtt_modal_);
  lv_label_set_text(title, "MQTT Settings");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  ta_mqtt_host_ = lv_textarea_create(mqtt_modal_);
  lv_textarea_set_one_line(ta_mqtt_host_, true);
  lv_textarea_set_placeholder_text(ta_mqtt_host_, "Broker host/IP");
  lv_obj_set_width(ta_mqtt_host_, SCR_WIDTH - 12);
  lv_obj_align(ta_mqtt_host_, LV_ALIGN_TOP_LEFT, 0, 28);
  lv_obj_add_event_cb(ta_mqtt_host_, on_textarea_focus, LV_EVENT_FOCUSED, this);

  ta_mqtt_port_ = lv_textarea_create(mqtt_modal_);
  lv_textarea_set_one_line(ta_mqtt_port_, true);
  lv_textarea_set_placeholder_text(ta_mqtt_port_, "Port (e.g. 1883)");
  lv_obj_set_width(ta_mqtt_port_, SCR_WIDTH - 12);
  lv_obj_align(ta_mqtt_port_, LV_ALIGN_TOP_LEFT, 0, 58);
  lv_obj_add_event_cb(ta_mqtt_port_, on_textarea_focus, LV_EVENT_FOCUSED, this);

  ta_mqtt_user_ = lv_textarea_create(mqtt_modal_);
  lv_textarea_set_one_line(ta_mqtt_user_, true);
  lv_textarea_set_placeholder_text(ta_mqtt_user_, "Username (optional)");
  lv_obj_set_width(ta_mqtt_user_, SCR_WIDTH - 12);
  lv_obj_align(ta_mqtt_user_, LV_ALIGN_TOP_LEFT, 0, 88);
  lv_obj_add_event_cb(ta_mqtt_user_, on_textarea_focus, LV_EVENT_FOCUSED, this);

  ta_mqtt_pass_ = lv_textarea_create(mqtt_modal_);
  lv_textarea_set_one_line(ta_mqtt_pass_, true);
  lv_textarea_set_password_mode(ta_mqtt_pass_, true);
  lv_textarea_set_placeholder_text(ta_mqtt_pass_, "Password (optional)");
  lv_obj_set_width(ta_mqtt_pass_, SCR_WIDTH - 12);
  lv_obj_align(ta_mqtt_pass_, LV_ALIGN_TOP_LEFT, 0, 118);
  lv_obj_add_event_cb(ta_mqtt_pass_, on_textarea_focus, LV_EVENT_FOCUSED, this);

  ta_mqtt_client_ = lv_textarea_create(mqtt_modal_);
  lv_textarea_set_one_line(ta_mqtt_client_, true);
  lv_textarea_set_placeholder_text(ta_mqtt_client_, "Client name (optional)");
  lv_obj_set_width(ta_mqtt_client_, SCR_WIDTH - 12);
  lv_obj_align(ta_mqtt_client_, LV_ALIGN_TOP_LEFT, 0, 148);
  lv_obj_add_event_cb(ta_mqtt_client_, on_textarea_focus, LV_EVENT_FOCUSED, this);

  lv_obj_t* close_btn = lv_btn_create(mqtt_modal_);
  lv_obj_set_size(close_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 0, 182);
  lv_obj_add_event_cb(close_btn, on_mqtt_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* close_text = lv_label_create(close_btn);
  lv_label_set_text(close_text, "Close");
  lv_obj_center(close_text);

  lv_obj_t* save_btn = lv_btn_create(mqtt_modal_);
  lv_obj_set_size(save_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(save_btn, LV_ALIGN_TOP_MID, 0, 182);
  lv_obj_add_event_cb(save_btn, on_mqtt_modal_save, LV_EVENT_CLICKED, this);
  lv_obj_t* save_text = lv_label_create(save_btn);
  lv_label_set_text(save_text, "Save");
  lv_obj_center(save_text);

  lv_obj_t* clear_btn = lv_btn_create(mqtt_modal_);
  lv_obj_set_size(clear_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(clear_btn, LV_ALIGN_TOP_RIGHT, 0, 182);
  lv_obj_add_event_cb(clear_btn, on_mqtt_modal_clear, LV_EVENT_CLICKED, this);
  lv_obj_t* clear_text = lv_label_create(clear_btn);
  lv_label_set_text(clear_text, "Clear");
  lv_obj_center(clear_text);

  mqtt_modal_status_ = lv_label_create(mqtt_modal_);
  lv_obj_set_width(mqtt_modal_status_, SCR_WIDTH - 12);
  lv_label_set_long_mode(mqtt_modal_status_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(mqtt_modal_status_, "Configure broker and save.");
  lv_obj_align(mqtt_modal_status_, LV_ALIGN_TOP_LEFT, 0, 220);

  std::string host;
  uint16_t port = 0;
  std::string user;
  std::string pass;
  std::string client_name;
  if (mqtt_get_broker_config_HAL(&host, &port, &user, &pass, &client_name)) {
    lv_textarea_set_text(ta_mqtt_host_, host.c_str());
    if (port > 0) {
      char port_buf[16];
      snprintf(port_buf, sizeof(port_buf), "%u", static_cast<unsigned>(port));
      lv_textarea_set_text(ta_mqtt_port_, port_buf);
    }
    lv_textarea_set_text(ta_mqtt_user_, user.c_str());
    lv_textarea_set_text(ta_mqtt_pass_, pass.c_str());
    lv_textarea_set_text(ta_mqtt_client_, client_name.c_str());
  }
  if (mqtt_modal_status_ != nullptr) {
    lv_label_set_text(mqtt_modal_status_,
                      mqtt_is_configured_HAL() ? (mqtt_is_connected_HAL() ? "MQTT connected" : "MQTT configured")
                                               : "MQTT not configured");
  }

  keyboard_ = lv_keyboard_create(root_);
  lv_obj_set_size(keyboard_, SCR_WIDTH, 128);
  lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard_, on_keyboard_apply, LV_EVENT_READY, this);
}

void SetupUi::close_mqtt_modal() {
  if (mqtt_modal_ == nullptr) return;
  lv_obj_del(mqtt_modal_);
  mqtt_modal_ = nullptr;
  ta_mqtt_host_ = nullptr;
  ta_mqtt_port_ = nullptr;
  ta_mqtt_user_ = nullptr;
  ta_mqtt_pass_ = nullptr;
  ta_mqtt_client_ = nullptr;
  mqtt_modal_status_ = nullptr;
  if (keyboard_ != nullptr) {
    lv_obj_del(keyboard_);
    keyboard_ = nullptr;
  }
}

bool SetupUi::apply_mqtt_modal() {
  if (ta_mqtt_host_ == nullptr || ta_mqtt_port_ == nullptr || ta_mqtt_user_ == nullptr ||
      ta_mqtt_pass_ == nullptr || ta_mqtt_client_ == nullptr) {
    return false;
  }
  const std::string host = trim_copy(lv_textarea_get_text(ta_mqtt_host_));
  const std::string port_text = trim_copy(lv_textarea_get_text(ta_mqtt_port_));
  const std::string user = lv_textarea_get_text(ta_mqtt_user_);
  const std::string pass = lv_textarea_get_text(ta_mqtt_pass_);
  const std::string client_name = trim_copy(lv_textarea_get_text(ta_mqtt_client_));

  if (host.empty()) {
    if (mqtt_modal_status_ != nullptr) lv_label_set_text(mqtt_modal_status_, "Host is required");
    return false;
  }
  if (!parse_u32_token(port_text)) {
    if (mqtt_modal_status_ != nullptr) lv_label_set_text(mqtt_modal_status_, "Port must be numeric");
    return false;
  }
  const uint32_t parsed_port = strtoul(port_text.c_str(), nullptr, 10);
  if (parsed_port == 0 || parsed_port > 65535) {
    if (mqtt_modal_status_ != nullptr) lv_label_set_text(mqtt_modal_status_, "Port must be 1-65535");
    return false;
  }

  if (!mqtt_set_broker_config_HAL(host, static_cast<uint16_t>(parsed_port), user, pass, client_name)) {
    if (mqtt_modal_status_ != nullptr) lv_label_set_text(mqtt_modal_status_, "Failed to save MQTT settings");
    return false;
  }

  mqtt_loop_HAL();
  const bool connected = mqtt_is_connected_HAL();
  if (mqtt_modal_status_ != nullptr) {
    lv_label_set_text(mqtt_modal_status_, connected ? "MQTT connected" : "MQTT saved (connecting...)");
  }
  if (settings_status_ != nullptr) {
    lv_label_set_text_fmt(settings_status_, connected ? "MQTT connected: %s:%u" : "MQTT saved: %s:%u",
                          host.c_str(), static_cast<unsigned>(parsed_port));
  }
  return true;
}

bool SetupUi::clear_mqtt_modal() {
  if (!mqtt_clear_broker_config_HAL()) {
    if (mqtt_modal_status_ != nullptr) lv_label_set_text(mqtt_modal_status_, "Failed to clear MQTT settings");
    return false;
  }
  if (ta_mqtt_host_ != nullptr) lv_textarea_set_text(ta_mqtt_host_, "");
  if (ta_mqtt_port_ != nullptr) lv_textarea_set_text(ta_mqtt_port_, "");
  if (ta_mqtt_user_ != nullptr) lv_textarea_set_text(ta_mqtt_user_, "");
  if (ta_mqtt_pass_ != nullptr) lv_textarea_set_text(ta_mqtt_pass_, "");
  if (ta_mqtt_client_ != nullptr) lv_textarea_set_text(ta_mqtt_client_, "");
  if (mqtt_modal_status_ != nullptr) lv_label_set_text(mqtt_modal_status_, "MQTT settings cleared");
  if (settings_status_ != nullptr) lv_label_set_text(settings_status_, "MQTT settings cleared");
  return true;
}

void SetupUi::dispatch_remote_command(CommandSlot slot) {
  const ActivityRecord* activity = selected_activity();
  if (activity == nullptr) {
    lv_label_set_text(remote_status_, "No activity selected");
    return;
  }
  bool sent = dispatcher_.dispatch(*activity, slot);
  if (sent) {
    lv_label_set_text_fmt(remote_status_, "Sent: %s", to_string(slot));
  } else {
    lv_label_set_text_fmt(remote_status_, "%s: %s", to_string(slot), dispatcher_.last_status().c_str());
  }
}

void SetupUi::dispatch_physical_key(char key_char) {
  const ActivityRecord* activity = selected_activity();
  if (activity == nullptr) return;
  for (const ActivityKeyBinding& binding : activity->key_bindings) {
    if (binding.key_char == key_char) {
      if (binding.device_id != 0 && !binding.command_name.empty()) {
        const bool sent = dispatcher_.dispatch_device_command(binding.device_id, binding.command_name);
        if (remote_status_ != nullptr) {
          const DeviceRecord* device = device_registry_.get_by_id(binding.device_id);
          if (sent) {
            lv_label_set_text_fmt(remote_status_, "Sent: %s (%s)",
                                  binding.command_name.c_str(),
                                  device != nullptr ? device->name.c_str() : "Unknown");
          } else {
            lv_label_set_text_fmt(remote_status_, "Failed: %s (%s): %s",
                                  binding.command_name.c_str(),
                                  device != nullptr ? device->name.c_str() : "Unknown",
                                  dispatcher_.last_status().c_str());
          }
        }
      } else if (!binding.command_name.empty()) {
        CommandSlot slot;
        if (command_slot_from_string(binding.command_name, &slot)) {
          dispatch_remote_command(slot);
        }
      }
      return;
    }
  }
}

void SetupUi::handle_physical_key(char key_char) {
  dispatch_physical_key(key_char);
}

const DeviceRecord* SetupUi::selected_device() const {
  const std::vector<DeviceRecord>& devices = device_registry_.all();
  if (selected_device_index_ < 0 || selected_device_index_ >= static_cast<int>(devices.size())) return nullptr;
  return &devices[selected_device_index_];
}

const ActivityRecord* SetupUi::selected_activity() const {
  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) return nullptr;
  return &activities[selected_activity_index_];
}

void SetupUi::on_tabview_changed(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->save_ui_context();
}

void SetupUi::on_device_add_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_device_modal();
}

void SetupUi::on_device_remove_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr || self->selected_device_index_ < 0) return;
  const DeviceRecord* selected = self->selected_device();
  if (selected == nullptr) return;
  const uint32_t removed_device_id = selected->id;
  if (!self->device_registry_.remove_by_index(static_cast<size_t>(self->selected_device_index_))) return;
  if (!self->remove_device_references_from_activities(removed_device_id) && self->settings_status_ != nullptr) {
    lv_label_set_text(self->settings_status_, "Warning: failed to clean activity refs");
  }
  self->selected_device_index_ = -1;
  self->rebuild_device_list();
  self->rebuild_activity_list();
  self->rebuild_remote_activity_dropdown();
  if (self->activity_keymap_modal_ != nullptr) {
    self->refresh_activity_key_options();
    self->refresh_activity_keymap_device_dropdown();
    self->refresh_activity_keymap_command_dropdown();
    self->refresh_activity_keymap_binding_hint();
  }
}

void SetupUi::on_device_rename_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_rename_modal(false);
}

void SetupUi::on_device_duplicate_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->duplicate_selected_device()) {
    lv_label_set_text(self->selected_device_label_, "Duplicated selected device");
  } else {
    lv_label_set_text(self->selected_device_label_, "Duplicate failed: select a device");
  }
}

void SetupUi::on_device_move_up_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->move_device_selection(-1)) {
    lv_label_set_text(self->selected_device_label_, "Moved device up");
  } else {
    lv_label_set_text(self->selected_device_label_, "Move up unavailable");
  }
}

void SetupUi::on_device_move_down_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->move_device_selection(1)) {
    lv_label_set_text(self->selected_device_label_, "Moved device down");
  } else {
    lv_label_set_text(self->selected_device_label_, "Move down unavailable");
  }
}

void SetupUi::on_device_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  lv_obj_t* target = lv_event_get_target(event);
  for (size_t i = 0; i < self->device_row_buttons_.size(); ++i) {
    if (self->device_row_buttons_[i] == target) {
      self->selected_device_index_ = static_cast<int>(i);
      break;
    }
  }
  self->rebuild_device_list();
  if (self->remote_activity_dd_ != nullptr && self->selected_device_index_ >= 0) {
    lv_dropdown_set_selected(self->remote_activity_dd_, static_cast<uint16_t>(self->selected_device_index_));
  }
  self->rebuild_remote_command_buttons();
}

void SetupUi::on_device_modal_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_device_modal();
}

void SetupUi::on_device_modal_save(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->save_device_modal()) {
    self->close_device_modal();
  }
}

void SetupUi::on_device_command_edit(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->selected_device() == nullptr) {
    lv_label_set_text(self->selected_device_label_, "Selected: choose a device first");
    return;
  }
  self->open_command_modal();
}

void SetupUi::on_command_modal_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_command_modal();
}

void SetupUi::on_command_modal_save(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->save_command_modal();
}

void SetupUi::on_command_modal_remove(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->remove_command_modal();
}

void SetupUi::on_command_modal_test(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->test_current_command();
}

void SetupUi::on_command_modal_learn(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->selected_device() == nullptr) {
    if (self->command_learn_status_ != nullptr) {
      lv_label_set_text(self->command_learn_status_, "Learn: select device first");
    }
    return;
  }
  self->start_ir_learning();
}

void SetupUi::on_command_transport_changed(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->refresh_command_modal_transport_ui();
}

void SetupUi::on_command_template_apply(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->apply_command_template();
}

void SetupUi::on_command_name_changed(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  char selected[64];
  lv_dropdown_get_selected_str(self->dd_command_name_, selected, sizeof(selected));
  if (trim_copy(selected) == "Add New") {
    self->open_command_name_modal();
    return;
  }
  self->update_command_payload_for_selected_name();
}

void SetupUi::on_command_name_modal_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_command_name_modal();
}

void SetupUi::on_command_name_modal_save(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->save_command_name_modal()) {
    self->close_command_name_modal();
  }
}

void SetupUi::on_rename_modal_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_rename_modal();
}

void SetupUi::on_rename_modal_save(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->save_rename_modal()) {
    self->close_rename_modal();
  }
}

void SetupUi::on_textarea_focus(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr || self->keyboard_ == nullptr) return;
  lv_obj_t* target = lv_event_get_target(event);
  lv_keyboard_set_textarea(self->keyboard_, target);
  lv_obj_clear_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_scroll_to_view_recursive(target, LV_ANIM_ON);
}

void SetupUi::on_activity_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  lv_obj_t* target = lv_event_get_target(event);
  for (size_t i = 0; i < self->activity_row_buttons_.size(); ++i) {
    if (self->activity_row_buttons_[i] == target) {
      self->selected_activity_index_ = static_cast<int>(i);
      break;
    }
  }
  self->rebuild_activity_list();
  const ActivityRecord* activity = self->selected_activity();
  if (activity != nullptr) {
    self->execute_activity_startup_actions(*activity);
    if (self->remote_status_ != nullptr) {
      lv_label_set_text_fmt(self->remote_status_, "Activity: %s", activity->name.c_str());
    }
  }
  self->save_ui_context();
}

void SetupUi::on_activity_add_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_activity_modal();
}

void SetupUi::on_activity_remove_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr || self->selected_activity_index_ < 0) return;
  self->activity_registry_.remove_by_index(static_cast<size_t>(self->selected_activity_index_));
  self->selected_activity_index_ = -1;
  self->rebuild_activity_list();
  self->rebuild_remote_activity_dropdown();
  self->save_ui_context();
}

void SetupUi::on_activity_rename_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_rename_modal(true);
}

void SetupUi::on_activity_duplicate_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->duplicate_selected_activity()) {
    lv_label_set_text(self->selected_activity_label_, "Duplicated selected activity");
    self->save_ui_context();
  } else {
    lv_label_set_text(self->selected_activity_label_, "Duplicate failed: select an activity");
  }
}

void SetupUi::on_activity_move_up_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->move_activity_selection(-1)) {
    lv_label_set_text(self->selected_activity_label_, "Moved activity up");
    self->save_ui_context();
  } else {
    lv_label_set_text(self->selected_activity_label_, "Move up unavailable");
  }
}

void SetupUi::on_activity_move_down_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->move_activity_selection(1)) {
    lv_label_set_text(self->selected_activity_label_, "Moved activity down");
    self->save_ui_context();
  } else {
    lv_label_set_text(self->selected_activity_label_, "Move down unavailable");
  }
}

void SetupUi::on_activity_modal_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_activity_modal();
}

void SetupUi::on_activity_modal_save(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->save_activity_modal()) {
    self->save_ui_context();
    self->close_activity_modal();
  }
}

void SetupUi::on_activity_keymap_open(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_activity_keymap_modal();
}

void SetupUi::on_activity_builder_open(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_activity_builder_modal();
}

void SetupUi::on_activity_keymap_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_activity_keymap_modal();
}

void SetupUi::on_activity_keymap_save(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->save_activity_keymap_modal();
}

void SetupUi::on_activity_keymap_device_changed(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->refresh_activity_keymap_command_dropdown();
  self->refresh_activity_keymap_binding_hint();
}

void SetupUi::on_activity_keymap_command_changed(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->refresh_activity_keymap_binding_hint();
}

void SetupUi::on_activity_keymap_device_filter_changed(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->refresh_activity_keymap_device_dropdown();
  self->refresh_activity_keymap_command_dropdown();
  self->refresh_activity_keymap_binding_hint();
}

void SetupUi::on_activity_keymap_command_filter_changed(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->refresh_activity_keymap_command_dropdown();
  self->refresh_activity_keymap_binding_hint();
}

void SetupUi::on_activity_keymap_overwrite_cancel(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_activity_keymap_overwrite_modal();
  if (self->keymap_status_label_ != nullptr) {
    lv_label_set_text(self->keymap_status_label_, "Mapping unchanged");
  }
}

void SetupUi::on_activity_keymap_overwrite_confirm(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  const char key_char = self->pending_keymap_key_char_;
  const uint32_t device_id = self->pending_keymap_device_id_;
  const std::string command_name = self->pending_keymap_command_name_;
  self->apply_activity_keymap_pending_overwrite();
  if (self->keymap_status_label_ != nullptr) {
    const DeviceRecord* device = self->device_registry_.get_by_id(device_id);
    lv_label_set_text_fmt(self->keymap_status_label_, "Overwrote: %c -> %s (%s)", key_char, command_name.c_str(),
                          device != nullptr ? device->name.c_str() : "Unknown");
  }
}

void SetupUi::on_activity_builder_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_activity_builder_modal();
}

void SetupUi::on_activity_builder_add_step(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->activity_builder_add_step();
}

void SetupUi::on_activity_builder_clear(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->activity_builder_clear();
}

void SetupUi::on_activity_builder_save(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->activity_builder_save();
}

void SetupUi::on_settings_backup_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->perform_sd_backup();
}

void SetupUi::on_settings_restore_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_sd_restore_modal();
}

void SetupUi::on_settings_restore_modal_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_sd_restore_modal();
}

void SetupUi::on_settings_restore_modal_apply(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->perform_sd_restore();
}

void SetupUi::on_settings_wifi_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_wifi_modal();
}

void SetupUi::on_settings_ble_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_ble_modal();
}

void SetupUi::on_settings_mqtt_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_mqtt_modal();
}

void SetupUi::on_settings_icons_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->refresh_remote_icon_overrides();
  self->rebuild_remote_command_buttons();
}

void SetupUi::on_settings_set_time_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_manual_time_modal();
}

void SetupUi::on_settings_power_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_power_modal();
}

void SetupUi::on_settings_time_modal_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_manual_time_modal();
}

void SetupUi::on_settings_time_modal_apply(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->apply_manual_time_modal()) {
    self->close_manual_time_modal();
  }
}

void SetupUi::on_power_modal_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_power_modal();
}

void SetupUi::on_power_modal_apply(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->apply_power_modal()) {
    self->close_power_modal();
  }
}

void SetupUi::on_wifi_modal_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_wifi_modal();
}

void SetupUi::on_wifi_modal_scan(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->refresh_wifi_scan_list();
}

void SetupUi::on_wifi_modal_connect(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->connect_wifi_from_modal()) {
    self->close_wifi_modal();
  }
}

void SetupUi::on_wifi_modal_forget(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->forget_wifi_credentials();
}

void SetupUi::on_ble_modal_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_ble_modal();
}

void SetupUi::on_ble_modal_advertise(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
#if (ENABLE_KEYBOARD_BLE == 1)
  keyboardBLE_startAdvertisingForAll_HAL();
  if (self->settings_status_ != nullptr) lv_label_set_text(self->settings_status_, "BLE advertising started");
#else
  if (self->settings_status_ != nullptr) {
    lv_label_set_text(self->settings_status_, "BLE not enabled in this firmware. Build/flash omote-v2-esp32-s3.");
  }
#endif
  self->refresh_ble_modal_status();
}

void SetupUi::on_ble_modal_stop(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
#if (ENABLE_KEYBOARD_BLE == 1)
  keyboardBLE_stopAdvertising_HAL();
  if (self->settings_status_ != nullptr) lv_label_set_text(self->settings_status_, "BLE advertising stopped");
#else
  if (self->settings_status_ != nullptr) {
    lv_label_set_text(self->settings_status_, "BLE not enabled in this firmware. Build/flash omote-v2-esp32-s3.");
  }
#endif
  self->refresh_ble_modal_status();
}

void SetupUi::on_ble_modal_disconnect(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
#if (ENABLE_KEYBOARD_BLE == 1)
  keyboardBLE_disconnectAllClients_HAL();
  if (self->settings_status_ != nullptr) lv_label_set_text(self->settings_status_, "BLE clients disconnected");
#else
  if (self->settings_status_ != nullptr) {
    lv_label_set_text(self->settings_status_, "BLE not enabled in this firmware. Build/flash omote-v2-esp32-s3.");
  }
#endif
  self->refresh_ble_modal_status();
}

void SetupUi::on_ble_modal_list_bonds(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
#if (ENABLE_KEYBOARD_BLE == 1)
  const std::string bonds = keyboardBLE_getBonds_HAL();
  if (self->ble_modal_status_ != nullptr) {
    if (bonds.empty()) {
      lv_label_set_text(self->ble_modal_status_, "Bonds: none");
    } else {
      lv_label_set_text_fmt(self->ble_modal_status_, "Bonds: %s", bonds.c_str());
    }
  }
  if (self->settings_status_ != nullptr) {
    if (bonds.empty()) {
      lv_label_set_text(self->settings_status_, "BLE bonds: none");
    } else {
      lv_label_set_text_fmt(self->settings_status_, "BLE bonds: %s", bonds.c_str());
    }
  }
#else
  if (self->settings_status_ != nullptr) {
    lv_label_set_text(self->settings_status_, "BLE not enabled in this firmware. Build/flash omote-v2-esp32-s3.");
  }
#endif
}

void SetupUi::on_ble_modal_clear_bonds(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
#if (ENABLE_KEYBOARD_BLE == 1)
  keyboardBLE_deleteBonds_HAL();
  keyboardBLE_disconnectAllClients_HAL();
  if (self->settings_status_ != nullptr) lv_label_set_text(self->settings_status_, "BLE bonds cleared");
#else
  if (self->settings_status_ != nullptr) {
    lv_label_set_text(self->settings_status_, "BLE not enabled in this firmware. Build/flash omote-v2-esp32-s3.");
  }
#endif
  self->refresh_ble_modal_status();
}

void SetupUi::on_mqtt_modal_close(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->close_mqtt_modal();
}

void SetupUi::on_mqtt_modal_save(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->apply_mqtt_modal();
}

void SetupUi::on_mqtt_modal_clear(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->clear_mqtt_modal();
}

void SetupUi::on_remote_activity_changed(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  uint16_t selected = lv_dropdown_get_selected(self->remote_activity_dd_);
  if (selected < self->remote_device_ids_.size()) {
    const uint32_t selected_device_id = self->remote_device_ids_[selected];
    self->selected_device_index_ = -1;
    const std::vector<DeviceRecord>& devices = self->device_registry_.all();
    for (size_t i = 0; i < devices.size(); ++i) {
      if (devices[i].id == selected_device_id) {
        self->selected_device_index_ = static_cast<int>(i);
        break;
      }
    }
  } else {
    self->selected_device_index_ = -1;
  }
  self->remote_command_page_index_ = 0;
  self->rebuild_device_list();
  self->rebuild_remote_command_buttons();
  self->save_ui_context();
}

void SetupUi::on_remote_page_prev(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  if (self->remote_command_page_index_ > 0) {
    --self->remote_command_page_index_;
    self->rebuild_remote_command_buttons();
  }
}

void SetupUi::on_remote_page_next(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  ++self->remote_command_page_index_;
  self->rebuild_remote_command_buttons();
}

void SetupUi::on_remote_command_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  lv_obj_t* target = lv_event_get_target(event);
  if (self->remote_activity_dd_ == nullptr || self->remote_device_ids_.empty()) return;
  const uint16_t selected = lv_dropdown_get_selected(self->remote_activity_dd_);
  if (selected >= self->remote_device_ids_.size()) {
    if (self->remote_status_ != nullptr) lv_label_set_text(self->remote_status_, "No device selected");
    return;
  }
  int command_index = -1;
  for (size_t i = 0; i < self->remote_command_buttons_.size(); ++i) {
    if (self->remote_command_buttons_[i] == target) {
      command_index = static_cast<int>(i);
      break;
    }
  }
  if (command_index < 0 || command_index >= static_cast<int>(self->remote_command_names_.size())) return;

  const uint32_t device_id = self->remote_device_ids_[selected];
  const std::string& command_name = self->remote_command_names_[command_index];
  const bool sent = self->dispatcher_.dispatch_device_command(device_id, command_name);
  const DeviceRecord* device = self->device_registry_.get_by_id(device_id);
  if (sent) {
    lv_label_set_text_fmt(self->remote_status_, "Sent: %s (%s)", command_name.c_str(),
                          device != nullptr ? device->name.c_str() : "Unknown");
  } else {
    lv_label_set_text_fmt(self->remote_status_, "Failed: %s (%s): %s", command_name.c_str(),
                          device != nullptr ? device->name.c_str() : "Unknown",
                          self->dispatcher_.last_status().c_str());
  }
}

void SetupUi::on_keyboard_apply(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr || self->keyboard_ == nullptr) return;
  lv_obj_add_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace omote_v2
