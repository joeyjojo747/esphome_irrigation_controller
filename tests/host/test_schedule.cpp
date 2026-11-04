#include "doctest.h"
#include "logic/civil.h"
#include "logic/schedule.h"
#include "logic/types.h"
#include "test_helpers.h"

using namespace irrigation;

static void init(Program (&ps)[kPrograms]) {
  for (auto& p : ps) p = default_program();
}

TEST_CASE("civil") {
  CHECK(days_from_civil(1970, 1, 1) == 0);
  CHECK(weekday_mon0(days_from_civil(2026, 11, 2)) == 0);  // Monday
  CHECK(weekday_mon0(0) == 3);                              // Thursday
  int y; unsigned m, d;
  civil_from_days(days_from_civil(2028, 2, 29), y, m, d);
  CHECK((y == 2028 && m == 2 && d == 29));
  civil_from_days(-1, y, m, d);
  CHECK((y == 1969 && m == 12 && d == 31));
}

TEST_CASE("defaults") {
  Program p = default_program();
  CHECK_FALSE(p.enabled);
  CHECK(p.start_minutes[3] == -1);
  CHECK(p.seasonal_pct == 100);
  Zone z = default_zone();
  CHECK(z.output == kNoOutput);
  CHECK(z.flow_high_pct == 130);
}

TEST_CASE("day modes") {
  Program p = default_program(); p.enabled = true;
  p.mode = DayMode::WEEKDAYS; p.weekday_mask = 0b0000101;  // Mon, Wed
  CHECK(is_due_on(p, days_from_civil(2026, 11, 2)));
  CHECK_FALSE(is_due_on(p, days_from_civil(2026, 11, 3)));
  p.mode = DayMode::ODD;
  CHECK(is_due_on(p, days_from_civil(2026, 10, 29)));
  CHECK_FALSE(is_due_on(p, days_from_civil(2026, 10, 31)));  // 31st excluded
  p.mode = DayMode::EVEN; CHECK(is_due_on(p, days_from_civil(2026, 10, 30)));
  p.mode = DayMode::INTERVAL; p.interval_days = 3; p.interval_anchor_day = days_from_civil(2026, 11, 1);
  CHECK_FALSE(is_due_on(p, days_from_civil(2026, 10, 29)));  // before anchor
  CHECK(is_due_on(p, days_from_civil(2026, 11, 1)));
  CHECK_FALSE(is_due_on(p, days_from_civil(2026, 11, 2)));
  CHECK(is_due_on(p, days_from_civil(2026, 11, 4)));
}

TEST_CASE("fires once at start minute") {
  Program ps[kPrograms]; init(ps); ps[0] = daily_at(360);
  Scheduler s; int32_t d = days_from_civil(2026, 11, 2);
  CHECK(s.tick({d, 359}, true, ps).empty());
  auto due = s.tick({d, 360}, true, ps); REQUIRE(due.size() == 1); CHECK(due[0].program == 0);
  CHECK(s.tick({d, 360}, true, ps).empty());  // same minute again
}

TEST_CASE("spring forward fires skipped 02:30 at 03:00") {
  Program ps[kPrograms]; init(ps); ps[0] = daily_at(150);
  Scheduler s; int32_t d = days_from_civil(2026, 10, 4);
  CHECK(s.tick({d, 119}, true, ps).empty());
  auto due = s.tick({d, 180}, true, ps);
  REQUIRE(due.size() == 1);
  CHECK(due[0].program == 0);
  CHECK(due[0].slot == 0);
}

TEST_CASE("fall back fires 02:30 only once") {
  Program ps[kPrograms]; init(ps); ps[0] = daily_at(150);
  Scheduler s; int32_t d = days_from_civil(2027, 4, 4);
  size_t total = 0;
  for (int m = 149; m <= 179; m++) total += s.tick({d, (int16_t)m}, true, ps).size();
  for (int m = 120; m <= 151; m++) total += s.tick({d, (int16_t)m}, true, ps).size();
  CHECK(total == 1);
}

TEST_CASE("no catch-up after clock becomes valid") {
  Program ps[kPrograms]; init(ps); ps[0] = daily_at(360);
  Scheduler s; int32_t d = days_from_civil(2026, 11, 2);
  CHECK(s.tick({d, 0}, false, ps).empty());
  CHECK(s.tick({d, 430}, true, ps).empty());
  CHECK(s.tick({d, 431}, true, ps).empty());
}

TEST_CASE("gap over 180 minutes does not fire old starts") {
  Program ps[kPrograms]; init(ps); ps[0] = daily_at(360);
  Scheduler s; int32_t d = days_from_civil(2026, 11, 2);
  CHECK(s.tick({d, 100}, true, ps).empty());
  CHECK(s.tick({d, 400}, true, ps).empty());
}

TEST_CASE("disabled program and unused slot never fire") {
  Program ps[kPrograms]; init(ps);
  ps[0] = daily_at(360); ps[0].enabled = false;
  Scheduler s; int32_t d = days_from_civil(2026, 11, 2);
  size_t total = 0;
  for (int m = 0; m < 1440; m++) total += s.tick({d, (int16_t)m}, true, ps).size();
  CHECK(total == 0);  // disabled
  ps[0] = daily_at(360); ps[0].start_minutes[0] = -1;  // enabled, all slots unused
  s.reset();
  for (int m = 0; m < 1440; m++) total += s.tick({d, (int16_t)m}, true, ps).size();
  CHECK(total == 0);
}

TEST_CASE("fires again next day") {
  Program ps[kPrograms]; init(ps); ps[0] = daily_at(0);
  Scheduler s; int32_t d = days_from_civil(2026, 11, 2);
  CHECK(s.tick({d, 1438}, true, ps).empty());
  CHECK(s.tick({d, 1439}, true, ps).empty());
  CHECK(s.tick({d + 1, 0}, true, ps).size() == 1);  // midnight rollover
}
