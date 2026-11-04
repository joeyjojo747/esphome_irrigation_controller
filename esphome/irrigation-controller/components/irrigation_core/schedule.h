#pragma once
#include <cstdint>
#include <vector>

#include "types.h"

namespace irrigation {

bool is_due_on(const Program& p, int32_t day);

// program x month x weather / 10000, rounded half up, clamped to 0..250.
uint16_t effective_pct(uint8_t program_pct, uint8_t month_pct, uint16_t weather_pct);

struct StartContext {
  bool controller_enabled;
  bool rain_delay_active;
  bool rain_hold;
  bool weather_skip_valid;
};

enum class StartDecision : uint8_t { RUN, SKIP_CONTROLLER_OFF, SKIP_RAIN_DELAY, SKIP_RAIN_SENSOR, SKIP_WEATHER };

StartDecision decide_start(const Program& p, const StartContext& c, uint16_t pct);
Result to_result(StartDecision d);

struct WeatherAdjust {
  uint16_t pct;
  bool skip;
  char reason[32];
  uint32_t valid_until_ts;
};

uint16_t active_weather_pct(const WeatherAdjust& w, const Program& p, uint32_t now_ts);
bool active_weather_skip(const WeatherAdjust& w, uint32_t now_ts);

struct DueStart {
  uint8_t program;
  uint8_t slot;
};

class Scheduler {
 public:
  Scheduler() { reset(); }
  std::vector<DueStart> tick(LocalDateTime now, bool time_valid, const Program (&progs)[kPrograms]);
  void reset();
  // Clears program i's once-per-day guard (call after editing it); out of range is ignored.
  void reset_program(uint8_t i);

 private:
  int64_t last_eval_;  // minute index (day*1440+minute); -1 = unset
  int32_t last_fired_day_[kPrograms][kStarts];
};

}  // namespace irrigation
