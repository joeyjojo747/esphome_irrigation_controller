#pragma once

#include "esphome/components/irrigation_core/irrigation_core.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"

namespace esphome::irrigation_core {

// Relay slot switch used by the sprinkler as a valve or pump switch.
// slot 0..15 = zones, 16 = master.
class IrrigationOutputSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(IrrigationCore *parent) { this->parent_ = parent; }
  void set_slot(uint8_t slot) { this->slot_ = slot; }
  uint8_t get_slot() const { return this->slot_; }

  void setup() override;
  void dump_config() override;
  // After the core (HARDWARE), which drives every output off first.
  float get_setup_priority() const override { return setup_priority::HARDWARE - 1.0f; }

 protected:
  void write_state(bool state) override;

  IrrigationCore *parent_{nullptr};
  uint8_t slot_{0};
};

}  // namespace esphome::irrigation_core
