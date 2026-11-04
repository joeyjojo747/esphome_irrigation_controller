#include "planner.h"

#include <algorithm>
#include <climits>

#include "civil.h"
#include "schedule.h"

namespace irrigation {

namespace {
constexpr uint32_t kMinRunS = 60;
}

std::vector<Step> expand_program(const Program& p, const Zone (&zones)[kZones], uint8_t zone_count,
                                 uint16_t pct, uint32_t hard_cap_s) {
  std::vector<std::vector<uint32_t>> cycles(kZones);
  std::vector<uint32_t> soak(kZones, 0);
  size_t rounds = 0;
  for (int i = 0; i < kZones && i < zone_count; i++) {
    const Zone& z = zones[i];
    if (p.zone_seconds[i] == 0 || !z.enabled || z.output == kNoOutput) continue;
    uint64_t total = (static_cast<uint64_t>(p.zone_seconds[i]) * pct + 50) / 100;
    if (total < kMinRunS) continue;
    if (total > hard_cap_s) total = hard_cap_s;
    if (p.cycle_soak && z.max_cycle_s > 0 && total > z.max_cycle_s) {
      const uint32_t n = static_cast<uint32_t>((total + z.max_cycle_s - 1) / z.max_cycle_s);
      const uint32_t base = static_cast<uint32_t>(total / n);
      const uint32_t rem = static_cast<uint32_t>(total % n);
      for (uint32_t k = 0; k < n; k++) cycles[i].push_back(base + (k < rem ? 1 : 0));
      soak[i] = z.soak_s;
    } else {
      cycles[i].push_back(static_cast<uint32_t>(total));
    }
    rounds = std::max(rounds, cycles[i].size());
  }
  std::vector<Step> out;
  for (size_t r = 0; r < rounds; r++)
    for (int i = 0; i < kZones; i++)
      if (r < cycles[i].size())
        out.push_back({static_cast<uint8_t>(i), cycles[i][r], r == 0 ? 0u : soak[i]});
  return out;
}

std::vector<PlannedRun> project(const Program (&progs)[kPrograms], const Zone (&zones)[kZones],
                                uint8_t zone_count, const uint8_t (&monthly_pct)[12], uint32_t hard_cap_s,
                                LocalDateTime from, int days,
                                std::function<uint16_t(const Program&, int32_t day)> weather_pct_for) {
  std::vector<PlannedRun> out;
  if (days <= 0) return out;
  const int64_t lo = static_cast<int64_t>(from.day) * 1440 + from.minute;
  const int64_t hi = lo + static_cast<int64_t>(days) * 1440;
  for (int32_t day = from.day; day <= from.day + days; day++) {
    int y;
    unsigned m, d;
    civil_from_days(day, y, m, d);
    for (int pi = 0; pi < kPrograms; pi++) {
      const Program& p = progs[pi];
      if (!p.enabled || !is_due_on(p, day)) continue;
      const uint16_t wpct = weather_pct_for ? weather_pct_for(p, day) : 100;
      const uint16_t pct = effective_pct(p.seasonal_pct, monthly_pct[m - 1], wpct);
      std::vector<Step> steps = expand_program(p, zones, zone_count, pct, hard_cap_s);
      if (steps.empty()) continue;
      uint32_t total = 0;
      bool seen[kZones] = {};
      uint8_t zc = 0;
      for (const auto& s : steps) {
        total += s.run_s;
        if (!seen[s.zone]) {
          seen[s.zone] = true;
          zc++;
        }
      }
      for (int s = 0; s < kStarts; s++) {
        const int16_t mn = p.start_minutes[s];
        if (mn < 0) continue;
        const int64_t t = static_cast<int64_t>(day) * 1440 + mn;
        if (t < lo || t >= hi) continue;
        bool dup = false;
        for (const auto& e : out)
          if (e.program == pi && e.day == day && e.minute == mn) dup = true;
        if (!dup) out.push_back({static_cast<uint8_t>(pi), day, mn, total, pct, zc});
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const PlannedRun& a, const PlannedRun& b) {
    if (a.day != b.day) return a.day < b.day;
    if (a.minute != b.minute) return a.minute < b.minute;
    return a.program < b.program;
  });
  return out;
}

}  // namespace irrigation
