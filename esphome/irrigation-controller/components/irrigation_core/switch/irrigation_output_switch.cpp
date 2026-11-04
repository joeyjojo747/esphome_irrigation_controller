#include "irrigation_output_switch.h"

#include "esphome/core/log.h"

namespace esphome::irrigation_core {

static const char *const TAG = "irrigation_core.switch";

void IrrigationOutputSwitch::setup() {
  bool initial = this->get_initial_state_with_restore_mode().value_or(false);
  if (initial) {
    this->turn_on();
  } else {
    this->turn_off();
  }
}

void IrrigationOutputSwitch::dump_config() {
  LOG_SWITCH("", "Irrigation output switch", this);
  if (this->slot_ == IrrigationCore::kMasterSlot) {
    ESP_LOGCONFIG(TAG, "  Slot: master");
  } else {
    ESP_LOGCONFIG(TAG, "  Slot: zone %u", this->slot_ + 1);
  }
}

void IrrigationOutputSwitch::write_state(bool state) {
  this->parent_->write_output(this->slot_, state);
  this->publish_state(state);
}

}  // namespace esphome::irrigation_core
