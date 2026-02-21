#include "ui/setup_ui.h"

#include <Arduino.h>
#include <algorithm>
#include <cerrno>
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

std::string trim_copy(const std::string& in) {
  size_t start = 0;
  while (start < in.size() && (in[start] == ' ' || in[start] == '\t')) ++start;
  size_t end = in.size();
  while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\t')) --end;
  return in.substr(start, end - start);
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

bool starts_with(const std::string& text, const char* prefix) {
  if (prefix == nullptr) return false;
  return text.rfind(prefix, 0) == 0;
}

bool is_valid_ir_payload(const std::string& raw_payload) {
  std::string payload = trim_copy(raw_payload);
  if (payload.empty()) return true;  // empty means "not mapped"

  size_t first_colon = payload.find(':');
  if (first_colon == std::string::npos) {
    return parse_u64_token(payload);  // data only, defaults for bits/repeat
  }

  size_t second_colon = payload.find(':', first_colon + 1);
  if (second_colon == std::string::npos) return false;
  if (payload.find(':', second_colon + 1) != std::string::npos) return false;

  std::string data = payload.substr(0, first_colon);
  std::string bits = payload.substr(first_colon + 1, second_colon - first_colon - 1);
  std::string repeat = payload.substr(second_colon + 1);
  return parse_u64_token(trim_copy(data)) && parse_u32_token(trim_copy(bits)) && parse_u32_token(trim_copy(repeat));
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
  update_status_bar();
  lv_obj_set_tile(tabview_, activities_page_, LV_ANIM_OFF);
}

void SetupUi::tick() {
  if (millis() - last_status_update_ms_ >= 1000) {
    last_status_update_ms_ = millis();
    update_status_bar();
  }
  if (ir_learning_active_) {
    infraredReceiver_loop_HAL();
  }
  lv_timer_handler();
}

void SetupUi::update_status_bar() {
  bool wifi_connected = false;
#if (ENABLE_WIFI_AND_MQTT == 1)
  wifi_connected = getIsWifiConnected_HAL();
#endif
  lv_label_set_text(wifi_status_label_, "WiFi");
  lv_obj_set_style_text_color(wifi_status_label_,
                              wifi_connected ? lv_color_hex(0x3EDC81) : lv_color_hex(0xE2574C),
                              LV_PART_MAIN);
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
        if (settings_status_ != nullptr) {
          if (wifi_connect_target_ssid_.empty()) {
            lv_label_set_text(settings_status_, "WiFi connected");
          } else {
            lv_label_set_text_fmt(settings_status_, "WiFi connected: %s", wifi_connect_target_ssid_.c_str());
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
    return;
  }
  if (battery_charging) {
    lv_label_set_text_fmt(battery_status_label_, "Batt: %d%%+", battery_pct);
  } else {
    lv_label_set_text_fmt(battery_status_label_, "Batt: %d%%", battery_pct);
  }
}

void SetupUi::build_devices_tab() {
  lv_obj_t* tab = devices_page_;
  if (tab == nullptr) return;
  lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(tab, 6, LV_PART_MAIN);
  const int tab_w = SCR_WIDTH - 12;
  const int footer_h = 34;
  const int list_top = 38;
  const int list_h = SCR_HEIGHT - 44 - list_top - footer_h - 16;

  lv_obj_t* title = lv_label_create(tab);
  lv_label_set_text(title, "Devices");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

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
  const int list_top = 44;
  const int list_h = SCR_HEIGHT - 44 - list_top - footer_h - 16;

  lv_obj_t* title = lv_label_create(tab);
  lv_label_set_text(title, "Activities");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

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
  lv_label_set_text(keymap_label, "Keymap");
  lv_obj_set_style_text_font(keymap_label, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_center(keymap_label);
}

void SetupUi::build_remote_tab() {
  lv_obj_t* tab = remote_page_;
  if (tab == nullptr) return;
  lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(tab, 6, LV_PART_MAIN);
  const int pad = 4;
  const int btn_w = (SCR_WIDTH - 12 - (2 * pad)) / 3;
  const int btn_h = 30;

  lv_obj_t* title = lv_label_create(tab);
  lv_label_set_text(title, "Remote");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t* dd_label = lv_label_create(tab);
  lv_label_set_text(dd_label, "Activity");
  lv_obj_align(dd_label, LV_ALIGN_TOP_LEFT, 0, 14);

  remote_activity_dd_ = lv_dropdown_create(tab);
  lv_obj_set_width(remote_activity_dd_, SCR_WIDTH - 12);
  lv_obj_align(remote_activity_dd_, LV_ALIGN_TOP_LEFT, 0, 30);
  lv_obj_add_event_cb(remote_activity_dd_, on_remote_activity_changed, LV_EVENT_VALUE_CHANGED, this);

  remote_status_ = lv_label_create(tab);
  lv_label_set_text(remote_status_, "Ready");
  lv_obj_align(remote_status_, LV_ALIGN_TOP_LEFT, 0, 62);

  auto add_cmd_btn = [&](const char* text, int x, int y, CommandSlot slot) {
    lv_obj_t* btn = lv_btn_create(tab);
    lv_obj_set_size(btn, btn_w, btn_h);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, on_remote_command_clicked, LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<intptr_t>(slot)));
  };

  const int x0 = 0;
  const int x1 = btn_w + pad;
  const int x2 = x1 + btn_w + pad;
  const int y0 = 84;
  const int y1 = y0 + btn_h + pad;
  const int y2 = y1 + btn_h + pad;
  const int y3 = y2 + btn_h + pad;
  const int y4 = y3 + btn_h + pad;

  add_cmd_btn("Power", x0, y0, CommandSlot::Power);
  add_cmd_btn("Mute", x1, y0, CommandSlot::Mute);
  add_cmd_btn("Home", x2, y0, CommandSlot::Home);

  add_cmd_btn("Up", x1, y1, CommandSlot::Up);
  add_cmd_btn("Left", x0, y2, CommandSlot::Left);
  add_cmd_btn("OK", x1, y2, CommandSlot::Ok);
  add_cmd_btn("Right", x2, y2, CommandSlot::Right);
  add_cmd_btn("Down", x1, y3, CommandSlot::Down);

  add_cmd_btn("Vol+", x0, y4, CommandSlot::VolumeUp);
  add_cmd_btn("Vol-", x1, y4, CommandSlot::VolumeDown);
  add_cmd_btn("Back", x2, y4, CommandSlot::Back);
}

void SetupUi::build_settings_tab() {
  lv_obj_t* tab = settings_page_;
  if (tab == nullptr) return;
  lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(tab);
  lv_label_set_text(title, "Settings");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t* help = lv_label_create(tab);
  lv_label_set_text(help, "Storage and recovery");
  lv_obj_align(help, LV_ALIGN_TOP_LEFT, 0, 20);

  settings_backup_btn_ = lv_btn_create(tab);
  lv_obj_set_size(settings_backup_btn_, SCR_WIDTH - 16, 38);
  lv_obj_align(settings_backup_btn_, LV_ALIGN_TOP_LEFT, 0, 46);
  lv_obj_add_event_cb(settings_backup_btn_, on_settings_backup_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* backup_label = lv_label_create(settings_backup_btn_);
  lv_label_set_text(backup_label, "Backup to SD");
  lv_obj_center(backup_label);

  settings_restore_btn_ = lv_btn_create(tab);
  lv_obj_set_size(settings_restore_btn_, SCR_WIDTH - 16, 38);
  lv_obj_align(settings_restore_btn_, LV_ALIGN_TOP_LEFT, 0, 90);
  lv_obj_add_event_cb(settings_restore_btn_, on_settings_restore_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* restore_label = lv_label_create(settings_restore_btn_);
  lv_label_set_text(restore_label, "Restore from SD...");
  lv_obj_center(restore_label);

  settings_wifi_btn_ = lv_btn_create(tab);
  lv_obj_set_size(settings_wifi_btn_, SCR_WIDTH - 16, 38);
  lv_obj_align(settings_wifi_btn_, LV_ALIGN_TOP_LEFT, 0, 134);
  lv_obj_add_event_cb(settings_wifi_btn_, on_settings_wifi_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* wifi_label = lv_label_create(settings_wifi_btn_);
  lv_label_set_text(wifi_label, "WiFi Settings");
  lv_obj_center(wifi_label);

  settings_set_time_btn_ = lv_btn_create(tab);
  lv_obj_set_size(settings_set_time_btn_, SCR_WIDTH - 16, 38);
  lv_obj_align(settings_set_time_btn_, LV_ALIGN_TOP_LEFT, 0, 178);
  lv_obj_add_event_cb(settings_set_time_btn_, on_settings_set_time_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* set_time_label = lv_label_create(settings_set_time_btn_);
  lv_label_set_text(set_time_label, "Set Time");
  lv_obj_center(set_time_label);

  settings_status_ = lv_label_create(tab);
  lv_label_set_text(settings_status_, "Status: idle");
  lv_obj_set_width(settings_status_, SCR_WIDTH - 16);
  lv_label_set_long_mode(settings_status_, LV_LABEL_LONG_WRAP);
  lv_obj_align(settings_status_, LV_ALIGN_TOP_LEFT, 0, 224);
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
    char row[120];
    snprintf(row, sizeof(row), "%s (%u dev / %u keys / %u start)", a.name.c_str(),
             static_cast<unsigned>(a.device_ids.size()),
             static_cast<unsigned>(a.key_bindings.size()),
             static_cast<unsigned>(a.startup_actions.size()));
    lv_obj_t* btn = lv_list_add_btn(activity_list_, nullptr, row);
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
  std::string options;
  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  for (size_t i = 0; i < activities.size(); ++i) {
    if (i > 0) options.append("\n");
    options.append(activities[i].name);
  }

  if (options.empty()) {
    options = "No activity";
  }
  lv_dropdown_set_options(remote_activity_dd_, options.c_str());

  int selected = selected_activity_index_;
  if (selected < 0 || selected >= static_cast<int>(activities.size())) {
    selected = activities.empty() ? -1 : 0;
  }
  selected_activity_index_ = selected;
  if (selected >= 0) {
    lv_dropdown_set_selected(remote_activity_dd_, static_cast<uint16_t>(selected));
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
  lv_obj_set_style_pad_bottom(command_modal_, 148, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(command_modal_);
  lv_label_set_text_fmt(title, "Edit: %s", device->name.c_str());
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  dd_command_name_ = lv_dropdown_create(command_modal_);
  lv_obj_set_width(dd_command_name_, SCR_WIDTH - 12);
  lv_obj_align(dd_command_name_, LV_ALIGN_TOP_LEFT, 0, 28);
  lv_obj_add_event_cb(dd_command_name_, on_command_name_changed, LV_EVENT_VALUE_CHANGED, this);

  ta_command_payload_ = lv_textarea_create(command_modal_);
  lv_obj_set_width(ta_command_payload_, SCR_WIDTH - 12);
  lv_obj_set_height(ta_command_payload_, 64);
  lv_textarea_set_placeholder_text(ta_command_payload_, "IR payload: data:bits:repeat (e.g. 0x20DF10EF:32:0)");
  lv_obj_align(ta_command_payload_, LV_ALIGN_TOP_LEFT, 0, 62);
  lv_obj_add_event_cb(ta_command_payload_, on_textarea_focus, LV_EVENT_FOCUSED, this);

  lv_obj_t* hint = lv_label_create(command_modal_);
  lv_label_set_text(hint, "Select 'Add New' in the list to create a command name.");
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 130);

  lv_obj_t* cancel_btn = lv_btn_create(command_modal_);
  lv_obj_set_size(cancel_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 0, 154);
  lv_obj_add_event_cb(cancel_btn, on_command_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* cancel_text = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_text, "Close");
  lv_obj_center(cancel_text);

  lv_obj_t* save_btn = lv_btn_create(command_modal_);
  lv_obj_set_size(save_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(save_btn, LV_ALIGN_TOP_MID, 0, 154);
  lv_obj_add_event_cb(save_btn, on_command_modal_save, LV_EVENT_CLICKED, this);
  lv_obj_t* save_text = lv_label_create(save_btn);
  lv_label_set_text(save_text, "Save");
  lv_obj_center(save_text);

  lv_obj_t* remove_btn = lv_btn_create(command_modal_);
  lv_obj_set_size(remove_btn, (SCR_WIDTH - 24) / 3, 32);
  lv_obj_align(remove_btn, LV_ALIGN_TOP_RIGHT, 0, 154);
  lv_obj_add_event_cb(remove_btn, on_command_modal_remove, LV_EVENT_CLICKED, this);
  lv_obj_t* remove_text = lv_label_create(remove_btn);
  lv_label_set_text(remove_text, "Remove");
  lv_obj_center(remove_text);

  lv_obj_t* test_btn = lv_btn_create(command_modal_);
  lv_obj_set_size(test_btn, (SCR_WIDTH - 18) / 2, 28);
  lv_obj_align(test_btn, LV_ALIGN_TOP_LEFT, 0, 192);
  lv_obj_add_event_cb(test_btn, on_command_modal_test, LV_EVENT_CLICKED, this);
  lv_obj_t* test_text = lv_label_create(test_btn);
  lv_label_set_text(test_text, "Test Command");
  lv_obj_center(test_text);

  lv_obj_t* learn_btn = lv_btn_create(command_modal_);
  lv_obj_set_size(learn_btn, (SCR_WIDTH - 18) / 2, 28);
  lv_obj_align(learn_btn, LV_ALIGN_TOP_RIGHT, 0, 192);
  lv_obj_add_event_cb(learn_btn, on_command_modal_learn, LV_EVENT_CLICKED, this);
  lv_obj_t* learn_text = lv_label_create(learn_btn);
  lv_label_set_text(learn_text, "Learn IR");
  lv_obj_center(learn_text);

  command_learn_status_ = lv_label_create(command_modal_);
  lv_label_set_text(command_learn_status_, "Learn: idle");
  lv_obj_align(command_learn_status_, LV_ALIGN_TOP_LEFT, 0, 226);

  keyboard_ = lv_keyboard_create(root_);
  lv_obj_set_size(keyboard_, SCR_WIDTH, 136);
  lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard_, on_keyboard_apply, LV_EVENT_READY, this);

  refresh_command_name_dropdown();
  update_command_payload_for_selected_name();
}

void SetupUi::close_command_modal() {
  stop_ir_learning();
  close_command_name_modal();
  if (command_modal_ == nullptr) return;
  lv_obj_del(command_modal_);
  command_modal_ = nullptr;
  dd_command_name_ = nullptr;
  ta_command_payload_ = nullptr;
  command_learn_status_ = nullptr;
  if (keyboard_ != nullptr) {
    lv_obj_del(keyboard_);
    keyboard_ = nullptr;
  }
}

bool SetupUi::save_command_modal() {
  if (dd_command_name_ == nullptr || ta_command_payload_ == nullptr || command_editor_device_id_ == 0) return false;
  DeviceRecord* device = device_registry_.get_by_id(command_editor_device_id_);
  if (device == nullptr) return false;

  const std::string command_name = selected_command_name();
  if (command_name.empty()) {
    lv_label_set_text(remote_status_, "Select or add a command name");
    return false;
  }
  std::string payload = lv_textarea_get_text(ta_command_payload_);
  if (device->transport == TransportType::IR && !is_valid_ir_payload(payload)) {
    lv_label_set_text(remote_status_, "Invalid IR payload format");
    return false;
  }
  upsert_command(device, command_name, payload);
  device_registry_.save();
  lv_label_set_text_fmt(remote_status_, "Saved command: %s", command_name.c_str());
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
  save_command_modal();
  const std::string command_name = selected_command_name();
  if (command_name.empty()) return;
  dispatcher_.dispatch_device_command(command_editor_device_id_, command_name);
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
  if (!is_valid_ir_payload(payload)) {
    lv_label_set_text(command_learn_status_, "Learn: bad payload");
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

  activity_device_list_ = lv_obj_create(activity_modal_);
  lv_obj_set_size(activity_device_list_, SCR_WIDTH - 12, 90);
  lv_obj_align(activity_device_list_, LV_ALIGN_TOP_LEFT, 0, 58);
  lv_obj_set_style_pad_all(activity_device_list_, 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(activity_device_list_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(activity_device_list_, LV_DIR_VER);

  activity_device_checkboxes_.clear();
  activity_device_checkbox_ids_.clear();
  const std::vector<DeviceRecord>& devices = device_registry_.all();
  for (const DeviceRecord& device : devices) {
    lv_obj_t* cb = lv_checkbox_create(activity_device_list_);
    lv_checkbox_set_text(cb, device.name.c_str());
    activity_device_checkboxes_.push_back(cb);
    activity_device_checkbox_ids_.push_back(device.id);
  }

  lv_obj_t* note = lv_label_create(activity_modal_);
  lv_label_set_text(note, "Select devices for this activity.");
  lv_obj_align(note, LV_ALIGN_TOP_LEFT, 0, 152);

  lv_obj_t* cancel_btn = lv_btn_create(activity_modal_);
  lv_obj_set_size(cancel_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 0, 172);
  lv_obj_add_event_cb(cancel_btn, on_activity_modal_close, LV_EVENT_CLICKED, this);
  lv_obj_t* cancel_text = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_text, "Cancel");
  lv_obj_center(cancel_text);

  lv_obj_t* save_btn = lv_btn_create(activity_modal_);
  lv_obj_set_size(save_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, 0, 172);
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
  activity_device_list_ = nullptr;
  activity_device_checkboxes_.clear();
  activity_device_checkbox_ids_.clear();
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

  for (size_t i = 0; i < activity_device_checkboxes_.size() && i < activity_device_checkbox_ids_.size(); ++i) {
    if (lv_obj_has_state(activity_device_checkboxes_[i], LV_STATE_CHECKED)) {
      activity.device_ids.push_back(activity_device_checkbox_ids_[i]);
    }
  }

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

  lv_obj_t* title = lv_label_create(activity_keymap_modal_);
  lv_label_set_text_fmt(title, "Keymap: %s", activities[selected_activity_index_].name.c_str());
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  dd_keymap_key_ = lv_dropdown_create(activity_keymap_modal_);
  lv_obj_set_width(dd_keymap_key_, SCR_WIDTH - 12);
  lv_obj_align(dd_keymap_key_, LV_ALIGN_TOP_LEFT, 0, 30);
  lv_obj_add_event_cb(dd_keymap_key_, on_activity_keymap_command_changed, LV_EVENT_VALUE_CHANGED, this);

  dd_keymap_device_ = lv_dropdown_create(activity_keymap_modal_);
  lv_obj_set_width(dd_keymap_device_, SCR_WIDTH - 12);
  lv_obj_align(dd_keymap_device_, LV_ALIGN_TOP_LEFT, 0, 68);
  lv_obj_add_event_cb(dd_keymap_device_, on_activity_keymap_device_changed, LV_EVENT_VALUE_CHANGED, this);

  dd_keymap_command_ = lv_dropdown_create(activity_keymap_modal_);
  lv_obj_set_width(dd_keymap_command_, SCR_WIDTH - 12);
  lv_obj_align(dd_keymap_command_, LV_ALIGN_TOP_LEFT, 0, 100);
  lv_obj_add_event_cb(dd_keymap_command_, on_activity_keymap_command_changed, LV_EVENT_VALUE_CHANGED, this);

  keymap_status_label_ = lv_label_create(activity_keymap_modal_);
  lv_label_set_text(keymap_status_label_, "Pick key + device + command, then Save");
  lv_obj_align(keymap_status_label_, LV_ALIGN_TOP_LEFT, 0, 132);

  keymap_slot_hint_label_ = lv_label_create(activity_keymap_modal_);
  lv_obj_set_width(keymap_slot_hint_label_, SCR_WIDTH - 12);
  lv_label_set_long_mode(keymap_slot_hint_label_, LV_LABEL_LONG_WRAP);
  lv_obj_align(keymap_slot_hint_label_, LV_ALIGN_TOP_LEFT, 0, 148);

  lv_obj_t* close_btn = lv_btn_create(activity_keymap_modal_);
  lv_obj_set_size(close_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 0, 216);
  lv_obj_add_event_cb(close_btn, on_activity_keymap_close, LV_EVENT_CLICKED, this);
  lv_obj_t* close_text = lv_label_create(close_btn);
  lv_label_set_text(close_text, "Close");
  lv_obj_center(close_text);

  lv_obj_t* save_btn = lv_btn_create(activity_keymap_modal_);
  lv_obj_set_size(save_btn, (SCR_WIDTH - 18) / 2, 32);
  lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, 0, 216);
  lv_obj_add_event_cb(save_btn, on_activity_keymap_save, LV_EVENT_CLICKED, this);
  lv_obj_t* save_text = lv_label_create(save_btn);
  lv_label_set_text(save_text, "Save Mapping");
  lv_obj_center(save_text);

  refresh_activity_key_options();
  refresh_activity_keymap_device_dropdown();
  refresh_activity_keymap_command_dropdown();
  refresh_activity_keymap_binding_hint();
}

void SetupUi::close_activity_keymap_modal() {
  if (activity_keymap_modal_ == nullptr) return;
  lv_obj_del(activity_keymap_modal_);
  activity_keymap_modal_ = nullptr;
  dd_keymap_key_ = nullptr;
  dd_keymap_device_ = nullptr;
  dd_keymap_command_ = nullptr;
  keymap_status_label_ = nullptr;
  keymap_slot_hint_label_ = nullptr;
  keymap_device_ids_.clear();
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

  ActivityRecord updated = activities[selected_activity_index_];
  bool replaced = false;
  for (ActivityKeyBinding& binding : updated.key_bindings) {
    if (binding.key_char == key_char) {
      binding.device_id = device_id;
      binding.command_name = command_name;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    ActivityKeyBinding binding;
    binding.key_char = key_char;
    binding.device_id = device_id;
    binding.command_name = command_name;
    updated.key_bindings.push_back(binding);
  }
  activity_registry_.upsert(updated);
  lv_label_set_text_fmt(keymap_status_label_, "Saved: %c -> %s (%s)", key_char, command_name.c_str(), selected_device != nullptr ? selected_device->name.c_str() : "Unknown");
  rebuild_activity_list();
  refresh_activity_key_options();
  refresh_activity_keymap_binding_hint();
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
  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) return;
  const ActivityRecord& activity = activities[selected_activity_index_];

  std::string options;
  keymap_device_ids_.clear();
  for (size_t i = 0; i < activity.device_ids.size(); ++i) {
    const DeviceRecord* d = device_registry_.get_by_id(activity.device_ids[i]);
    if (d == nullptr) continue;
    if (!options.empty()) options += "\n";
    options += d->name;
    keymap_device_ids_.push_back(d->id);
  }
  if (options.empty()) options = "No devices";
  lv_dropdown_set_options(dd_keymap_device_, options.c_str());
}

void SetupUi::refresh_activity_keymap_command_dropdown() {
  if (dd_keymap_device_ == nullptr || dd_keymap_command_ == nullptr) return;

  const std::vector<ActivityRecord>& activities = activity_registry_.all();
  if (selected_activity_index_ < 0 || selected_activity_index_ >= static_cast<int>(activities.size())) {
    return;
  }
  const uint16_t selected_idx = lv_dropdown_get_selected(dd_keymap_device_);
  const DeviceRecord* selected_device = (selected_idx < keymap_device_ids_.size()) ? device_registry_.get_by_id(keymap_device_ids_[selected_idx]) : nullptr;

  std::string options;
  if (selected_device != nullptr) {
    for (size_t i = 0; i < selected_device->commands.size(); ++i) {
      if (i > 0) options += "\n";
      options += selected_device->commands[i].name;
    }
  }
  if (options.empty()) options = "No commands";
  lv_dropdown_set_options(dd_keymap_command_, options.c_str());
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
  for (uint32_t id : selected.device_ids) {
    const DeviceRecord* device = device_registry_.get_by_id(id);
    if (device == nullptr) continue;
    if (!device_options.empty()) device_options += "\n";
    device_options += device->name;
    activity_builder_device_ids_.push_back(id);
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
    lv_label_set_text(builder_status_label_, "No devices in activity");
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
  close_sd_restore_modal();
  if (normalized) {
    status += " (normalized)";
  }
  if (settings_status_ != nullptr) lv_label_set_text(settings_status_, status.c_str());
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
    lv_label_set_text_fmt(remote_status_, "No mapping: %s", to_string(slot));
  }
}

void SetupUi::dispatch_physical_key(char key_char) {
  const ActivityRecord* activity = selected_activity();
  if (activity == nullptr) return;
  for (const ActivityKeyBinding& binding : activity->key_bindings) {
    if (binding.key_char == key_char) {
      if (binding.device_id != 0 && !binding.command_name.empty()) {
        dispatcher_.dispatch_device_command(binding.device_id, binding.command_name);
        if (remote_status_ != nullptr) {
          const DeviceRecord* device = device_registry_.get_by_id(binding.device_id);
          lv_label_set_text_fmt(remote_status_, "Sent: %s (%s)",
                                binding.command_name.c_str(),
                                device != nullptr ? device->name.c_str() : "Unknown");
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
  int current = lv_dropdown_get_selected(remote_activity_dd_);
  if (current < 0 || current >= static_cast<int>(activities.size())) return nullptr;
  return &activities[current];
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
  const std::vector<ActivityRecord>& activities = self->activity_registry_.all();
  if (self->selected_activity_index_ >= 0 && self->selected_activity_index_ < static_cast<int>(activities.size())) {
    lv_dropdown_set_selected(self->remote_activity_dd_, static_cast<uint16_t>(self->selected_activity_index_));
  }
  self->rebuild_activity_list();
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
}

void SetupUi::on_activity_rename_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_rename_modal(true);
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

void SetupUi::on_settings_set_time_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  self->open_manual_time_modal();
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

void SetupUi::on_remote_activity_changed(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  const std::vector<ActivityRecord>& activities = self->activity_registry_.all();
  uint16_t selected = lv_dropdown_get_selected(self->remote_activity_dd_);
  if (selected < activities.size()) {
    self->selected_activity_index_ = static_cast<int>(selected);
  } else {
    self->selected_activity_index_ = -1;
  }
  self->rebuild_activity_list();

  const ActivityRecord* activity = self->selected_activity();
  if (activity == nullptr) {
    lv_label_set_text(self->remote_status_, "Activity changed");
    return;
  }
  self->execute_activity_startup_actions(*activity);
  lv_label_set_text_fmt(self->remote_status_, "Activity: %s", activity->name.c_str());
}

void SetupUi::on_remote_command_clicked(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr) return;
  lv_obj_t* target = lv_event_get_target(event);
  CommandSlot slot = static_cast<CommandSlot>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
  self->dispatch_remote_command(slot);
}

void SetupUi::on_keyboard_apply(lv_event_t* event) {
  SetupUi* self = static_cast<SetupUi*>(lv_event_get_user_data(event));
  if (self == nullptr || self->keyboard_ == nullptr) return;
  lv_obj_add_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace omote_v2
