#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "esphome/components/sprinkler/sprinkler.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"

#ifdef USE_IRRIGATION_FLOW_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_IRRIGATION_NAME_TEXTS
#include "esphome/components/text/text.h"
#endif
#ifdef USE_IRRIGATION_EXPANDER
#include "esphome/components/mcp23x17_base/mcp23x17_base.h"
#endif

#include "buttons.h"
#include "flow.h"
#include "pinmap.h"
#include "planner.h"
#include "runlog.h"
#include "runner.h"
#include "schedule.h"
#include "types.h"

namespace irrigation {

// §5 Settings, persisted as one NVS blob.
struct Settings {
  int16_t master_output;  // output code (pinmap.h)
  bool relay_active_high;
  uint8_t zone_count;  // 1..16
  bool controller_enabled;
  uint8_t monthly_pct[12];  // Jan..Dec, 0..200
  float pulses_per_litre;
  float leak_lpm;
  uint16_t flow_settle_s;
  RainType rain_type;
  float mm_per_tip;
  float rain_skip_mm;
  bool rain_sensor_stops_running;
  uint32_t zone_hard_cap_s;
  ButtonFn start_short_fn;
  ButtonFn start_long_fn;
  ButtonFn stop_short_fn;
  ButtonFn stop_long_fn;
  uint8_t button_program;  // 0 = A
};

inline Settings default_settings() {
  Settings s{};
  s.master_output = kNoOutput;
  s.relay_active_high = true;
  s.zone_count = 6;
  s.controller_enabled = true;
  for (auto &m : s.monthly_pct) m = 100;
  s.pulses_per_litre = 450.0f;
  s.leak_lpm = 0.5f;
  s.flow_settle_s = 60;
  s.rain_type = RainType::NONE;
  s.mm_per_tip = 0.2f;
  s.rain_skip_mm = 3.0f;
  s.rain_sensor_stops_running = true;
  s.zone_hard_cap_s = 7200;
  s.start_short_fn = ButtonFn::RUN_PROGRAM;
  s.start_long_fn = ButtonFn::NEXT_ZONE;
  s.stop_short_fn = ButtonFn::STOP_ALL;
  s.stop_long_fn = ButtonFn::RAIN_DELAY;
  s.button_program = 0;
  return s;
}

}  // namespace irrigation

namespace esphome::irrigation_core {

using irrigation::PinError;

// on_event trigger: (type, json).
class EventTrigger : public Trigger<std::string, std::string> {};

class IrrigationCore;

// Runner -> ESPHome sprinkler adapter.
class SprinklerDriver : public irrigation::ValveDriver {
 public:
  void set_sprinkler(sprinkler::Sprinkler *s) { this->spr_ = s; }
  void start(uint8_t zone, uint32_t run_s) override;
  void stop_all() override;
  // Not idle while a requested start is still pending in the sprinkler state
  // machine (pump/stop delays), so the Runner does not mistake the gap for a stop.
  bool is_idle() const override;
  // True once when a requested valve did not open within kStartPendingMs; the core
  // then ends the Runner step as STOPPED.
  bool take_start_failure();

 protected:
  static constexpr uint32_t kStartPendingMs = 15000;
  sprinkler::Sprinkler *spr_{nullptr};
  mutable bool pending_{false};
  mutable uint32_t pending_since_ms_{0};
};

class IrrigationCore : public Component {
 public:
  static constexpr uint8_t kMasterSlot = irrigation::kZones;  // 16

  IrrigationCore();

  // ---- codegen setters ----
  void set_sprinkler(sprinkler::Sprinkler *s) {
    this->sprinkler_ = s;
    this->driver_.set_sprinkler(s);
  }
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
#ifdef USE_IRRIGATION_FLOW_SENSOR
  void set_flow_sensor(sensor::Sensor *s) { this->flow_sensor_ = s; }
#endif
#ifdef USE_IRRIGATION_EXPANDER
  void set_expander(mcp23x17_base::MCP23X17Base *e) {
    this->expander_ = e;
    this->board_.has_expander = true;
  }
#endif
#ifdef USE_IRRIGATION_NAME_TEXTS
  void set_program_name_text(uint8_t i, text::Text *t) {
    if (i < irrigation::kPrograms) this->program_name_texts_[i] = t;
  }
  void set_zone_name_text(uint8_t i, text::Text *t) {
    if (i < irrigation::kZones) this->zone_name_texts_[i] = t;
  }
#endif
  void add_output_candidate(uint8_t pin) { this->board_.candidates.push_back(pin); }
  void add_reserved_pin(uint8_t pin) { this->board_.reserved.push_back(pin); }
  void add_default_output(int16_t code) { this->default_outputs_.push_back(code); }
  void set_default_master_output(int16_t code) { this->default_master_output_ = code; }
  void add_on_event_trigger(EventTrigger *t) { this->event_triggers_.push_back(t); }

  // ---- Component ----
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // ---- program / zone / settings access ----
  irrigation::Program &program(uint8_t i);
  void save_program(uint8_t i);
  // anchor = today + days_until_first (needs a valid clock).
  void set_program_interval_offset(uint8_t i, uint8_t days_until_first);
  irrigation::Zone &zone(uint8_t i);
  void save_zone(uint8_t i);
  irrigation::Settings &settings() { return this->settings_; }
  void save_settings();

  // ---- outputs ----
  PinError assign_zone_output(uint8_t zone, int16_t code);
  PinError assign_master_output(int16_t code);
  PinError set_relay_active_high(bool v);
  // slot 0..15 zones, 16 master. Called by the output switch.
  void write_output(uint8_t slot, bool on);
  int16_t output_code(uint8_t slot) const;
  std::string last_error() const { return this->last_error_; }
  // Option strings for the output select (stable for the lifetime of the program).
  const std::vector<const char *> &output_option_ptrs() const { return this->option_ptrs_; }
  // Called with (slot, label) after a slot's output changes.
  void add_on_output_changed_callback(std::function<void(uint8_t, const std::string &)> &&cb) {
    this->output_changed_cbs_.push_back(std::move(cb));
  }

  // ---- commands ----
  void run_zone(uint8_t zone, uint32_t seconds);
  void run_program(uint8_t p);
  void stop_all();
  void set_rain_delay_hours(uint16_t h);
  void set_weather_adjust(uint16_t pct, bool skip, const std::string &reason, uint16_t valid_hours);
  void learn_flow(uint8_t zone);
  void rain_tip();
  // raw_state = rain binary sensor state (true = contact closed).
  void set_rain_switch(bool raw_state);
  void next_zone();
  bool rain_delay_active() const;
  void clear_rain_delay();
  void button_press(bool is_start, bool is_long);
  bool status_led_level() const;

  // ---- API actions ----
  // Arguments are range-clamped (WARN when changed). An invalid index, array length
  // or output label rejects the whole call instead: WARN, Last Error set, nothing
  // changed. Each successful set_* saves, which emits config_changed once.
  void api_run_zone(int zone, int minutes);
  void api_run_program(int program);
  void api_rain_delay(int hours);
  void api_set_weather_adjust(int pct, bool skip, const std::string &reason, int valid_hours);
  bool api_set_program(int program, const std::string &name, bool enabled, int mode, int weekday_mask,
                       int interval_days, int days_until_first, const std::vector<int32_t> &starts,
                       const std::vector<int32_t> &zone_minutes, int seasonal_pct, bool cycle_soak,
                       bool weather_adjust);
  bool api_set_zone(int zone, const std::string &name, bool enabled, int head, float precip, int soil, int slope,
                    int sun, int plant, float area, int max_cycle_min, int soak_min, float baseline, int high_pct,
                    int low_pct, int action, const std::string &output);
  bool api_set_settings(int zone_count, bool controller_enabled, const std::vector<int32_t> &monthly_pct,
                        float pulses_per_litre, float leak_lpm, int settle_s, int rain_type, float mm_per_tip,
                        float rain_skip_mm, bool rain_stops, int hard_cap_min, int start_short_fn,
                        int start_long_fn, int stop_short_fn, int stop_long_fn, int button_program,
                        const std::string &master_output, bool relay_active_high);
  // Days until the interval anchor (0 when the clock is invalid).
  int program_days_until_first(uint8_t p) const;
  uint32_t runlog_last_seq() const { return this->runlog_.last_seq(); }

  // ---- names (the Program N Name / Zone N Name text entities when configured) ----
  std::string program_name(uint8_t p) const { return this->program_name_(p); }
  std::string zone_name(uint8_t z) const { return this->zone_name_(z); }
  void set_program_name(uint8_t p, const std::string &name);
  void set_zone_name(uint8_t z, const std::string &name);

  // ---- JSON ----
  // days is clamped to 1..14; at most 256 runs are emitted (*truncated says if more existed).
  std::string plan_json(int days, bool *truncated = nullptr);
  std::string runlog_json(uint32_t since_seq);
  std::string config_json();

  // ---- status ----
  bool clock_valid() const;
  bool rain_hold() const;
  std::string status_text() const;
  uint16_t current_weather_pct() const;
  int32_t next_run_ts() const;  // 0 = none / clock invalid
  float zone_litres_total(uint8_t z) const;
  float total_litres() const { return this->litres_.total; }
  float rain_mm_24h() const;
  uint32_t remaining_s() const;  // of the active valve
  std::string active_zone_name() const;
  std::string active_program_name() const;
  bool leak_active() const { return this->leak_active_; }
  bool flow_alarm_active() const { return this->flow_alarm_active_; }

 protected:
  struct Programs {
    irrigation::Program p[irrigation::kPrograms];
  };
  struct Zones {
    irrigation::Zone z[irrigation::kZones];
  };
  struct Litres {
    float zone[irrigation::kZones];
    float total;
  };
  struct Clock {
    bool valid;
    uint32_t ts;    // UTC epoch
    int32_t day;    // local days since 1970-01-01
    int16_t minute; // local minute of day
    uint8_t second;
    uint8_t month;  // 1..12
  };
  struct InstanceAcc {
    uint32_t id{0};
    uint8_t program{0};
    uint32_t first_ts{0};
    uint32_t act_s{0};
    float litres{0};
    bool warn{false};
  };
  struct PendingRun {
    uint16_t pct{100};
    uint32_t plan_s{0};
    uint8_t zones{0};
  };

  Clock clock_now_() const;
  uint32_t uptime_s_() const;
  uint32_t ts_of_uptime_(uint32_t up_s) const;
  void tick_(uint32_t dt_ms);
  void flush_events_();
  void emit_(const char *type, std::string json);

  PinError assign_output_(uint8_t slot, int16_t code);
  std::string output_error_text_(uint8_t slot, int16_t code, PinError e) const;
  bool parse_output_(const std::string &label, int16_t &code) const;
  bool reject_(const std::string &msg);
  void hw_configure_off_(int16_t code);
  void hw_write_(int16_t code, bool level);
  void hw_park_(int16_t code);
  void repark_released_();
  std::string slot_name_(uint8_t slot) const;
  void notify_output_changed_(uint8_t slot);
  bool watering_() const;

  std::string program_name_(uint8_t p) const;
  std::string zone_name_(uint8_t z) const;
  bool enqueue_(uint8_t p, uint16_t pct, bool manual, uint32_t now_s);
  void save_litres_();
  void save_weather_();
  void save_rain_delay_();
  void trip_stop_(std::optional<uint8_t> az, irrigation::Result r, uint32_t now_s);
  void alert_(const char *kind, uint8_t zone, float flow, float limit);
  float flow_lpm_() const;
  bool has_flow_sensor_() const;

  // Runner callbacks
  void on_step_started_(const irrigation::StepEvent &ev);
  void on_step_finished_(const irrigation::StepEvent &ev);
  void on_program_finished_(uint32_t instance, uint8_t program);

  sprinkler::Sprinkler *sprinkler_{nullptr};
  time::RealTimeClock *time_{nullptr};
#ifdef USE_IRRIGATION_FLOW_SENSOR
  sensor::Sensor *flow_sensor_{nullptr};
#endif
#ifdef USE_IRRIGATION_EXPANDER
  mcp23x17_base::MCP23X17Base *expander_{nullptr};
#endif
#ifdef USE_IRRIGATION_NAME_TEXTS
  text::Text *program_name_texts_[irrigation::kPrograms]{};
  text::Text *zone_name_texts_[irrigation::kZones]{};
#endif

  irrigation::BoardPins board_{{}, {}, false};
  std::vector<int16_t> default_outputs_;
  int16_t default_master_output_{irrigation::kNoOutput};
  std::vector<EventTrigger *> event_triggers_;
  std::vector<std::pair<std::string, std::string>> pending_events_;
  std::vector<std::string> options_;
  std::vector<const char *> option_ptrs_;
  std::vector<std::function<void(uint8_t, const std::string &)>> output_changed_cbs_;

  Programs programs_{};
  Zones zones_{};
  irrigation::Settings settings_{};
  irrigation::WeatherAdjust weather_{};
  uint32_t rain_delay_until_ts_{0};
  Litres litres_{};

  ESPPreferenceObject pref_programs_, pref_zones_, pref_settings_, pref_weather_, pref_rain_delay_,
      pref_litres_;

  SprinklerDriver driver_;
  irrigation::Runner runner_;
  irrigation::Scheduler scheduler_;
  irrigation::FlowMonitor flow_;
  irrigation::SafetyMonitor safety_;
  irrigation::RainWindow rain_window_;
  irrigation::RunLog runlog_;

  // Codes actually configured in hardware (the Zone/Settings fields mirror these;
  // a save_* with a changed output field is routed through the assignment path).
  int16_t applied_[irrigation::kZones + 1];
  bool applied_active_high_{true};
  bool ready_{false};
  std::string last_error_;
  uint32_t last_tick_ms_{0};
  uint32_t led_ack_until_ms_{0};
  bool rain_switch_raw_{false};
  bool clock_alert_sent_{false};
  bool leak_active_{false};
  bool flow_alarm_active_{false};
  bool litres_dirty_{false};
  uint32_t last_litres_save_s_{0};

  // Valve tracking (from sprinkler.active_valve(), so manual toggles count).
  // Stop actions (max runtime, flow stop) fire at most once per valve opening.
  bool trip_latched_{false};
  std::optional<uint8_t> trip_zone_;
  // Returned by program()/zone() for an out-of-range index.
  irrigation::Program scratch_program_{};
  irrigation::Zone scratch_zone_{};
  bool flow_zone_open_{false};
  uint8_t flow_zone_{0};

  // Runner step / instance bookkeeping.
  bool step_active_{false};
  uint8_t step_zone_{0};
  float step_litres_{0};
  InstanceAcc inst_;
  PendingRun pending_[irrigation::kPrograms + 1];  // index kPrograms = manual
};

}  // namespace esphome::irrigation_core
