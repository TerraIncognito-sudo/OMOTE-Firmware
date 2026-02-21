void init_battery_HAL(void) {};

void get_battery_status_HAL(int *battery_voltage, int *battery_percentage, bool *battery_ischarging) {
  *battery_voltage = 3950;
  *battery_percentage = 50;
  *battery_ischarging = false; 
}

bool battery_is_charge_control_available_HAL(void) {
  return false;
}

bool set_battery_charging_enabled_HAL(bool enabled) {
  (void)enabled;
  return false;
}
