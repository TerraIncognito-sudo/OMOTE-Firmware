#include <Arduino.h>
#include <lvgl.h>
#include <nvs_flash.h>
#include <time.h>
#include <Preferences.h>

#include "hardwareLayer.h"

#include "app/activity_registry.h"
#include "app/command_dispatcher.h"
#include "app/device_registry.h"
#include "ui/setup_ui.h"

using namespace omote_v2;

DeviceRegistry g_registry;
ActivityRegistry g_activity_registry;
CommandDispatcher g_dispatcher(g_registry);
SetupUi* g_ui = nullptr;

enum keypad_rawKeyStatesV2 {IDLE_RAW_V2, PRESSED_RAW_V2, RELEASED_RAW_V2};
struct rawKeyV2 {
  unsigned long timestampReceived;
  char keyChar;
  keypad_rawKeyStatesV2 rawKeyState;
};
static constexpr uint8_t kRows = 5;
static constexpr uint8_t kCols = 5;
rawKeyV2 g_raw_keys[kRows][kCols] = {};
unsigned long g_charge_protection_timer_ms = 0;
bool g_charge_cutoff_applied = false;
unsigned long g_charge_full_since_ms = 0;
unsigned long g_charge_resume_since_ms = 0;
bool g_charge_hw_notice_logged = false;
unsigned long g_activity_check_timer_ms = 0;

namespace {
constexpr const char* kUiPrefsNs = "omotev2ui";
constexpr const char* kUiTimezoneKey = "timezone";
constexpr const char* kDefaultTimezoneValue = "EST5EDT,M3.2.0/2,M11.1.0/2";
constexpr unsigned long kChargeProtectionIntervalMs = 5000;
constexpr unsigned long kChargeFullHoldMs = 30000;
constexpr unsigned long kChargeResumeHoldMs = 15000;
constexpr int kChargeCutoffPercent = 98;
constexpr int kChargeResumePercent = 95;

const char* wakeup_reason_text(Wakeup_reasons reason) {
  switch (reason) {
    case WAKEUP_BY_IMU:
      return "motion";
    case WAKEUP_BY_KEYPAD:
      return "keypad";
    case WAKEUP_BY_RESET:
    default:
      return "reset";
  }
}

void apply_saved_timezone_setting() {
  Preferences prefs;
  std::string timezone = kDefaultTimezoneValue;
  if (prefs.begin(kUiPrefsNs, true)) {
    timezone = std::string(prefs.getString(kUiTimezoneKey, kDefaultTimezoneValue).c_str());
    prefs.end();
  }
  if (timezone.empty()) timezone = kDefaultTimezoneValue;
  setenv("TZ", timezone.c_str(), 1);
  tzset();
}
}  // namespace

void setup() {
  Serial.begin(115200);

  // Recover from NVS partition issues after partition/layout changes.
  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  init_hardware_general_HAL();
  init_sleep_HAL();
  init_preferences_HAL();
  init_userled_HAL();
  init_infraredSender_HAL();

  lv_init();
  init_lvgl_HAL();
  setLastActivityTimestamp_HAL();
  init_battery_HAL();
  init_keys_HAL();
  init_IMU_HAL();
  apply_saved_timezone_setting();
#if (ENABLE_WIFI_AND_MQTT == 1)
  init_mqtt_HAL();
  wifi_request_time_sync_HAL();
#endif
#if (ENABLE_KEYBOARD_BLE == 1)
  init_keyboardBLE_HAL();
#endif

  g_registry.load();
  g_activity_registry.load();
  static SetupUi ui(g_registry, g_activity_registry, g_dispatcher);
  g_ui = &ui;
  g_ui->init();
  if (g_ui != nullptr) {
    g_ui->notify_power_event(std::string("Wake source: ") + wakeup_reason_text(wakeup_reason));
  }
}

void loop() {
  // V2 test firmware currently runs touchscreen-first and avoids legacy keypad/IMU
  // paths until those are re-integrated with the new runtime architecture.
  update_backlightBrightness_HAL();
#if (OMOTE_HARDWARE_REV >= 5)
  update_keyboardBrightness_HAL();
#endif
  if (millis() - g_charge_protection_timer_ms >= kChargeProtectionIntervalMs) {
    g_charge_protection_timer_ms = millis();
    int battery_mv = 0;
    int battery_pct = 0;
    bool battery_charging = false;
    get_battery_status_HAL(&battery_mv, &battery_pct, &battery_charging);

    if (battery_mv <= 0) {
      g_charge_full_since_ms = 0;
      g_charge_resume_since_ms = 0;
    } else if (battery_pct >= kChargeCutoffPercent && battery_charging && !g_charge_cutoff_applied) {
      if (g_charge_full_since_ms == 0) g_charge_full_since_ms = millis();
      g_charge_resume_since_ms = 0;
      if (millis() - g_charge_full_since_ms < kChargeFullHoldMs) {
        // Require stable "full" state to avoid toggling on noisy SOC updates.
      } else if (battery_is_charge_control_available_HAL()) {
        if (set_battery_charging_enabled_HAL(false)) {
          g_charge_cutoff_applied = true;
          g_charge_full_since_ms = 0;
          Serial.println("Charge protection: USB charging disabled (battery full/stable).");
          if (g_ui != nullptr) g_ui->notify_power_event("Charge protection: charging paused at full battery");
        }
      } else if (!g_charge_hw_notice_logged) {
        Serial.println("Charge protection: full battery detected. Hardware auto-termination in use.");
        g_charge_hw_notice_logged = true;
      }
    } else if (g_charge_cutoff_applied && battery_pct <= kChargeResumePercent) {
      if (g_charge_resume_since_ms == 0) g_charge_resume_since_ms = millis();
      g_charge_full_since_ms = 0;
      if (millis() - g_charge_resume_since_ms < kChargeResumeHoldMs) {
        // Hold before re-enabling charging.
      } else if (battery_is_charge_control_available_HAL()) {
        if (set_battery_charging_enabled_HAL(true)) {
          Serial.println("Charge protection: USB charging re-enabled.");
          g_charge_cutoff_applied = false;
          g_charge_resume_since_ms = 0;
          if (g_ui != nullptr) g_ui->notify_power_event("Charge protection: charging resumed");
        }
      } else {
        g_charge_cutoff_applied = false;
        g_charge_resume_since_ms = 0;
      }
    } else {
      g_charge_full_since_ms = 0;
      if (!g_charge_cutoff_applied) g_charge_resume_since_ms = 0;
      if (battery_pct <= (kChargeResumePercent - 2)) {
        g_charge_hw_notice_logged = false;
      }
    }
  }

  if (millis() - g_activity_check_timer_ms >= 100) {
    g_activity_check_timer_ms = millis();
    check_activity_HAL();
  }
#if (ENABLE_WIFI_AND_MQTT == 1)
  mqtt_loop_HAL();
#endif

  keys_getKeys_HAL(g_raw_keys, millis());
  for (uint8_t row = 0; row < kRows; ++row) {
    for (uint8_t col = 0; col < kCols; ++col) {
      if (g_raw_keys[row][col].rawKeyState == PRESSED_RAW_V2 && g_ui != nullptr) {
        g_ui->handle_physical_key(g_raw_keys[row][col].keyChar);
      }
      g_raw_keys[row][col].rawKeyState = IDLE_RAW_V2;
    }
  }
  g_ui->tick();
  delay(5);
}
