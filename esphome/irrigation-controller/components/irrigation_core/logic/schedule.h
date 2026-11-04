#pragma once
#include <cstdint>
#include <vector>

#include "types.h"

namespace irrigation {

bool is_due_on(const Program& p, int32_t day);

struct DueStart {
  uint8_t program;
  uint8_t slot;
};

class Scheduler {
 public:
  Scheduler() { reset(); }
  std::vector<DueStart> tick(LocalDateTime now, bool time_valid, const Program (&progs)[kPrograms]);
  void reset();

 private:
  int64_t last_eval_;  // minute index (day*1440+minute); -1 = unset
  int32_t last_fired_day_[kPrograms][kStarts];
};

}  // namespace irrigation
