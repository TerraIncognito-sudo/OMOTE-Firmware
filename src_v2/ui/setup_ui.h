#pragma once

#include <string>
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

 private:
  static void on_device_add_clicked(lv_event_t* event);
  static void on_device_remove_clicked(lv_event_t* event);
  static void on_device_rename_clicked(lv_event_t* event);
  static void on_device_clicked(lv_event_t* event);
  static void on_device_modal_close(lv_event_t* event);
  static void on_device_modal_save(lv_event_t* event);
  static void on_device_command_edit(lv_event_t* event);
  static void on_command_modal_close(lv_event_t* event);
  static void on_command_modal_save(lv_event_t* event);
  static void on_command_modal_remove(lv_event_t* event);
  static void on_command_modal_test(lv_event_t* event);
  static void on_command_modal_learn(lv_event_t* event);
  static void on_command_name_changed(lv_event_t* event);
  static void on_command_name_modal_close(lv_event_t* event);
  static void on_command_name_modal_save(lv_event_t* event);
  static void on_rename_modal_close(lv_event_t* event);
  static void on_rename_modal_save(lv_event_t* event);
  static void on_activity_keymap_device_changed(lv_event_t* event);
  static void on_activity_keymap_command_changed(lv_event_t* event);
  static void on_textarea_focus(lv_event_t* event);
  static void on_activity_clicked(lv_event_t* event);
  static void on_activity_add_clicked(lv_event_t* event);
  static void on_activity_remove_clicked(lv_event_t* event);
  static void on_activity_rename_clicked(lv_event_t* event);
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
  static void on_settings_format_clicked(lv_event_t* event);
  static void on_remote_activity_changed(lv_event_t* event);
  static void on_remote_command_clicked(lv_event_t* event);
  static void on_keyboard_apply(lv_event_t* event);

  void build_devices_tab();
  void build_activities_tab();
  void build_remote_tab();
  void build_settings_tab();
  void update_status_bar();
  void rebuild_device_list();
  void rebuild_activity_list();
  void rebuild_remote_activity_dropdown();
  void open_device_modal();
  void close_device_modal();
  bool save_device_modal();
  void open_command_modal();
  void close_command_modal();
  bool save_command_modal();
  bool remove_command_modal();
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
  void refresh_activity_keymap_device_dropdown();
  void refresh_activity_keymap_command_dropdown();
  void refresh_activity_keymap_binding_hint();
  void open_activity_builder_modal();
  void close_activity_builder_modal();
  void activity_builder_add_step();
  void activity_builder_clear();
  void activity_builder_save();
  void refresh_activity_builder_preview();
  void perform_sd_backup();
  void perform_sd_restore();
  void perform_sd_format();
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

  lv_obj_t* device_modal_ = nullptr;
  lv_obj_t* dd_type_ = nullptr;
  lv_obj_t* dd_transport_ = nullptr;
  lv_obj_t* ta_device_name_ = nullptr;

  lv_obj_t* command_modal_ = nullptr;
  lv_obj_t* dd_command_name_ = nullptr;
  lv_obj_t* ta_command_payload_ = nullptr;
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

  lv_obj_t* activity_modal_ = nullptr;
  lv_obj_t* ta_activity_name_ = nullptr;
  lv_obj_t* activity_device_list_ = nullptr;
  lv_obj_t* activity_keymap_modal_ = nullptr;
  lv_obj_t* dd_keymap_key_ = nullptr;
  lv_obj_t* dd_keymap_device_ = nullptr;
  lv_obj_t* dd_keymap_command_ = nullptr;
  lv_obj_t* keymap_status_label_ = nullptr;
  lv_obj_t* keymap_slot_hint_label_ = nullptr;
  lv_obj_t* activity_builder_modal_ = nullptr;
  lv_obj_t* dd_builder_device_ = nullptr;
  lv_obj_t* dd_builder_slot_ = nullptr;
  lv_obj_t* builder_status_label_ = nullptr;
  lv_obj_t* builder_preview_label_ = nullptr;

  lv_obj_t* remote_activity_dd_ = nullptr;
  lv_obj_t* remote_status_ = nullptr;
  lv_obj_t* settings_status_ = nullptr;
  lv_obj_t* settings_backup_btn_ = nullptr;
  lv_obj_t* settings_restore_btn_ = nullptr;
  lv_obj_t* settings_format_btn_ = nullptr;

  lv_obj_t* keyboard_ = nullptr;
  unsigned long last_status_update_ms_ = 0;
  bool ir_learning_active_ = false;
  SdBackupService sd_backup_;

  std::vector<lv_obj_t*> device_row_buttons_;
  std::vector<lv_obj_t*> activity_row_buttons_;
  std::vector<lv_obj_t*> activity_device_checkboxes_;
  std::vector<uint32_t> activity_device_checkbox_ids_;
  std::vector<uint32_t> keymap_device_ids_;
  std::vector<uint32_t> activity_builder_device_ids_;
  std::vector<ActivityStartupAction> activity_builder_actions_;
};

}  // namespace omote_v2
