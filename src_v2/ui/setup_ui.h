#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <lvgl.h>

#include "app/activity_registry.h"
#include "app/command_dispatcher.h"
#include "app/device_registry.h"
#include "app/sd_backup_service.h"

namespace omote_v2 {

class SetupUi {
 public:
  SetupUi(DeviceRegistry& device_registry, ActivityRegistry& activity_registry, CommandDispatcher& dispatcher);
  void init();
  void tick();
  void handle_ir_learned_message(const std::string& message);
  void handle_physical_key(char key_char);
  void notify_power_event(const std::string& message);

 private:
  static void on_tabview_changed(lv_event_t* event);
  static void on_device_add_clicked(lv_event_t* event);
  static void on_device_remove_clicked(lv_event_t* event);
  static void on_device_rename_clicked(lv_event_t* event);
  static void on_device_duplicate_clicked(lv_event_t* event);
  static void on_device_move_up_clicked(lv_event_t* event);
  static void on_device_move_down_clicked(lv_event_t* event);
  static void on_device_clicked(lv_event_t* event);
  static void on_device_modal_close(lv_event_t* event);
  static void on_device_modal_save(lv_event_t* event);
  static void on_device_command_edit(lv_event_t* event);
  static void on_command_modal_close(lv_event_t* event);
  static void on_command_modal_save(lv_event_t* event);
  static void on_command_modal_remove(lv_event_t* event);
  static void on_command_modal_test(lv_event_t* event);
  static void on_command_modal_learn(lv_event_t* event);
  static void on_command_transport_changed(lv_event_t* event);
  static void on_command_template_apply(lv_event_t* event);
  static void on_command_name_changed(lv_event_t* event);
  static void on_command_name_modal_close(lv_event_t* event);
  static void on_command_name_modal_save(lv_event_t* event);
  static void on_rename_modal_close(lv_event_t* event);
  static void on_rename_modal_save(lv_event_t* event);
  static void on_activity_keymap_device_changed(lv_event_t* event);
  static void on_activity_keymap_command_changed(lv_event_t* event);
  static void on_activity_keymap_device_filter_changed(lv_event_t* event);
  static void on_activity_keymap_command_filter_changed(lv_event_t* event);
  static void on_activity_keymap_overwrite_cancel(lv_event_t* event);
  static void on_activity_keymap_overwrite_confirm(lv_event_t* event);
  static void on_textarea_focus(lv_event_t* event);
  static void on_activity_clicked(lv_event_t* event);
  static void on_activity_add_clicked(lv_event_t* event);
  static void on_activity_remove_clicked(lv_event_t* event);
  static void on_activity_rename_clicked(lv_event_t* event);
  static void on_activity_duplicate_clicked(lv_event_t* event);
  static void on_activity_move_up_clicked(lv_event_t* event);
  static void on_activity_move_down_clicked(lv_event_t* event);
  static void on_activity_modal_close(lv_event_t* event);
  static void on_activity_modal_save(lv_event_t* event);
  static void on_activity_keymap_open(lv_event_t* event);
  static void on_activity_builder_open(lv_event_t* event);
  static void on_activity_keymap_close(lv_event_t* event);
  static void on_activity_keymap_save(lv_event_t* event);
  static void on_activity_builder_close(lv_event_t* event);
  static void on_activity_builder_add_step(lv_event_t* event);
  static void on_activity_builder_clear(lv_event_t* event);
  static void on_activity_builder_save(lv_event_t* event);
  static void on_settings_backup_clicked(lv_event_t* event);
  static void on_settings_restore_clicked(lv_event_t* event);
  static void on_settings_restore_modal_close(lv_event_t* event);
  static void on_settings_restore_modal_apply(lv_event_t* event);
  static void on_settings_wifi_clicked(lv_event_t* event);
  static void on_settings_ble_clicked(lv_event_t* event);
  static void on_settings_mqtt_clicked(lv_event_t* event);
  static void on_settings_icons_clicked(lv_event_t* event);
  static void on_settings_set_time_clicked(lv_event_t* event);
  static void on_settings_power_clicked(lv_event_t* event);
  static void on_settings_time_modal_close(lv_event_t* event);
  static void on_settings_time_modal_apply(lv_event_t* event);
  static void on_power_modal_close(lv_event_t* event);
  static void on_power_modal_apply(lv_event_t* event);
  static void on_wifi_modal_close(lv_event_t* event);
  static void on_wifi_modal_scan(lv_event_t* event);
  static void on_wifi_modal_connect(lv_event_t* event);
  static void on_wifi_modal_forget(lv_event_t* event);
  static void on_ble_modal_close(lv_event_t* event);
  static void on_ble_modal_advertise(lv_event_t* event);
  static void on_ble_modal_stop(lv_event_t* event);
  static void on_ble_modal_disconnect(lv_event_t* event);
  static void on_ble_modal_list_bonds(lv_event_t* event);
  static void on_ble_modal_clear_bonds(lv_event_t* event);
  static void on_mqtt_modal_close(lv_event_t* event);
  static void on_mqtt_modal_save(lv_event_t* event);
  static void on_mqtt_modal_clear(lv_event_t* event);
  static void on_remote_activity_changed(lv_event_t* event);
  static void on_remote_page_prev(lv_event_t* event);
  static void on_remote_page_next(lv_event_t* event);
  static void on_remote_command_clicked(lv_event_t* event);
  static void on_keyboard_apply(lv_event_t* event);

  void build_devices_tab();
  void build_activities_tab();
  void build_remote_tab();
  void build_settings_tab();
  void save_ui_context();
  void restore_ui_context();
  void update_status_bar();
  void rebuild_device_list();
  void rebuild_activity_list();
  void rebuild_remote_activity_dropdown();
  void rebuild_remote_command_buttons();
  void refresh_remote_icon_overrides();
  void open_device_modal();
  void close_device_modal();
  bool save_device_modal();
  void open_command_modal();
  void close_command_modal();
  bool save_command_modal();
  bool remove_command_modal();
  void refresh_command_modal_transport_ui();
  bool apply_command_template();
  void refresh_command_name_dropdown();
  std::string selected_command_name() const;
  void update_command_payload_for_selected_name();
  void open_command_name_modal();
  void close_command_name_modal();
  bool save_command_name_modal();
  void open_rename_modal(bool rename_activity);
  void close_rename_modal();
  bool save_rename_modal();
  void test_current_command();
  void start_ir_learning();
  void stop_ir_learning();
  void open_activity_modal();
  void close_activity_modal();
  bool save_activity_modal();
  void open_activity_keymap_modal();
  void close_activity_keymap_modal();
  void save_activity_keymap_modal();
  void refresh_activity_key_options();
  void refresh_activity_keymap_device_dropdown();
  void refresh_activity_keymap_command_dropdown();
  void refresh_activity_keymap_binding_hint();
  void open_activity_keymap_overwrite_modal(const ActivityKeyBinding& existing_binding, uint32_t new_device_id,
                                            const std::string& new_command_name);
  void close_activity_keymap_overwrite_modal();
  void apply_activity_keymap_pending_overwrite();
  bool move_device_selection(int delta);
  bool move_activity_selection(int delta);
  bool duplicate_selected_device();
  bool duplicate_selected_activity();
  void open_activity_builder_modal();
  void close_activity_builder_modal();
  void activity_builder_add_step();
  void activity_builder_clear();
  void activity_builder_save();
  void refresh_activity_builder_preview();
  void perform_sd_backup();
  void open_sd_restore_modal();
  void close_sd_restore_modal();
  void perform_sd_restore();
  void open_power_modal();
  void close_power_modal();
  bool apply_power_modal();
  void open_manual_time_modal();
  void close_manual_time_modal();
  bool apply_manual_time_modal();
  void open_wifi_modal();
  void close_wifi_modal();
  void refresh_wifi_scan_list();
  bool connect_wifi_from_modal();
  void forget_wifi_credentials();
  void open_ble_modal();
  void close_ble_modal();
  void refresh_ble_modal_status();
  void open_mqtt_modal();
  void close_mqtt_modal();
  bool apply_mqtt_modal();
  bool clear_mqtt_modal();
  bool remove_device_references_from_activities(uint32_t device_id);
  bool remove_command_references_from_activities(uint32_t device_id, const std::string& command_name);
  bool normalize_restored_records(std::vector<DeviceRecord>* devices, std::vector<ActivityRecord>* activities) const;
  void execute_activity_startup_actions(const ActivityRecord& activity);
  void dispatch_remote_command(CommandSlot slot);
  void dispatch_physical_key(char key_char);
  const DeviceRecord* selected_device() const;
  const ActivityRecord* selected_activity() const;

  DeviceRegistry& device_registry_;
  ActivityRegistry& activity_registry_;
  CommandDispatcher& dispatcher_;

  int selected_device_index_ = -1;
  int selected_activity_index_ = -1;
  uint32_t command_editor_device_id_ = 0;

  lv_obj_t* root_ = nullptr;
  lv_obj_t* status_bar_ = nullptr;
  lv_obj_t* wifi_status_label_ = nullptr;
  lv_obj_t* time_status_label_ = nullptr;
  lv_obj_t* battery_status_label_ = nullptr;
  lv_obj_t* tabview_ = nullptr;
  lv_obj_t* devices_page_ = nullptr;
  lv_obj_t* activities_page_ = nullptr;
  lv_obj_t* remote_page_ = nullptr;
  lv_obj_t* settings_page_ = nullptr;

  lv_obj_t* selected_device_label_ = nullptr;
  lv_obj_t* device_list_ = nullptr;
  lv_obj_t* device_add_btn_ = nullptr;
  lv_obj_t* device_remove_btn_ = nullptr;
  lv_obj_t* device_rename_btn_ = nullptr;
  lv_obj_t* device_command_btn_ = nullptr;
  lv_obj_t* device_duplicate_btn_ = nullptr;
  lv_obj_t* device_move_up_btn_ = nullptr;
  lv_obj_t* device_move_down_btn_ = nullptr;

  lv_obj_t* device_modal_ = nullptr;
  lv_obj_t* dd_type_ = nullptr;
  lv_obj_t* dd_transport_ = nullptr;
  lv_obj_t* ta_device_name_ = nullptr;

  lv_obj_t* command_modal_ = nullptr;
  lv_obj_t* dd_command_device_type_ = nullptr;
  lv_obj_t* dd_command_transport_ = nullptr;
  lv_obj_t* dd_command_name_ = nullptr;
  lv_obj_t* ta_command_payload_ = nullptr;
  lv_obj_t* command_hint_label_ = nullptr;
  lv_obj_t* command_template_label_ = nullptr;
  lv_obj_t* dd_command_template_ = nullptr;
  lv_obj_t* dd_command_import_mode_ = nullptr;
  lv_obj_t* command_template_apply_btn_ = nullptr;
  lv_obj_t* command_learn_btn_ = nullptr;
  lv_obj_t* command_learn_status_ = nullptr;
  lv_obj_t* command_name_modal_ = nullptr;
  lv_obj_t* dd_new_command_common_ = nullptr;
  lv_obj_t* ta_new_command_custom_ = nullptr;
  lv_obj_t* rename_modal_ = nullptr;
  lv_obj_t* ta_rename_name_ = nullptr;
  bool rename_activity_target_ = false;

  lv_obj_t* selected_activity_label_ = nullptr;
  lv_obj_t* activity_list_ = nullptr;
  lv_obj_t* activity_add_btn_ = nullptr;
  lv_obj_t* activity_remove_btn_ = nullptr;
  lv_obj_t* activity_rename_btn_ = nullptr;
  lv_obj_t* activity_keymap_btn_ = nullptr;
  lv_obj_t* activity_builder_btn_ = nullptr;
  lv_obj_t* activity_duplicate_btn_ = nullptr;
  lv_obj_t* activity_move_up_btn_ = nullptr;
  lv_obj_t* activity_move_down_btn_ = nullptr;

  lv_obj_t* activity_modal_ = nullptr;
  lv_obj_t* ta_activity_name_ = nullptr;
  lv_obj_t* activity_keymap_modal_ = nullptr;
  lv_obj_t* dd_keymap_key_ = nullptr;
  lv_obj_t* ta_keymap_device_filter_ = nullptr;
  lv_obj_t* dd_keymap_device_ = nullptr;
  lv_obj_t* ta_keymap_command_filter_ = nullptr;
  lv_obj_t* dd_keymap_command_ = nullptr;
  lv_obj_t* keymap_status_label_ = nullptr;
  lv_obj_t* keymap_slot_hint_label_ = nullptr;
  lv_obj_t* keymap_overwrite_modal_ = nullptr;
  lv_obj_t* activity_builder_modal_ = nullptr;
  lv_obj_t* dd_builder_device_ = nullptr;
  lv_obj_t* dd_builder_slot_ = nullptr;
  lv_obj_t* builder_status_label_ = nullptr;
  lv_obj_t* builder_preview_label_ = nullptr;

  lv_obj_t* remote_activity_dd_ = nullptr;
  lv_obj_t* remote_status_ = nullptr;
  lv_obj_t* remote_page_prev_btn_ = nullptr;
  lv_obj_t* remote_page_next_btn_ = nullptr;
  lv_obj_t* remote_page_label_ = nullptr;
  lv_obj_t* settings_status_ = nullptr;
  lv_obj_t* settings_backup_btn_ = nullptr;
  lv_obj_t* settings_restore_btn_ = nullptr;
  lv_obj_t* settings_wifi_btn_ = nullptr;
  lv_obj_t* settings_ble_btn_ = nullptr;
  lv_obj_t* settings_mqtt_btn_ = nullptr;
  lv_obj_t* settings_icons_btn_ = nullptr;
  lv_obj_t* settings_power_btn_ = nullptr;
  lv_obj_t* settings_set_time_btn_ = nullptr;
  lv_obj_t* restore_modal_ = nullptr;
  lv_obj_t* dd_restore_backup_ = nullptr;
  lv_obj_t* power_modal_ = nullptr;
  lv_obj_t* dd_power_sleep_timeout_ = nullptr;
  lv_obj_t* dd_power_debounce_ = nullptr;
  lv_obj_t* sw_power_wakeup_imu_ = nullptr;
  lv_obj_t* time_modal_ = nullptr;
  lv_obj_t* ta_manual_time_ = nullptr;
  lv_obj_t* dd_manual_timezone_ = nullptr;
  lv_obj_t* wifi_modal_ = nullptr;
  lv_obj_t* dd_wifi_network_ = nullptr;
  lv_obj_t* ta_wifi_password_ = nullptr;
  lv_obj_t* wifi_modal_status_ = nullptr;
  lv_obj_t* ble_modal_ = nullptr;
  lv_obj_t* ble_modal_status_ = nullptr;
  lv_obj_t* mqtt_modal_ = nullptr;
  lv_obj_t* ta_mqtt_host_ = nullptr;
  lv_obj_t* ta_mqtt_port_ = nullptr;
  lv_obj_t* ta_mqtt_user_ = nullptr;
  lv_obj_t* ta_mqtt_pass_ = nullptr;
  lv_obj_t* ta_mqtt_client_ = nullptr;
  lv_obj_t* mqtt_modal_status_ = nullptr;

  lv_obj_t* keyboard_ = nullptr;
  unsigned long last_status_update_ms_ = 0;
  bool ir_learning_active_ = false;
  SdBackupService sd_backup_;

  std::vector<lv_obj_t*> device_row_buttons_;
  std::vector<lv_obj_t*> activity_row_buttons_;
  std::vector<uint32_t> keymap_device_ids_;
  std::vector<uint32_t> activity_builder_device_ids_;
  std::vector<ActivityStartupAction> activity_builder_actions_;
  std::vector<std::string> restore_backup_paths_;
  std::vector<std::string> wifi_scan_results_;
  std::vector<uint32_t> remote_device_ids_;
  std::vector<std::string> remote_command_names_;
  std::vector<lv_obj_t*> remote_command_buttons_;
  std::unordered_map<std::string, std::string> remote_icon_overrides_;
  int remote_command_page_index_ = 0;
  char pending_keymap_key_char_ = '\0';
  uint32_t pending_keymap_device_id_ = 0;
  std::string pending_keymap_command_name_;
  std::string manual_time_initial_text_;
  bool wifi_connect_attempt_active_ = false;
  unsigned long wifi_connect_attempt_started_ms_ = 0;
  std::string wifi_connect_target_ssid_;
  std::string last_power_event_;
  bool last_wifi_connected_known_ = false;
  bool last_wifi_connected_ = false;
};

}  // namespace omote_v2
