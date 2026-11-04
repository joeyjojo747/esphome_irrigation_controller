#include "irrigation_output_select.h"

#include "esphome/core/log.h"

namespace esphome::irrigation_core {

static const char *const TAG = "irrigation_core.select";

void IrrigationOutputSelect::setup() {
  // The core (setup_priority HARDWARE) has built the option strings by now; they
  // live as long as the core, and the pointer list is copied into the traits.
  const auto &opts = this->parent_->output_option_ptrs();
  this->traits.set_options(select::SelectOptions(opts.data(), opts.size()));
  this->parent_->add_on_output_changed_callback([this](uint8_t slot, const std::string &label) {
    if (slot == this->slot_) this->publish_state(label);
  });
  this->ready_ = true;
  this->publish_current_();
}

void IrrigationOutputSelect::set_slot(uint8_t slot) {
  this->slot_ = slot;
  if (this->ready_) this->publish_current_();
}

void IrrigationOutputSelect::publish_current_() {
  this->publish_state(irrigation::output_label(this->parent_->output_code(this->slot_)));
}

void IrrigationOutputSelect::dump_config() {
  LOG_SELECT("", "Irrigation output select", this);
  if (this->slot_ == IrrigationCore::kMasterSlot) {
    ESP_LOGCONFIG(TAG, "  Slot: master");
  } else {
    ESP_LOGCONFIG(TAG, "  Slot: zone %u", this->slot_ + 1);
  }
}

void IrrigationOutputSelect::control(size_t index) {
  const int16_t code = irrigation::parse_output_label(this->option_at(index));
  const PinError e = this->slot_ == IrrigationCore::kMasterSlot ? this->parent_->assign_master_output(code)
                                                                  : this->parent_->assign_zone_output(this->slot_, code);
  // Success publishes through the output-changed callback; on error re-publish the old label.
  if (e != PinError::OK) this->publish_current_();
}

}  // namespace esphome::irrigation_core
