#include "irrigation_core.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "civil.h"

namespace esphome::irrigation_core {

static const char *const TAG = "irrigation_core";

using irrigation::kManual;
using irrigation::kNoOutput;
using irrigation::kPrograms;
using irrigation::kStarts;
using irrigation::kZones;
using irrigation::Result;

namespace {

void json_escape_append(std::string &out, const std::string &s) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

std::string fmt_float(float v, int decimals) {
  if (!std::isfinite(v)) v = 0.0f;
  char buf[32];
  snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(v));
  return buf;
}

const char *b(bool v) { return v ? "true" : "false"; }

bool warn_result(Result r) {
  return r == Result::FLOW_HIGH || r == Result::FLOW_LOW || r == Result::MAX_RUNTIME || r == Result::STOPPED;
}

}  // namespace

// ---------------------------------------------------------------- SprinklerDriver

void SprinklerDriver::start(uint8_t zone, uint32_t run_s) {
  this->pending_ = true;
  this->pending_since_ms_ = millis();
  this->spr_->start_single_valve(optional<size_t>(static_cast<size_t>(zone)), optional<uint32_t>(run_s));
}

void SprinklerDriver::stop_all() {
  this->pending_ = false;
  this->spr_->shutdown(true);
}

bool SprinklerDriver::is_idle() const {
  if (this->spr_->active_valve().has_value()) {
    this->pending_ = false;  // the valve opened: from now on idle means it closed
    return false;
  }
  // A start still pending in the sprinkler FSM (stop/pump delays) is not a stop.
  // A start that never opens is not reported idle here (that could read as
  // COMPLETED for a short step): take_start_failure() reports it instead.
  return !this->pending_;
}

bool SprinklerDriver::take_start_failure() {
  if (!this->pending_) return false;
  if (this->spr_->active_valve().has_value()) {
    this->pending_ = false;
    return false;
  }
  if (millis() - this->pending_since_ms_ < kStartPendingMs) return false;
  this->pending_ = false;
  return true;
}

// ---------------------------------------------------------------- setup / loop

IrrigationCore::IrrigationCore() : runner_(&driver_) {
  for (auto &a : this->applied_) a = kNoOutput;
}

void IrrigationCore::setup() {
  this->pref_programs_ = global_preferences->make_preference<Programs>(fnv1_hash("irr_programs"), true);
  this->pref_zones_ = global_preferences->make_preference<Zones>(fnv1_hash("irr_zones"), true);
  this->pref_settings_ = global_preferences->make_preference<irrigation::Settings>(fnv1_hash("irr_settings"), true);
  this->pref_weather_ = global_preferences->make_preference<irrigation::WeatherAdjust>(fnv1_hash("irr_weather"), true);
  this->pref_rain_delay_ = global_preferences->make_preference<uint32_t>(fnv1_hash("irr_rain_delay"), true);
  this->pref_litres_ = global_preferences->make_preference<Litres>(fnv1_hash("irr_litres"), true);

  if (!this->pref_programs_.load(&this->programs_)) {
    ESP_LOGI(TAG, "No stored programs, using defaults");
    for (auto &p : this->programs_.p) p = irrigation::default_program();
  }
  if (!this->pref_zones_.load(&this->zones_)) {
    ESP_LOGI(TAG, "No stored zones, using default outputs");
    for (int i = 0; i < kZones; i++) {
      this->zones_.z[i] = irrigation::default_zone();
      this->zones_.z[i].output = i < (int) this->default_outputs_.size() ? this->default_outputs_[i] : kNoOutput;
    }
  }
  if (!this->pref_settings_.load(&this->settings_)) {
    ESP_LOGI(TAG, "No stored settings, using defaults");
    this->settings_ = irrigation::default_settings();
    this->settings_.master_output = this->default_master_output_;
    size_t n = this->default_outputs_.size();
    this->settings_.zone_count = static_cast<uint8_t>(n < 1 ? 1 : (n > (size_t) kZones ? kZones : n));
  }
  if (!this->pref_weather_.load(&this->weather_)) {
    this->weather_ = irrigation::WeatherAdjust{100, false, {0}, 0};
  }
  this->weather_.reason[sizeof(this->weather_.reason) - 1] = '\0';
  if (!this->pref_rain_delay_.load(&this->rain_delay_until_ts_)) this->rain_delay_until_ts_ = 0;
  if (!this->pref_litres_.load(&this->litres_)) this->litres_ = Litres{};
  for (auto &l : this->litres_.zone)
    if (!std::isfinite(l) || l < 0) l = 0;
  if (!std::isfinite(this->litres_.total) || this->litres_.total < 0) this->litres_.total = 0;

  // Sanitise settings that index arrays.
  if (this->settings_.zone_count < 1) this->settings_.zone_count = 1;
  if (this->settings_.zone_count > kZones) this->settings_.zone_count = kZones;
  if (this->settings_.button_program >= kPrograms) this->settings_.button_program = 0;
  this->applied_active_high_ = this->settings_.relay_active_high;

  // Re-validate stored outputs against this firmware's board pins, then drive every
  // assigned output to the off level before any other component (sprinkler, scheduler) runs.
  int16_t assigned[kZones + 1];
  for (auto &a : assigned) a = kNoOutput;
  for (uint8_t slot = 0; slot <= kMasterSlot; slot++) {
    int16_t code = slot < kZones ? this->zones_.z[slot].output : this->settings_.master_output;
    PinError e = irrigation::validate_assignment(slot, code, assigned, this->board_);
    if (e != PinError::OK) {
      ESP_LOGW(TAG, "%s: stored output %s rejected (%s); set to None", this->slot_name_(slot).c_str(),
               irrigation::output_label(code).c_str(), irrigation::pin_error_text(e));
      code = kNoOutput;
    }
    assigned[slot] = code;
    this->applied_[slot] = code;
    if (slot < kZones)
      this->zones_.z[slot].output = code;
    else
      this->settings_.master_output = code;
    this->hw_configure_off_(code);
  }

  this->options_ = irrigation::output_options(this->board_.candidates, this->board_.has_expander);
  this->option_ptrs_.clear();
  for (const auto &o : this->options_) this->option_ptrs_.push_back(o.c_str());

  irrigation::FlowSettings fs;
  fs.settle_s = this->settings_.flow_settle_s;
  fs.leak_lpm = this->settings_.leak_lpm;
  this->flow_.configure(fs);

  this->runner_.on_step_started = [this](const irrigation::StepEvent &ev) { this->on_step_started_(ev); };
  this->runner_.on_step_finished = [this](const irrigation::StepEvent &ev) { this->on_step_finished_(ev); };
  this->runner_.on_program_finished = [this](uint32_t inst, uint8_t p) { this->on_program_finished_(inst, p); };

  // Never use 0 as a "no ack" sentinel: millis() wraps after ~49.7 days.
  this->led_ack_until_ms_ = millis();
  this->last_tick_ms_ = millis();
  this->last_litres_save_s_ = this->uptime_s_();
  this->ready_ = true;
}

void IrrigationCore::dump_config() {
  ESP_LOGCONFIG(TAG, "Irrigation core:");
  ESP_LOGCONFIG(TAG, "  Expander: %s", YESNO(this->board_.has_expander));
  ESP_LOGCONFIG(TAG, "  Flow sensor: %s", YESNO(this->has_flow_sensor_()));
  ESP_LOGCONFIG(TAG, "  Zone count: %u, relay active %s", this->settings_.zone_count,
                this->applied_active_high_ ? "high" : "low");
  for (uint8_t slot = 0; slot <= kMasterSlot; slot++) {
    if (this->applied_[slot] == kNoOutput) continue;
    ESP_LOGCONFIG(TAG, "  %s -> %s", this->slot_name_(slot).c_str(),
                  irrigation::output_label(this->applied_[slot]).c_str());
  }
}

void IrrigationCore::loop() {
  this->flush_events_();
  const uint32_t now_ms = millis();
  const uint32_t dt = now_ms - this->last_tick_ms_;
  if (dt >= 1000) {
    this->last_tick_ms_ = now_ms;
    this->tick_(dt);
    this->flush_events_();
  }
}

uint32_t IrrigationCore::uptime_s_() const { return static_cast<uint32_t>(esp_timer_get_time() / 1000000LL); }

IrrigationCore::Clock IrrigationCore::clock_now_() const {
  Clock c{};
  if (this->time_ == nullptr) return c;
  ESPTime t = this->time_->now();
  if (!t.is_valid()) return c;
  c.valid = true;
  c.ts = static_cast<uint32_t>(t.timestamp);
  c.day = irrigation::days_from_civil(t.year, t.month, t.day_of_month);
  c.minute = static_cast<int16_t>(t.hour * 60 + t.minute);
  c.second = t.second;
  c.month = t.month;
  return c;
}

uint32_t IrrigationCore::ts_of_uptime_(uint32_t up_s) const {
  Clock c = this->clock_now_();
  if (!c.valid) return 0;
  return c.ts - (this->uptime_s_() - up_s);
}

bool IrrigationCore::has_flow_sensor_() const {
#ifdef USE_IRRIGATION_FLOW_SENSOR
  return this->flow_sensor_ != nullptr;
#else
  return false;
#endif
}

float IrrigationCore::flow_lpm_() const {
#ifdef USE_IRRIGATION_FLOW_SENSOR
  if (this->flow_sensor_ != nullptr) {
    float f = this->flow_sensor_->state;
    if (std::isfinite(f) && f > 0) return f;
  }
#endif
  return 0.0f;
}

void IrrigationCore::tick_(uint32_t dt_ms) {
  const uint32_t now_s = this->uptime_s_();
  const Clock c = this->clock_now_();
  const auto &st = this->settings_;

  // 1-2. Scheduled starts.
  auto due = this->scheduler_.tick(irrigation::LocalDateTime{c.day, c.minute}, c.valid, this->programs_.p);
  for (const auto &d : due) {
    const irrigation::Program &p = this->programs_.p[d.program];
    const uint16_t wpct = irrigation::active_weather_pct(this->weather_, p, c.ts);
    const uint16_t pct = irrigation::effective_pct(p.seasonal_pct, st.monthly_pct[(c.month + 11) % 12], wpct);
    irrigation::StartContext ctx{st.controller_enabled, this->rain_delay_active(), this->rain_hold(),
                                 irrigation::active_weather_skip(this->weather_, c.ts)};
    const auto dec = irrigation::decide_start(p, ctx, pct);
    if (dec != irrigation::StartDecision::RUN) {
      auto steps = irrigation::expand_program(p, this->zones_.z, st.zone_count, pct, st.zone_hard_cap_s);
      uint32_t planned = 0;
      for (const auto &s : steps) planned += s.run_s;
      irrigation::RunRecord r{0, 0, d.program, 255, c.ts, planned, 0, 0.0f, irrigation::to_result(dec)};
      r.seq = this->runlog_.add(r);
      ESP_LOGI(TAG, "%s start %u skipped: %s", this->program_name_(d.program).c_str(), d.slot + 1,
               irrigation::result_name(r.result));
      this->emit_("skipped", irrigation::record_json(r));
      continue;
    }
    if (!this->enqueue_(d.program, pct, false, now_s))
      ESP_LOGW(TAG, "%s not queued (already queued/running, nothing to water, or queue full)",
               this->program_name_(d.program).c_str());
  }

  // 3. Runner and flow. A valve that never opened ends its step as STOPPED.
  if (this->driver_.take_start_failure()) {
    ESP_LOGW(TAG, "Valve did not open; step stopped");
    this->runner_.stop_zone_with(Result::STOPPED, now_s);
  }
  this->runner_.loop(now_s);

  const float flow = this->flow_lpm_();
  auto av = this->sprinkler_->active_valve();
  std::optional<uint8_t> az;
  if (av.has_value() && *av < (size_t) kZones) az = static_cast<uint8_t>(*av);
  if (az != this->trip_zone_) {  // new valve opening (or all closed): re-arm stop actions
    this->trip_zone_ = az;
    this->trip_latched_ = false;
  }

  if (this->has_flow_sensor_()) {
    if (this->flow_zone_open_ && (!az.has_value() || *az != this->flow_zone_)) {
      auto nb = this->flow_.zone_stopped(now_s);
      if (nb.has_value() && std::isfinite(*nb) && *nb > 0.0f) {
        ESP_LOGI(TAG, "%s flow baseline %.2f -> %.2f L/min", this->zone_name_(this->flow_zone_).c_str(),
                 this->zones_.z[this->flow_zone_].flow_baseline_lpm, *nb);
        this->zones_.z[this->flow_zone_].flow_baseline_lpm = *nb;
        this->save_zone(this->flow_zone_);
      }
      this->flow_zone_open_ = false;
      this->flow_alarm_active_ = false;
    }
    if (!this->flow_zone_open_ && az.has_value()) {
      const irrigation::Zone &z = this->zones_.z[*az];
      this->flow_.zone_started(*az, z.flow_baseline_lpm, z.flow_high_pct, z.flow_low_pct, now_s);
      this->flow_zone_open_ = true;
      this->flow_zone_ = *az;
      this->flow_alarm_active_ = false;
      this->leak_active_ = false;
    }
    const irrigation::FlowEvent fe = this->flow_.update(flow, now_s);
    if (fe == irrigation::FlowEvent::HIGH || fe == irrigation::FlowEvent::LOW) {
      const uint8_t zi = this->flow_zone_;
      const irrigation::Zone &z = this->zones_.z[zi];
      const bool high = fe == irrigation::FlowEvent::HIGH;
      const float limit = z.flow_baseline_lpm * (high ? z.flow_high_pct : z.flow_low_pct) / 100.0f;
      this->flow_alarm_active_ = true;
      ESP_LOGW(TAG, "%s flow %s: %.1f L/min (limit %.1f)", this->zone_name_(zi).c_str(), high ? "high" : "low",
               flow, limit);
      this->alert_(high ? "flow_high" : "flow_low", zi, flow, limit);
      if (z.flow_action == irrigation::FlowAction::ALERT_AND_STOP)
        this->trip_stop_(az, high ? Result::FLOW_HIGH : Result::FLOW_LOW, now_s);
    } else if (fe == irrigation::FlowEvent::LEAK) {
      this->leak_active_ = true;
      ESP_LOGW(TAG, "Leak: %.1f L/min with all valves closed", flow);
      this->alert_("leak", 255, flow, st.leak_lpm);
    }
    if (this->leak_active_ && flow <= st.leak_lpm) this->leak_active_ = false;
  }

  // Litres, attributed to the zone the sprinkler has open (manual toggles count too).
  if (flow > 0.0f) {
    const uint32_t dt = dt_ms > 5000 ? 5000 : dt_ms;
    const float l = flow * (dt / 1000.0f) / 60.0f;
    this->litres_.total += l;
    if (az.has_value()) {
      this->litres_.zone[*az] += l;
      if (this->step_active_ && *az == this->step_zone_ && this->runner_.active_zone() == az)
        this->step_litres_ += l;
    }
    this->litres_dirty_ = true;
    if (now_s - this->last_litres_save_s_ >= 300) this->save_litres_();
  }

  // 4. Max-runtime failsafe on whatever valve is open.
  if (this->safety_.exceeded(az, now_s, st.zone_hard_cap_s) && az.has_value() && !this->trip_latched_) {
    const uint8_t zi = *az;
    ESP_LOGW(TAG, "%s exceeded the %" PRIu32 " s runtime limit", this->zone_name_(zi).c_str(), st.zone_hard_cap_s);
    this->trip_stop_(az, Result::MAX_RUNTIME, now_s);
    this->alert_("max_runtime", zi, flow, static_cast<float>(st.zone_hard_cap_s));
  }

  // 5. Rain hold.
  this->runner_.set_hold(this->rain_hold() && st.rain_sensor_stops_running, now_s);

  // 6. Clock alert, once per invalid period, after a 120 s grace from boot.
  if (!c.valid) {
    if (!this->clock_alert_sent_ && now_s >= 120) {
      ESP_LOGW(TAG, "Clock not set: scheduled starts are suppressed");
      this->alert_("clock_invalid", 255, 0.0f, 0.0f);
      this->clock_alert_sent_ = true;
    }
  } else {
    this->clock_alert_sent_ = false;
  }
}

void IrrigationCore::trip_stop_(std::optional<uint8_t> az, Result r, uint32_t now_s) {
  if (this->trip_latched_ || !az.has_value()) return;
  this->trip_latched_ = true;  // once per valve opening
  const auto rz = this->runner_.active_zone();
  if (rz == az) {
    this->runner_.stop_zone_with(r, now_s);
  } else if (!rz.has_value()) {
    this->sprinkler_->shutdown(true);  // a valve the Runner does not own (web UI / HA)
  }
  // else: the Runner already started a different zone; the tripped valve is closing.
}

// ---------------------------------------------------------------- events

void IrrigationCore::emit_(const char *type, std::string json) {
  // Deferred to loop(): Runner callbacks must not re-enter the Runner through automations.
  this->pending_events_.emplace_back(type, std::move(json));
}

void IrrigationCore::flush_events_() {
  if (this->pending_events_.empty()) return;
  std::vector<std::pair<std::string, std::string>> evs;
  evs.swap(this->pending_events_);
  for (const auto &e : evs) {
    ESP_LOGD(TAG, "event %s %s", e.first.c_str(), e.second.c_str());
    for (auto *t : this->event_triggers_) t->trigger(e.first, e.second);
  }
}

void IrrigationCore::alert_(const char *kind, uint8_t zone, float flow, float limit) {
  std::string j = "{\"kind\":\"";
  j += kind;
  j += "\",\"z\":" + std::to_string(zone) + ",\"flow\":" + fmt_float(flow, 1) + ",\"limit\":" + fmt_float(limit, 1) +
       "}";
  this->emit_("alert", std::move(j));
}

void IrrigationCore::on_step_started_(const irrigation::StepEvent &ev) {
  const uint32_t ts = this->ts_of_uptime_(ev.start_s);
  if (ev.instance != this->inst_.id) {
    this->inst_ = InstanceAcc{};
    this->inst_.id = ev.instance;
    this->inst_.program = ev.program;
    this->inst_.first_ts = ts;
    const PendingRun &pr = this->pending_[ev.program < kPrograms ? ev.program : kPrograms];
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"inst\":%" PRIu32 ",\"p\":%u,\"zones\":%u,\"plan\":%" PRIu32 ",\"pct\":%u,\"ts\":%" PRIu32 "}",
             ev.instance, ev.program, pr.zones, pr.plan_s, pr.pct, ts);
    this->emit_("program_started", buf);
  }
  this->step_active_ = true;
  this->step_zone_ = ev.zone;
  this->step_litres_ = 0.0f;
  irrigation::RunRecord r{0, ev.instance, ev.program, ev.zone, ts, ev.planned_s, 0, 0.0f, Result::COMPLETED};
  ESP_LOGI(TAG, "%s: %s for %" PRIu32 " s", this->program_name_(ev.program).c_str(),
           this->zone_name_(ev.zone).c_str(), ev.planned_s);
  this->emit_("run_started", irrigation::record_json(r));
}

void IrrigationCore::on_step_finished_(const irrigation::StepEvent &ev) {
  irrigation::RunRecord r{0,           ev.instance,   ev.program,          ev.zone, this->ts_of_uptime_(ev.start_s),
                          ev.planned_s, ev.actual_s, this->step_litres_, ev.result};
  r.seq = this->runlog_.add(r);
  if (ev.instance == this->inst_.id) {
    this->inst_.act_s += ev.actual_s;
    this->inst_.litres += this->step_litres_;
    if (warn_result(ev.result)) this->inst_.warn = true;
  }
  this->step_active_ = false;
  ESP_LOGI(TAG, "%s: %s finished (%s, %" PRIu32 " s)", this->program_name_(ev.program).c_str(),
           this->zone_name_(ev.zone).c_str(), irrigation::result_name(ev.result), ev.actual_s);
  this->emit_("run_finished", irrigation::record_json(r));
  this->save_litres_();
}

void IrrigationCore::on_program_finished_(uint32_t instance, uint8_t program) {
  const PendingRun &pr = this->pending_[program < kPrograms ? program : kPrograms];
  const bool same = instance == this->inst_.id;
  std::string j = "{\"inst\":" + std::to_string(instance) + ",\"p\":" + std::to_string(program) +
                  ",\"zones\":" + std::to_string(pr.zones) + ",\"act\":" + std::to_string(same ? this->inst_.act_s : 0) +
                  ",\"l\":" + fmt_float(same ? this->inst_.litres : 0.0f, 1) +
                  ",\"ts\":" + std::to_string(same ? this->inst_.first_ts : 0) +
                  ",\"warn\":" + b(same && this->inst_.warn) + "}";
  this->emit_("program_finished", std::move(j));
}

// ---------------------------------------------------------------- persistence

irrigation::Program &IrrigationCore::program(uint8_t i) {
  if (i >= kPrograms) {
    ESP_LOGW(TAG, "program(%u) out of range", i);
    this->scratch_program_ = irrigation::default_program();
    return this->scratch_program_;  // never edits a real program
  }
  return this->programs_.p[i];
}

irrigation::Zone &IrrigationCore::zone(uint8_t i) {
  if (i >= kZones) {
    ESP_LOGW(TAG, "zone(%u) out of range", i);
    this->scratch_zone_ = irrigation::default_zone();
    return this->scratch_zone_;  // never edits a real zone
  }
  return this->zones_.z[i];
}

void IrrigationCore::save_program(uint8_t i) {
  if (i >= kPrograms) {
    ESP_LOGW(TAG, "save_program(%u) out of range", i);
    return;
  }
  // An edited start time may move later the same day: let it fire again.
  this->scheduler_.reset_program(i);
  this->pref_programs_.save(&this->programs_);
  global_preferences->sync();
  this->emit_("config_changed", "{}");
}

void IrrigationCore::set_program_interval_offset(uint8_t i, uint8_t days_until_first) {
  if (i >= kPrograms) return;
  Clock c = this->clock_now_();
  if (!c.valid) {
    ESP_LOGW(TAG, "Clock not set: interval anchor of %s unchanged", this->program_name_(i).c_str());
    return;
  }
  this->programs_.p[i].interval_anchor_day = c.day + days_until_first;
}

void IrrigationCore::save_zone(uint8_t i) {
  if (i >= kZones) {
    ESP_LOGW(TAG, "save_zone(%u) out of range", i);
    return;
  }
  // A changed output field goes through validation and re-drives the hardware
  // (assign_output_ saves the zone itself on success).
  const int16_t want = this->zones_.z[i].output;
  if (want != this->applied_[i]) {
    this->zones_.z[i].output = this->applied_[i];
    if (this->assign_output_(i, want) == PinError::OK) return;
  }
  this->pref_zones_.save(&this->zones_);
  global_preferences->sync();
  this->emit_("config_changed", "{}");
}

void IrrigationCore::save_settings() {
  auto &s = this->settings_;
  if (s.zone_count < 1) s.zone_count = 1;
  if (s.zone_count > kZones) s.zone_count = kZones;
  if (s.button_program >= kPrograms) s.button_program = 0;
  // Output-related fields are applied through their dedicated paths, which save again.
  if (s.master_output != this->applied_[kMasterSlot]) {
    const int16_t want = s.master_output;
    s.master_output = this->applied_[kMasterSlot];
    if (this->assign_output_(kMasterSlot, want) == PinError::OK) return;
  }
  if (s.relay_active_high != this->applied_active_high_) {
    const bool want = s.relay_active_high;
    s.relay_active_high = this->applied_active_high_;
    this->set_relay_active_high(want);
    return;
  }
  irrigation::FlowSettings fs;
  fs.settle_s = s.flow_settle_s;
  fs.leak_lpm = s.leak_lpm;
  this->flow_.configure(fs);
  this->pref_settings_.save(&this->settings_);
  global_preferences->sync();
  this->emit_("config_changed", "{}");
}

void IrrigationCore::save_litres_() {
  if (!this->litres_dirty_) return;
  this->litres_dirty_ = false;
  this->last_litres_save_s_ = this->uptime_s_();
  this->pref_litres_.save(&this->litres_);
  global_preferences->sync();
}

void IrrigationCore::save_weather_() {
  this->pref_weather_.save(&this->weather_);
  global_preferences->sync();
}

void IrrigationCore::save_rain_delay_() {
  this->pref_rain_delay_.save(&this->rain_delay_until_ts_);
  global_preferences->sync();
}

// ---------------------------------------------------------------- outputs

int16_t IrrigationCore::output_code(uint8_t slot) const { return slot <= kMasterSlot ? this->applied_[slot] : kNoOutput; }

std::string IrrigationCore::slot_name_(uint8_t slot) const {
  if (slot == kMasterSlot) return "Master";
  return "Zone " + std::to_string(slot + 1);
}

void IrrigationCore::notify_output_changed_(uint8_t slot) {
  const std::string label = irrigation::output_label(this->output_code(slot));
  for (auto &cb : this->output_changed_cbs_) cb(slot, label);
}

bool IrrigationCore::watering_() const {
  return this->runner_.busy() || this->sprinkler_->active_valve().has_value();
}

PinError IrrigationCore::assign_zone_output(uint8_t zone, int16_t code) {
  if (zone >= kZones) {
    this->last_error_ = "Zone " + std::to_string(zone + 1) + ": invalid zone";
    ESP_LOGW(TAG, "%s", this->last_error_.c_str());
    return PinError::INVALID;
  }
  return this->assign_output_(zone, code);
}

PinError IrrigationCore::assign_master_output(int16_t code) { return this->assign_output_(kMasterSlot, code); }

std::string IrrigationCore::output_error_text_(uint8_t slot, int16_t code, PinError e) const {
  std::string msg = this->slot_name_(slot) + ": ";
  msg += e == PinError::INVALID ? "code " + std::to_string(code) : irrigation::output_label(code);
  msg += " ";
  msg += irrigation::pin_error_text(e);
  if (e == PinError::DUPLICATE) {
    for (uint8_t i = 0; i <= kMasterSlot; i++)
      if (i != slot && this->applied_[i] == code) {
        msg += " " + this->slot_name_(i);
        break;
      }
  }
  return msg;
}

bool IrrigationCore::parse_output_(const std::string &label, int16_t &code) const {
  for (const auto &o : this->options_)
    if (o == label) {
      code = irrigation::parse_output_label(label);
      return true;
    }
  return false;
}

bool IrrigationCore::reject_(const std::string &msg) {
  this->last_error_ = msg;
  ESP_LOGW(TAG, "%s", msg.c_str());
  return false;
}

PinError IrrigationCore::assign_output_(uint8_t slot, int16_t code) {
  if (slot > kMasterSlot) return PinError::INVALID;
  const PinError e = irrigation::validate_assignment(slot, code, this->applied_, this->board_);
  if (e != PinError::OK) {
    const std::string msg = this->output_error_text_(slot, code, e);
    this->last_error_ = msg;
    ESP_LOGW(TAG, "%s", msg.c_str());
    this->notify_output_changed_(slot);  // selects snap back to the current label
    return e;
  }
  const int16_t old = this->applied_[slot];
  if (old == code) return PinError::OK;
  if (this->watering_()) {
    ESP_LOGW(TAG, "Output change on %s stops watering", this->slot_name_(slot).c_str());
    this->stop_all();
  }
  this->hw_park_(old);
  this->applied_[slot] = code;
  this->hw_configure_off_(code);
  ESP_LOGI(TAG, "%s: %s -> %s", this->slot_name_(slot).c_str(), irrigation::output_label(old).c_str(),
           irrigation::output_label(code).c_str());
  this->last_error_.clear();
  if (slot < kZones) {
    this->zones_.z[slot].output = code;
    this->save_zone(slot);  // emits config_changed
  } else {
    this->settings_.master_output = code;
    this->save_settings();  // emits config_changed
  }
  this->notify_output_changed_(slot);
  return PinError::OK;
}

PinError IrrigationCore::set_relay_active_high(bool v) {
  this->stop_all();
  this->applied_active_high_ = v;
  this->settings_.relay_active_high = v;
  for (uint8_t slot = 0; slot <= kMasterSlot; slot++) this->hw_configure_off_(this->applied_[slot]);
  // Released outputs were parked at the old off level, which is now the ON level.
  this->repark_released_();
  ESP_LOGI(TAG, "Relays now active %s", v ? "high" : "low");
  this->save_settings();
  return PinError::OK;
}

void IrrigationCore::write_output(uint8_t slot, bool on) {
  if (!this->ready_) return;
  if (slot > kMasterSlot) {
    ESP_LOGW(TAG, "write_output: invalid slot %u", slot);
    return;
  }
  const int16_t code = this->applied_[slot];
  if (code == kNoOutput) {
    // Off on an unassigned slot is the normal boot/stop path; only warn when asked to switch on.
    if (on) ESP_LOGW(TAG, "%s has no output assigned", this->slot_name_(slot).c_str());
    return;
  }
  this->hw_write_(code, on == this->applied_active_high_);
}

void IrrigationCore::hw_configure_off_(int16_t code) {
  const bool off = !this->applied_active_high_;
  const irrigation::OutputRef r = irrigation::decode_output(code);
  if (r.kind == irrigation::OutputKind::GPIO) {
    const auto pin = static_cast<gpio_num_t>(r.index);
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
      ESP_LOGW(TAG, "GPIO %u cannot be an output", r.index);
      return;
    }
    gpio_set_level(pin, off);  // latch the off level before enabling the driver
    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << r.index;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    gpio_set_level(pin, off);
  } else if (r.kind == irrigation::OutputKind::EXPANDER) {
#ifdef USE_IRRIGATION_EXPANDER
    if (this->expander_ != nullptr) {
      this->expander_->digital_write(r.index, off);  // OLAT first, then direction
      this->expander_->pin_mode(r.index, gpio::FLAG_OUTPUT);
    }
#endif
  }
}

void IrrigationCore::hw_write_(int16_t code, bool level) {
  const irrigation::OutputRef r = irrigation::decode_output(code);
  if (r.kind == irrigation::OutputKind::GPIO) {
    gpio_set_level(static_cast<gpio_num_t>(r.index), level);
  } else if (r.kind == irrigation::OutputKind::EXPANDER) {
#ifdef USE_IRRIGATION_EXPANDER
    if (this->expander_ != nullptr) this->expander_->digital_write(r.index, level);
#endif
  }
}

void IrrigationCore::hw_park_(int16_t code) {
  const irrigation::OutputRef r = irrigation::decode_output(code);
  if (r.kind == irrigation::OutputKind::GPIO) {
    // Input pulled to the relay's off level (gpio_reset_pin would pull up and
    // switch an active-high KS0011 on).
    const auto pin = static_cast<gpio_num_t>(r.index);
    const bool up = irrigation::park_pull(this->applied_active_high_) == irrigation::Pull::UP;
    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << r.index;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
  } else if (r.kind == irrigation::OutputKind::EXPANDER) {
#ifdef USE_IRRIGATION_EXPANDER
    // An MCP channel is only ever used for a relay: leave it driven at the off level.
    if (this->expander_ != nullptr) this->expander_->digital_write(r.index, !this->applied_active_high_);
#endif
  }
}

void IrrigationCore::repark_released_() {
  auto in_use = [this](int16_t code) {
    for (uint8_t s = 0; s <= kMasterSlot; s++)
      if (this->applied_[s] == code) return true;
    return false;
  };
  for (uint8_t pin : this->board_.candidates)
    if (!in_use(pin)) this->hw_park_(pin);
#ifdef USE_IRRIGATION_EXPANDER
  if (this->expander_ != nullptr) {
    for (int16_t code = 100; code < 116; code++) {
      if (in_use(code)) continue;
      this->hw_park_(code);
      // A never-used MCP channel is still an input after reset: make it an off output.
      this->expander_->pin_mode(static_cast<uint8_t>(code - 100), gpio::FLAG_OUTPUT);
    }
  }
#endif
}

// ---------------------------------------------------------------- commands

bool IrrigationCore::enqueue_(uint8_t p, uint16_t pct, bool manual, uint32_t now_s) {
  auto steps = irrigation::expand_program(this->programs_.p[p], this->zones_.z, this->settings_.zone_count, pct,
                                          this->settings_.zone_hard_cap_s);
  if (steps.empty()) return false;
  PendingRun pr;
  pr.pct = pct;
  bool seen[kZones] = {};
  for (const auto &s : steps) {
    pr.plan_s += s.run_s;
    if (s.zone < kZones && !seen[s.zone]) {
      seen[s.zone] = true;
      pr.zones++;
    }
  }
  if (!this->runner_.enqueue_program(p, std::move(steps), now_s, manual)) return false;
  this->pending_[p] = pr;
  return true;
}

void IrrigationCore::run_zone(uint8_t zone, uint32_t seconds) {
  if (zone >= kZones || zone >= this->sprinkler_->number_of_valves()) {
    ESP_LOGW(TAG, "run_zone: zone %u out of range", zone + 1);
    return;
  }
  if (this->applied_[zone] == kNoOutput) {
    ESP_LOGW(TAG, "run_zone: %s has no output assigned", this->zone_name_(zone).c_str());
    return;
  }
  if (seconds == 0) {
    ESP_LOGW(TAG, "run_zone: zero duration");
    return;
  }
  if (seconds > this->settings_.zone_hard_cap_s) seconds = this->settings_.zone_hard_cap_s;
  PendingRun pr;
  pr.pct = 100;
  pr.plan_s = seconds;
  pr.zones = 1;
  this->pending_[kPrograms] = pr;
  this->runner_.run_manual_zone(zone, seconds, this->uptime_s_());
}

void IrrigationCore::run_program(uint8_t p) {
  if (p >= kPrograms) {
    ESP_LOGW(TAG, "run_program: program %u out of range", p);
    return;
  }
  if (!this->settings_.controller_enabled) {
    ESP_LOGW(TAG, "run_program: controller disabled");
    return;
  }
  const Clock c = this->clock_now_();
  const uint8_t month_pct = c.valid ? this->settings_.monthly_pct[(c.month + 11) % 12] : 100;
  // Manual program run: program % x monthly %, weather 100, no rain/weather skips.
  const uint16_t pct = irrigation::effective_pct(this->programs_.p[p].seasonal_pct, month_pct, 100);
  if (!this->enqueue_(p, pct, true, this->uptime_s_()))
    ESP_LOGW(TAG, "run_program: %s not queued (already queued/running, nothing to water, or queue full)",
             this->program_name_(p).c_str());
}

void IrrigationCore::stop_all() {
  this->runner_.stop_all(this->uptime_s_());
  // Also ends valves the Runner did not start (web UI / HA valve switch).
  this->sprinkler_->shutdown(true);
}

void IrrigationCore::next_zone() { this->runner_.stop_zone_with(Result::STOPPED, this->uptime_s_()); }

void IrrigationCore::set_rain_delay_hours(uint16_t h) {
  if (h == 0) {
    this->clear_rain_delay();
    return;
  }
  const Clock c = this->clock_now_();
  if (!c.valid) {
    ESP_LOGW(TAG, "Clock not set: rain delay not set");
    return;
  }
  this->rain_delay_until_ts_ = c.ts + static_cast<uint32_t>(h) * 3600u;
  ESP_LOGI(TAG, "Rain delay %u h", h);
  this->save_rain_delay_();
}

void IrrigationCore::clear_rain_delay() {
  this->rain_delay_until_ts_ = 0;
  this->save_rain_delay_();
}

bool IrrigationCore::rain_delay_active() const {
  if (this->rain_delay_until_ts_ == 0) return false;
  const Clock c = this->clock_now_();
  return c.valid && c.ts < this->rain_delay_until_ts_;
}

void IrrigationCore::set_weather_adjust(uint16_t pct, bool skip, const std::string &reason, uint16_t valid_hours) {
  const Clock c = this->clock_now_();
  if (!c.valid) {
    ESP_LOGW(TAG, "Clock not set: weather adjust ignored");
    return;
  }
  if (pct > 250) pct = 250;
  this->weather_.pct = pct;
  this->weather_.skip = skip;
  strncpy(this->weather_.reason, reason.c_str(), sizeof(this->weather_.reason) - 1);
  this->weather_.reason[sizeof(this->weather_.reason) - 1] = '\0';
  this->weather_.valid_until_ts = c.ts + static_cast<uint32_t>(valid_hours) * 3600u;
  ESP_LOGI(TAG, "Weather adjust %u%%%s for %u h (%s)", pct, skip ? " skip" : "", valid_hours,
           this->weather_.reason);
  this->save_weather_();
}

void IrrigationCore::learn_flow(uint8_t zone) {
  if (zone >= kZones) {
    ESP_LOGW(TAG, "learn_flow: zone %u out of range", zone + 1);
    return;
  }
  this->zones_.z[zone].flow_baseline_lpm = 0.0f;
  this->save_zone(zone);
  // Learning needs at least 120 s of post-settle flow.
  this->run_zone(zone, static_cast<uint32_t>(this->settings_.flow_settle_s) + 150u);
}

void IrrigationCore::rain_tip() {
  if (this->settings_.rain_type != irrigation::RainType::TIPPING) return;
  const Clock c = this->clock_now_();
  if (!c.valid) return;  // the rain window keys on wall-clock time
  this->rain_window_.add_tip(c.ts);
}

void IrrigationCore::set_rain_switch(bool raw_state) { this->rain_switch_raw_ = raw_state; }

bool IrrigationCore::rain_hold() const {
  switch (this->settings_.rain_type) {
    case irrigation::RainType::SWITCH_NC:
      return !this->rain_switch_raw_;  // NC contact opens when wet
    case irrigation::RainType::SWITCH_NO:
      return this->rain_switch_raw_;
    case irrigation::RainType::TIPPING:
      return this->settings_.rain_skip_mm > 0.0f && this->rain_mm_24h() >= this->settings_.rain_skip_mm;
    case irrigation::RainType::NONE:
    default:
      return false;
  }
}

float IrrigationCore::rain_mm_24h() const {
  if (this->settings_.rain_type != irrigation::RainType::TIPPING) return 0.0f;
  const Clock c = this->clock_now_();
  if (!c.valid) return 0.0f;
  return this->rain_window_.mm_last_24h(c.ts, this->settings_.mm_per_tip);
}

void IrrigationCore::button_press(bool is_start, bool is_long) {
  const auto &s = this->settings_;
  const irrigation::ButtonFn fn =
      is_start ? (is_long ? s.start_long_fn : s.start_short_fn) : (is_long ? s.stop_long_fn : s.stop_short_fn);
  const irrigation::ButtonState bs{s.controller_enabled, this->watering_(), this->rain_delay_active()};
  const irrigation::ButtonCmd cmd = irrigation::resolve_button(fn, bs);
  switch (cmd) {
    case irrigation::ButtonCmd::RUN_PROGRAM:
      this->run_program(s.button_program);
      break;
    case irrigation::ButtonCmd::STOP_ALL:
      this->stop_all();
      break;
    case irrigation::ButtonCmd::NEXT_ZONE:
      this->next_zone();
      break;
    case irrigation::ButtonCmd::SET_RAIN_DELAY_24H:
      this->set_rain_delay_hours(24);
      break;
    case irrigation::ButtonCmd::CLEAR_RAIN_DELAY:
      this->clear_rain_delay();
      break;
    case irrigation::ButtonCmd::NONE:
    default:
      break;
  }
  if (cmd != irrigation::ButtonCmd::NONE) this->led_ack_until_ms_ = millis() + irrigation::kLedAckMs;
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"btn\":\"%s\",\"press\":\"%s\",\"fn\":%u,\"cmd\":%u}", is_start ? "start" : "stop",
           is_long ? "long" : "short", static_cast<unsigned>(fn), static_cast<unsigned>(cmd));
  this->emit_("button", buf);
}

bool IrrigationCore::status_led_level() const {
  const irrigation::LedState ls{this->watering_(), this->rain_delay_active() || this->rain_hold(),
                                this->clock_valid()};
  return irrigation::led_level(irrigation::led_mode(ls), millis(), this->led_ack_until_ms_);
}

// ---------------------------------------------------------------- names / status

std::string IrrigationCore::program_name_(uint8_t p) const {
  if (p == kManual) return "Manual";
  if (p >= kPrograms) return "Program ?";
#ifdef USE_IRRIGATION_NAME_TEXTS
  if (this->program_name_texts_[p] != nullptr && !this->program_name_texts_[p]->state.empty())
    return this->program_name_texts_[p]->state;
#endif
  return std::string("Program ") + static_cast<char>('A' + p);
}

std::string IrrigationCore::zone_name_(uint8_t z) const {
  if (z >= kZones) return "Zone ?";
#ifdef USE_IRRIGATION_NAME_TEXTS
  if (this->zone_name_texts_[z] != nullptr && !this->zone_name_texts_[z]->state.empty())
    return this->zone_name_texts_[z]->state;
#endif
  return "Zone " + std::to_string(z + 1);
}

bool IrrigationCore::clock_valid() const { return this->clock_now_().valid; }

std::string IrrigationCore::active_zone_name() const {
  auto av = this->sprinkler_->active_valve();
  if (!av.has_value()) return "None";
  return this->zone_name_(static_cast<uint8_t>(*av));
}

std::string IrrigationCore::active_program_name() const {
  auto ap = this->runner_.active_program();
  if (ap.has_value()) return this->program_name_(*ap);
  if (this->sprinkler_->active_valve().has_value()) return "Manual";
  return "None";
}

std::string IrrigationCore::status_text() const {
  if (this->sprinkler_->active_valve().has_value())
    return "Watering " + this->active_zone_name() + " (" + this->active_program_name() + ")";
  if (this->runner_.busy()) return "Soaking";
  if (!this->settings_.controller_enabled) return "Disabled";
  if (!this->clock_valid()) return "Clock not set";
  if (this->rain_delay_active()) return "Rain delay";
  if (this->rain_hold()) return "Rain hold";
  return "Idle";
}

uint16_t IrrigationCore::current_weather_pct() const {
  const Clock c = this->clock_now_();
  return (c.valid && c.ts < this->weather_.valid_until_ts) ? this->weather_.pct : 100;
}

float IrrigationCore::zone_litres_total(uint8_t z) const { return z < kZones ? this->litres_.zone[z] : 0.0f; }

uint32_t IrrigationCore::remaining_s() const {
  auto r = this->sprinkler_->time_remaining_active_valve();
  return r.has_value() ? *r : 0;
}

int32_t IrrigationCore::next_run_ts() const {
  const Clock c = this->clock_now_();
  if (!c.valid) return 0;
  const uint32_t ts = c.ts;
  auto runs = irrigation::project(
      this->programs_.p, this->zones_.z, this->settings_.zone_count, this->settings_.monthly_pct,
      this->settings_.zone_hard_cap_s, irrigation::LocalDateTime{c.day, c.minute}, 8,
      [this, ts](const irrigation::Program &p, int32_t) { return irrigation::active_weather_pct(this->weather_, p, ts); });
  const int64_t now_idx = static_cast<int64_t>(c.day) * 1440 + c.minute;
  int64_t best = -1;
  for (const auto &r : runs) {
    const int64_t idx = static_cast<int64_t>(r.day) * 1440 + r.minute;
    if (idx <= now_idx) continue;
    if (best < 0 || idx < best) best = idx;
  }
  if (best < 0) return 0;
  // Local minutes -> epoch, assuming no DST change before the run (good enough for display).
  return static_cast<int32_t>(static_cast<int64_t>(ts) - c.second + (best - now_idx) * 60);
}

// ---------------------------------------------------------------- API actions

namespace {

int clamp_int(const char *what, int v, int lo, int hi) {
  const int r = v < lo ? lo : (v > hi ? hi : v);
  if (r != v) ESP_LOGW(TAG, "%s %d out of range %d..%d, using %d", what, v, lo, hi, r);
  return r;
}

float clamp_float(const char *what, float v, float lo, float hi) {
  float r = v;
  if (!std::isfinite(r) || r < lo) r = lo;
  if (r > hi) r = hi;
  if (!(r == v)) ESP_LOGW(TAG, "%s %.2f out of range %.2f..%.2f, using %.2f", what, v, lo, hi, r);
  return r;
}

}  // namespace

void IrrigationCore::set_program_name(uint8_t p, const std::string &name) {
#ifdef USE_IRRIGATION_NAME_TEXTS
  if (p >= kPrograms || this->program_name_texts_[p] == nullptr) return;
  this->program_name_texts_[p]->make_call().set_value(name.substr(0, 24)).perform();
#else
  (void) p;
  (void) name;
#endif
}

void IrrigationCore::set_zone_name(uint8_t z, const std::string &name) {
#ifdef USE_IRRIGATION_NAME_TEXTS
  if (z >= kZones || this->zone_name_texts_[z] == nullptr) return;
  this->zone_name_texts_[z]->make_call().set_value(name.substr(0, 24)).perform();
#else
  (void) z;
  (void) name;
#endif
}

void IrrigationCore::api_run_zone(int zone, int minutes) {
  if (zone < 1 || zone > kZones) {
    this->reject_("run_zone: zone " + std::to_string(zone) + " out of range 1..16");
    return;
  }
  minutes = clamp_int("run_zone minutes", minutes, 0, 240);
  this->run_zone(static_cast<uint8_t>(zone - 1), static_cast<uint32_t>(minutes) * 60u);
}

void IrrigationCore::api_run_program(int program) {
  if (program < 0 || program >= kPrograms) {
    this->reject_("run_program: program " + std::to_string(program) + " out of range 0..7");
    return;
  }
  this->run_program(static_cast<uint8_t>(program));
}

void IrrigationCore::api_rain_delay(int hours) {
  hours = clamp_int("rain_delay hours", hours, 0, 720);
  this->set_rain_delay_hours(static_cast<uint16_t>(hours));
}

void IrrigationCore::api_set_weather_adjust(int pct, bool skip, const std::string &reason, int valid_hours) {
  pct = clamp_int("weather pct", pct, 0, 250);
  valid_hours = clamp_int("weather valid_hours", valid_hours, 0, 720);
  this->set_weather_adjust(static_cast<uint16_t>(pct), skip, reason, static_cast<uint16_t>(valid_hours));
}

bool IrrigationCore::api_set_program(int program, const std::string &name, bool enabled, int mode, int weekday_mask,
                                     int interval_days, int days_until_first, const std::vector<int32_t> &starts,
                                     const std::vector<int32_t> &zone_minutes, int seasonal_pct, bool cycle_soak,
                                     bool weather_adjust) {
  if (program < 0 || program >= kPrograms)
    return this->reject_("set_program: program " + std::to_string(program) + " out of range 0..7");
  if (starts.size() > static_cast<size_t>(kStarts))
    ESP_LOGW(TAG, "set_program: %u start times given, using the first %d", (unsigned) starts.size(), kStarts);
  if (zone_minutes.size() > static_cast<size_t>(kZones))
    ESP_LOGW(TAG, "set_program: %u zone times given, using the first %d", (unsigned) zone_minutes.size(), kZones);
  const uint8_t pi = static_cast<uint8_t>(program);
  irrigation::Program &p = this->program(pi);
  p.enabled = enabled;
  p.mode = static_cast<irrigation::DayMode>(clamp_int("program mode", mode, 0, 3));
  p.weekday_mask = static_cast<uint8_t>(clamp_int("weekday_mask", weekday_mask, 0, 127));
  p.interval_days = static_cast<uint8_t>(clamp_int("interval_days", interval_days, 1, 30));
  for (int s = 0; s < kStarts; s++) {
    int v = s < (int) starts.size() ? starts[s] : -1;
    if (v < 0)
      v = -1;
    else
      v = clamp_int("start minute", v, 0, 1439);
    p.start_minutes[s] = static_cast<int16_t>(v);
  }
  for (int z = 0; z < kZones; z++) {
    const int m = z < (int) zone_minutes.size() ? clamp_int("zone minutes", zone_minutes[z], 0, 240) : 0;
    p.zone_seconds[z] = static_cast<uint16_t>(m * 60);
  }
  p.seasonal_pct = static_cast<uint8_t>(clamp_int("seasonal_pct", seasonal_pct, 0, 200));
  p.cycle_soak = cycle_soak;
  p.weather_adjust = weather_adjust;
  days_until_first = clamp_int("days_until_first", days_until_first, 0, 29);
  if (p.mode == irrigation::DayMode::INTERVAL)
    this->set_program_interval_offset(pi, static_cast<uint8_t>(days_until_first));
  this->set_program_name(pi, name);
  this->save_program(pi);
  return true;
}

bool IrrigationCore::api_set_zone(int zone, const std::string &name, bool enabled, int head, float precip, int soil,
                                  int slope, int sun, int plant, float area, int max_cycle_min, int soak_min,
                                  float baseline, int high_pct, int low_pct, int action, const std::string &output) {
  if (zone < 1 || zone > kZones)
    return this->reject_("set_zone: zone " + std::to_string(zone) + " out of range 1..16");
  const uint8_t zi = static_cast<uint8_t>(zone - 1);
  // Validate the output first so a bad label changes nothing.
  int16_t code = this->applied_[zi];
  if (!output.empty()) {
    if (!this->parse_output_(output, code))
      return this->reject_(this->slot_name_(zi) + ": unknown output '" + output + "'");
    if (code != this->applied_[zi]) {
      const PinError e = irrigation::validate_assignment(zi, code, this->applied_, this->board_);
      if (e != PinError::OK) return this->reject_(this->output_error_text_(zi, code, e));
    }
  }
  irrigation::Zone &z = this->zone(zi);
  z.enabled = enabled;
  z.head = static_cast<irrigation::HeadType>(clamp_int("head", head, 0, 4));
  z.precip_mm_h = clamp_float("precip", precip, 1.0f, 100.0f);
  z.soil = static_cast<irrigation::Soil>(clamp_int("soil", soil, 0, 5));
  z.slope_pct = static_cast<uint8_t>(clamp_int("slope", slope, 0, 50));
  z.sun = static_cast<irrigation::Sun>(clamp_int("sun", sun, 0, 2));
  z.plant = static_cast<irrigation::Plant>(clamp_int("plant", plant, 0, 5));
  z.area_m2 = clamp_float("area", area, 0.0f, 5000.0f);
  z.max_cycle_s = static_cast<uint16_t>(clamp_int("max_cycle_min", max_cycle_min, 0, 60) * 60);
  z.soak_s = static_cast<uint16_t>(clamp_int("soak_min", soak_min, 0, 120) * 60);
  z.flow_baseline_lpm = clamp_float("baseline", baseline, 0.0f, 200.0f);
  z.flow_high_pct = static_cast<uint16_t>(clamp_int("high_pct", high_pct, 100, 300));
  z.flow_low_pct = static_cast<uint8_t>(clamp_int("low_pct", low_pct, 0, 100));
  z.flow_action = static_cast<irrigation::FlowAction>(clamp_int("action", action, 0, 1));
  z.output = code;  // save_zone routes a changed output through validation + hardware
  this->set_zone_name(zi, name);
  this->save_zone(zi);
  return true;
}

bool IrrigationCore::api_set_settings(int zone_count, bool controller_enabled, const std::vector<int32_t> &monthly_pct,
                                      float pulses_per_litre, float leak_lpm, int settle_s, int rain_type,
                                      float mm_per_tip, float rain_skip_mm, bool rain_stops, int hard_cap_min,
                                      int start_short_fn, int start_long_fn, int stop_short_fn, int stop_long_fn,
                                      int button_program, const std::string &master_output,
                                      bool relay_active_high) {
  if (monthly_pct.size() != 12)
    return this->reject_("set_settings: monthly_pct needs 12 values, got " + std::to_string(monthly_pct.size()));
  int16_t mcode = this->applied_[kMasterSlot];
  if (!master_output.empty()) {
    if (!this->parse_output_(master_output, mcode))
      return this->reject_("Master: unknown output '" + master_output + "'");
    if (mcode != this->applied_[kMasterSlot]) {
      const PinError e = irrigation::validate_assignment(kMasterSlot, mcode, this->applied_, this->board_);
      if (e != PinError::OK) return this->reject_(this->output_error_text_(kMasterSlot, mcode, e));
    }
  }
  auto &s = this->settings_;
  s.zone_count = static_cast<uint8_t>(clamp_int("zone_count", zone_count, 1, kZones));
  s.controller_enabled = controller_enabled;
  for (int m = 0; m < 12; m++)
    s.monthly_pct[m] = static_cast<uint8_t>(clamp_int("monthly_pct", monthly_pct[m], 0, 200));
  s.pulses_per_litre = clamp_float("pulses_per_litre", pulses_per_litre, 1.0f, 5000.0f);
  s.leak_lpm = clamp_float("leak_lpm", leak_lpm, 0.1f, 20.0f);
  s.flow_settle_s = static_cast<uint16_t>(clamp_int("settle_s", settle_s, 10, 300));
  s.rain_type = static_cast<irrigation::RainType>(clamp_int("rain_type", rain_type, 0, 3));
  s.mm_per_tip = clamp_float("mm_per_tip", mm_per_tip, 0.1f, 1.0f);
  s.rain_skip_mm = clamp_float("rain_skip_mm", rain_skip_mm, 0.0f, 50.0f);
  s.rain_sensor_stops_running = rain_stops;
  s.zone_hard_cap_s = static_cast<uint32_t>(clamp_int("hard_cap_min", hard_cap_min, 5, 240)) * 60u;
  s.start_short_fn = static_cast<irrigation::ButtonFn>(clamp_int("start_short_fn", start_short_fn, 0, 4));
  s.start_long_fn = static_cast<irrigation::ButtonFn>(clamp_int("start_long_fn", start_long_fn, 0, 4));
  s.stop_short_fn = static_cast<irrigation::ButtonFn>(clamp_int("stop_short_fn", stop_short_fn, 0, 4));
  s.stop_long_fn = static_cast<irrigation::ButtonFn>(clamp_int("stop_long_fn", stop_long_fn, 0, 4));
  s.button_program = static_cast<uint8_t>(clamp_int("button_program", button_program, 0, kPrograms - 1));
  s.master_output = mcode;  // save_settings applies output / relay level changes
  s.relay_active_high = relay_active_high;
  this->save_settings();
  return true;
}

int IrrigationCore::program_days_until_first(uint8_t i) const {
  if (i >= kPrograms) return 0;
  const Clock c = this->clock_now_();
  if (!c.valid) return 0;
  const irrigation::Program &p = this->programs_.p[i];
  const int32_t n = p.interval_days == 0 ? 1 : p.interval_days;
  return static_cast<int>(((p.interval_anchor_day - c.day) % n + n) % n);
}

// ---------------------------------------------------------------- JSON

std::string IrrigationCore::plan_json(int days) {
  const Clock c = this->clock_now_();
  if (!c.valid || days <= 0) return "[]";
  if (days > 62) days = 62;
  const uint32_t ts = c.ts;
  auto runs = irrigation::project(
      this->programs_.p, this->zones_.z, this->settings_.zone_count, this->settings_.monthly_pct,
      this->settings_.zone_hard_cap_s, irrigation::LocalDateTime{c.day, c.minute}, days,
      [this, ts](const irrigation::Program &p, int32_t) { return irrigation::active_weather_pct(this->weather_, p, ts); });
  return irrigation::plan_json(runs);
}

std::string IrrigationCore::runlog_json(uint32_t since_seq) {
  return irrigation::records_json(this->runlog_.since(since_seq));
}

std::string IrrigationCore::config_json() {
  std::string j;
  j.reserve(6144);
  j += "{\"programs\":[";
  for (int i = 0; i < kPrograms; i++) {
    const irrigation::Program &p = this->programs_.p[i];
    if (i) j += ',';
    const int dtf = this->program_days_until_first(static_cast<uint8_t>(i));
    j += "{\"program\":" + std::to_string(i) + ",\"name\":";
    json_escape_append(j, this->program_name_(i));
    j += ",\"enabled\":";
    j += b(p.enabled);
    j += ",\"mode\":" + std::to_string(static_cast<int>(p.mode));
    j += ",\"weekday_mask\":" + std::to_string(p.weekday_mask);
    j += ",\"interval_days\":" + std::to_string(p.interval_days);
    j += ",\"days_until_first\":" + std::to_string(dtf);
    j += ",\"starts\":[";
    for (int s = 0; s < kStarts; s++) {
      if (s) j += ',';
      j += std::to_string(p.start_minutes[s]);
    }
    j += "],\"zone_minutes\":[";
    for (int z = 0; z < kZones; z++) {
      if (z) j += ',';
      j += std::to_string((p.zone_seconds[z] + 30) / 60);
    }
    j += "],\"seasonal_pct\":" + std::to_string(p.seasonal_pct);
    j += ",\"cycle_soak\":";
    j += b(p.cycle_soak);
    j += ",\"weather_adjust\":";
    j += b(p.weather_adjust);
    j += '}';
  }
  j += "],\"zones\":[";
  for (int i = 0; i < kZones; i++) {
    const irrigation::Zone &z = this->zones_.z[i];
    if (i) j += ',';
    j += "{\"zone\":" + std::to_string(i + 1) + ",\"name\":";
    json_escape_append(j, this->zone_name_(i));
    j += ",\"enabled\":";
    j += b(z.enabled);
    j += ",\"head\":" + std::to_string(static_cast<int>(z.head));
    j += ",\"precip\":" + fmt_float(z.precip_mm_h, 2);
    j += ",\"soil\":" + std::to_string(static_cast<int>(z.soil));
    j += ",\"slope\":" + std::to_string(z.slope_pct);
    j += ",\"sun\":" + std::to_string(static_cast<int>(z.sun));
    j += ",\"plant\":" + std::to_string(static_cast<int>(z.plant));
    j += ",\"area\":" + fmt_float(z.area_m2, 2);
    j += ",\"max_cycle_min\":" + std::to_string((z.max_cycle_s + 30) / 60);
    j += ",\"soak_min\":" + std::to_string((z.soak_s + 30) / 60);
    j += ",\"baseline\":" + fmt_float(z.flow_baseline_lpm, 2);
    j += ",\"high_pct\":" + std::to_string(z.flow_high_pct);
    j += ",\"low_pct\":" + std::to_string(z.flow_low_pct);
    j += ",\"action\":" + std::to_string(static_cast<int>(z.flow_action));
    j += ",\"output\":";
    json_escape_append(j, irrigation::output_label(this->applied_[i]));
    j += '}';
  }
  const auto &s = this->settings_;
  j += "],\"settings\":{\"zone_count\":" + std::to_string(s.zone_count);
  j += ",\"controller_enabled\":";
  j += b(s.controller_enabled);
  j += ",\"monthly_pct\":[";
  for (int m = 0; m < 12; m++) {
    if (m) j += ',';
    j += std::to_string(s.monthly_pct[m]);
  }
  j += "],\"pulses_per_litre\":" + fmt_float(s.pulses_per_litre, 2);
  j += ",\"leak_lpm\":" + fmt_float(s.leak_lpm, 2);
  j += ",\"settle_s\":" + std::to_string(s.flow_settle_s);
  j += ",\"rain_type\":" + std::to_string(static_cast<int>(s.rain_type));
  j += ",\"mm_per_tip\":" + fmt_float(s.mm_per_tip, 2);
  j += ",\"rain_skip_mm\":" + fmt_float(s.rain_skip_mm, 2);
  j += ",\"rain_stops\":";
  j += b(s.rain_sensor_stops_running);
  j += ",\"hard_cap_min\":" + std::to_string((s.zone_hard_cap_s + 30) / 60);
  j += ",\"start_short_fn\":" + std::to_string(static_cast<int>(s.start_short_fn));
  j += ",\"start_long_fn\":" + std::to_string(static_cast<int>(s.start_long_fn));
  j += ",\"stop_short_fn\":" + std::to_string(static_cast<int>(s.stop_short_fn));
  j += ",\"stop_long_fn\":" + std::to_string(static_cast<int>(s.stop_long_fn));
  j += ",\"button_program\":" + std::to_string(s.button_program);
  j += ",\"master_output\":";
  json_escape_append(j, irrigation::output_label(this->applied_[kMasterSlot]));
  j += ",\"relay_active_high\":";
  j += b(this->applied_active_high_);
  j += "}}";
  return j;
}

}  // namespace esphome::irrigation_core
