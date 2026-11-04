#include "flow.h"

namespace irrigation {

void FlowMonitor::zone_started(uint8_t zone, float baseline_lpm, uint8_t high_pct, uint8_t low_pct, uint32_t now_s) {
  active_ = true;
  zone_ = zone;
  baseline_ = baseline_lpm;
  high_pct_ = high_pct;
  low_pct_ = low_pct;
  start_s_ = now_s;
  sum_ = 0.0;
  count_ = 0;
  raised_ = false;
  band_ = FlowEvent::NONE;
  band_since_ = 0;
  leak_over_ = false;
  leak_raised_ = false;
}

std::optional<float> FlowMonitor::zone_stopped(uint32_t now_s) {
  std::optional<float> result;
  if (active_) {
    uint32_t elapsed = now_s - start_s_;
    bool enough = elapsed >= static_cast<uint32_t>(s_.settle_s) + s_.min_learn_s && count_ > 0;
    if (enough && !raised_) {
      float avg = static_cast<float>(sum_ / count_);
      result = baseline_ > 0.0f ? 0.7f * baseline_ + 0.3f * avg : avg;
    }
  }
  active_ = false;
  drain_until_ = now_s + s_.drain_s;
  leak_over_ = false;
  leak_raised_ = false;
  return result;
}

FlowEvent FlowMonitor::update(float flow_lpm, uint32_t now_s) {
  if (active_) {
    if (now_s - start_s_ < s_.settle_s) return FlowEvent::NONE;
    sum_ += flow_lpm;
    count_++;
    if (raised_ || baseline_ <= 0.0f) return FlowEvent::NONE;
    FlowEvent band = FlowEvent::NONE;
    if (flow_lpm > baseline_ * high_pct_ / 100.0f)
      band = FlowEvent::HIGH;
    else if (flow_lpm < baseline_ * low_pct_ / 100.0f)
      band = FlowEvent::LOW;
    if (band == FlowEvent::NONE) {
      band_ = FlowEvent::NONE;
      return FlowEvent::NONE;
    }
    if (band != band_) {
      band_ = band;
      band_since_ = now_s;
    }
    if (now_s - band_since_ >= s_.sustain_s) {
      raised_ = true;
      return band;
    }
    return FlowEvent::NONE;
  }

  // No zone running: leak detection once the line has drained.
  if (now_s < drain_until_) return FlowEvent::NONE;
  if (flow_lpm > s_.leak_lpm) {
    if (!leak_over_) {
      leak_over_ = true;
      leak_since_ = now_s;
    }
    if (!leak_raised_ && now_s - leak_since_ >= s_.leak_hold_s) {
      leak_raised_ = true;
      return FlowEvent::LEAK;
    }
  } else {
    leak_over_ = false;
    leak_raised_ = false;
  }
  return FlowEvent::NONE;
}

bool SafetyMonitor::exceeded(std::optional<uint8_t> active_zone, uint32_t now_s, uint32_t cap_s) {
  if (!active_zone) {
    tracking_ = false;
    return false;
  }
  if (!tracking_ || zone_ != *active_zone) {
    tracking_ = true;
    zone_ = *active_zone;
    since_ = now_s;
  }
  return now_s - since_ >= cap_s + 10;
}

void RainWindow::add_tip(uint32_t now_s) {
  uint32_t h = now_s / 3600;
  int i = h % kBuckets;
  if (!valid_[i] || hour_[i] != h) {
    valid_[i] = true;
    hour_[i] = h;
    tips_[i] = 0;
  }
  tips_[i]++;
}

float RainWindow::mm_last_24h(uint32_t now_s, float mm_per_tip) const {
  uint32_t h = now_s / 3600;
  uint32_t total = 0;
  for (int i = 0; i < kBuckets; i++) {
    if (valid_[i] && hour_[i] <= h && h - hour_[i] < kBuckets) total += tips_[i];
  }
  return total * mm_per_tip;
}

}  // namespace irrigation
