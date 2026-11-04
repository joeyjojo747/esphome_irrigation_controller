#pragma once
#include <cstdint>
#include <optional>

namespace irrigation {

enum class FlowEvent : uint8_t { NONE, HIGH, LOW, LEAK };

struct FlowSettings {
  uint16_t settle_s = 60;
  float leak_lpm = 0.5f;
  uint16_t leak_hold_s = 60;
  uint16_t drain_s = 60;
  uint16_t sustain_s = 30;
  uint16_t min_learn_s = 120;
};

class FlowMonitor {
 public:
  void configure(const FlowSettings& s) { s_ = s; }
  void zone_started(uint8_t zone, float baseline_lpm, uint8_t high_pct, uint8_t low_pct, uint32_t now_s);
  // Returns the new baseline if learnable (EMA 0.7*old + 0.3*avg), else nullopt.
  std::optional<float> zone_stopped(uint32_t now_s);
  FlowEvent update(float flow_lpm, uint32_t now_s);  // call every second

 private:
  FlowSettings s_;
  bool active_ = false;
  uint8_t zone_ = 0;
  float baseline_ = 0.0f;
  uint8_t high_pct_ = 130, low_pct_ = 70;
  uint32_t start_s_ = 0;
  double sum_ = 0.0;
  uint32_t count_ = 0;
  bool raised_ = false;
  FlowEvent band_ = FlowEvent::NONE;
  uint32_t band_since_ = 0;
  // leak
  uint32_t drain_until_ = 0;
  bool leak_over_ = false;
  uint32_t leak_since_ = 0;
  bool leak_raised_ = false;
};

class SafetyMonitor {
 public:
  bool exceeded(std::optional<uint8_t> active_zone, uint32_t now_s, uint32_t cap_s);

 private:
  bool tracking_ = false;
  uint8_t zone_ = 0;
  uint32_t since_ = 0;
};

class RainWindow {
 public:
  void add_tip(uint32_t now_s);
  float mm_last_24h(uint32_t now_s, float mm_per_tip) const;

 private:
  static constexpr int kBuckets = 24;
  bool valid_[kBuckets] = {};
  uint32_t hour_[kBuckets] = {};
  uint32_t tips_[kBuckets] = {};
};

}  // namespace irrigation
