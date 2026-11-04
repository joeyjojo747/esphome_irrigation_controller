#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "types.h"

namespace irrigation {

struct Step {
  uint8_t zone;
  uint32_t run_s;
  uint32_t soak_s;
};

std::vector<Step> expand_program(const Program& p, const Zone (&zones)[kZones], uint8_t zone_count,
                                 uint16_t pct, uint32_t hard_cap_s);

struct PlannedRun {
  uint8_t program;
  int32_t day;
  int16_t minute;
  uint32_t total_s;
  uint16_t pct;
  uint8_t zone_count;
};

// Rain/weather skips are not modelled: only pct == 0 (empty expansion) drops a run.
std::vector<PlannedRun> project(const Program (&progs)[kPrograms], const Zone (&zones)[kZones],
                                uint8_t zone_count, const uint8_t (&monthly_pct)[12], uint32_t hard_cap_s,
                                LocalDateTime from, int days,
                                std::function<uint16_t(const Program&, int32_t day)> weather_pct_for);

}  // namespace irrigation
