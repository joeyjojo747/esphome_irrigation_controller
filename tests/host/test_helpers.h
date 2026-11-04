#pragma once
#include "types.h"

namespace irrigation {

// Enabled program, every weekday, one start at `minute`.
inline Program daily_at(int16_t minute) {
  Program p = default_program();
  p.enabled = true;
  p.mode = DayMode::WEEKDAYS;
  p.weekday_mask = 0x7F;
  p.start_minutes[0] = minute;
  return p;
}

// Default zone wired to a board GPIO output.
inline Zone assigned_zone(int16_t output = 0) {
  Zone z = default_zone();
  z.output = output;
  return z;
}

}  // namespace irrigation
