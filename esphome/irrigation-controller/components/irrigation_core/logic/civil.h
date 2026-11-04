#pragma once
#include <cstdint>

namespace irrigation {

// Howard Hinnant civil-date algorithms (public domain).
int32_t days_from_civil(int y, unsigned m, unsigned d);
void civil_from_days(int32_t z, int& y, unsigned& m, unsigned& d);
unsigned weekday_mon0(int32_t day);  // Monday = 0

}  // namespace irrigation
