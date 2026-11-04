#pragma once

#include "esphome/components/irrigation_core/irrigation_core.h"
#include "esphome/components/select/select.h"
#include "esphome/core/component.h"

namespace esphome::irrigation_core {

// Output picker for one slot (0..15 zones, 16 master). control() validates through
// the core; on error the old label is re-published so Home Assistant snaps back.
class IrrigationOutputSelect : public select::Select, public Component {
 public:
  void set_parent(IrrigationCore *parent) { this->parent_ = parent; }
  // Retarget (e.g. a zone editor select following the selected zone) and re-publish.
  void set_slot(uint8_t slot);
  uint8_t get_slot() const { return this->slot_; }

  void setup() override;
  void dump_config() override;

 protected:
  void control(size_t index) override;
  void publish_current_();

  IrrigationCore *parent_{nullptr};
  uint8_t slot_{0};
  bool ready_{false};
};

}  // namespace esphome::irrigation_core
