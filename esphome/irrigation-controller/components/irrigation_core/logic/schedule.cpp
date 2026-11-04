#include "schedule.h"

#include <climits>

#include "civil.h"

namespace irrigation {

bool is_due_on(const Program& p, int32_t day) {
  switch (p.mode) {
    case DayMode::WEEKDAYS:
      return (p.weekday_mask >> weekday_mon0(day)) & 1;
    case DayMode::ODD:
    case DayMode::EVEN: {
      int y;
      unsigned m, d;
      civil_from_days(day, y, m, d);
      if (p.mode == DayMode::ODD) return (d % 2 == 1) && d != 31;
      return d % 2 == 0;
    }
    case DayMode::INTERVAL: {
      if (day < p.interval_anchor_day) return false;
      const int32_t n = p.interval_days == 0 ? 1 : p.interval_days;
      return (day - p.interval_anchor_day) % n == 0;
    }
  }
  return false;
}

uint16_t effective_pct(uint8_t program_pct, uint8_t month_pct, uint16_t weather_pct) {
  const uint64_t prod = static_cast<uint64_t>(program_pct) * month_pct * weather_pct;
  const uint64_t v = (prod + 5000) / 10000;
  return static_cast<uint16_t>(v > 250 ? 250 : v);
}

StartDecision decide_start(const Program& p, const StartContext& c, uint16_t pct) {
  if (!c.controller_enabled) return StartDecision::SKIP_CONTROLLER_OFF;
  if (c.rain_delay_active) return StartDecision::SKIP_RAIN_DELAY;
  if (c.rain_hold) return StartDecision::SKIP_RAIN_SENSOR;
  if (c.weather_skip_valid && p.weather_adjust) return StartDecision::SKIP_WEATHER;
  if (pct == 0) return StartDecision::SKIP_WEATHER;
  return StartDecision::RUN;
}

Result to_result(StartDecision d) {
  switch (d) {
    case StartDecision::SKIP_CONTROLLER_OFF: return Result::SKIPPED_CONTROLLER_OFF;
    case StartDecision::SKIP_RAIN_DELAY: return Result::SKIPPED_RAIN_DELAY;
    case StartDecision::SKIP_RAIN_SENSOR: return Result::SKIPPED_RAIN_SENSOR;
    case StartDecision::SKIP_WEATHER: return Result::SKIPPED_WEATHER;
    case StartDecision::RUN: break;
  }
  return Result::COMPLETED;
}

uint16_t active_weather_pct(const WeatherAdjust& w, const Program& p, uint32_t now_ts) {
  return (now_ts < w.valid_until_ts && p.weather_adjust) ? w.pct : 100;
}

bool active_weather_skip(const WeatherAdjust& w, uint32_t now_ts) {
  return w.skip && now_ts < w.valid_until_ts;
}

void Scheduler::reset() {
  last_eval_ = -1;
  for (auto& row : last_fired_day_)
    for (auto& v : row) v = INT32_MIN;
}

std::vector<DueStart> Scheduler::tick(LocalDateTime now, bool time_valid,
                                      const Program (&progs)[kPrograms]) {
  std::vector<DueStart> out;
  if (!time_valid) {
    last_eval_ = -1;
    return out;
  }
  const int64_t cur = static_cast<int64_t>(now.day) * 1440 + now.minute;
  if (last_eval_ == -1) last_eval_ = cur - 1;
  if (cur <= last_eval_) {  // same minute, or clock stepped back (DST fall-back)
    last_eval_ = cur;
    return out;
  }
  if (cur - last_eval_ > 180) last_eval_ = cur - 1;  // long gap: no catch-up

  for (int64_t t = last_eval_ + 1; t <= cur; t++) {
    const int32_t day = static_cast<int32_t>(t / 1440);
    const int16_t minute = static_cast<int16_t>(t % 1440);
    for (int pi = 0; pi < kPrograms; pi++) {
      const Program& p = progs[pi];
      if (!p.enabled) continue;
      for (int s = 0; s < kStarts; s++) {
        if (p.start_minutes[s] != minute) continue;
        if (!is_due_on(p, day)) continue;
        if (last_fired_day_[pi][s] == day) continue;
        last_fired_day_[pi][s] = day;
        out.push_back({static_cast<uint8_t>(pi), static_cast<uint8_t>(s)});
      }
    }
  }
  last_eval_ = cur;
  return out;
}

}  // namespace irrigation
