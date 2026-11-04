#include "doctest.h"
#include "logic/civil.h"
#include "logic/planner.h"
#include "logic/schedule.h"
#include "logic/types.h"
#include "test_helpers.h"

using namespace irrigation;

static void zones_init(Zone (&z)[kZones]) {
  for (auto& x : z) x = assigned_zone();
}

TEST_CASE("effective pct") {
  CHECK(effective_pct(100, 100, 100) == 100);
  CHECK(effective_pct(150, 120, 130) == 234);
  CHECK(effective_pct(200, 200, 200) == 250);
  CHECK(effective_pct(100, 0, 100) == 0);
}

TEST_CASE("decide_start order") {
  Program p = default_program();
  CHECK(decide_start(p, {false, true, true, true}, 100) == StartDecision::SKIP_CONTROLLER_OFF);
  CHECK(decide_start(p, {true, true, true, true}, 100) == StartDecision::SKIP_RAIN_DELAY);
  CHECK(decide_start(p, {true, false, true, true}, 100) == StartDecision::SKIP_RAIN_SENSOR);
  CHECK(decide_start(p, {true, false, false, true}, 100) == StartDecision::SKIP_WEATHER);
  p.weather_adjust = false;
  CHECK(decide_start(p, {true, false, false, true}, 100) == StartDecision::RUN);
  CHECK(decide_start(p, {true, false, false, false}, 0) == StartDecision::SKIP_WEATHER);
  CHECK(to_result(StartDecision::SKIP_CONTROLLER_OFF) == Result::SKIPPED_CONTROLLER_OFF);
  CHECK(to_result(StartDecision::SKIP_RAIN_DELAY) == Result::SKIPPED_RAIN_DELAY);
  CHECK(to_result(StartDecision::SKIP_RAIN_SENSOR) == Result::SKIPPED_RAIN_SENSOR);
  CHECK(to_result(StartDecision::SKIP_WEATHER) == Result::SKIPPED_WEATHER);
}

TEST_CASE("weather adjust expires") {
  Program p = default_program();
  WeatherAdjust w{60, true, "Rain", 1000};
  CHECK(active_weather_pct(w, p, 999) == 60);
  CHECK(active_weather_skip(w, 999));
  CHECK(active_weather_pct(w, p, 1000) == 100);
  CHECK_FALSE(active_weather_skip(w, 1000));
  p.weather_adjust = false;
  CHECK(active_weather_pct(w, p, 999) == 100);
}

TEST_CASE("expand interleaves cycle and soak") {
  Zone z[kZones];
  zones_init(z);
  z[0].max_cycle_s = 600;
  z[0].soak_s = 1800;
  Program p = default_program();
  p.zone_seconds[0] = 1200;
  p.zone_seconds[1] = 300;
  auto s = expand_program(p, z, 16, 100, 7200);
  REQUIRE(s.size() == 3);
  CHECK((s[0].zone == 0 && s[0].run_s == 600 && s[0].soak_s == 0));
  CHECK((s[1].zone == 1 && s[1].run_s == 300 && s[1].soak_s == 0));
  CHECK((s[2].zone == 0 && s[2].run_s == 600 && s[2].soak_s == 1800));
}

TEST_CASE("expand pct, remainder, minimum, cap, zone_count, disabled") {
  Zone z[kZones];
  zones_init(z);
  Program p = default_program();

  p.zone_seconds[0] = 1000;
  auto s = expand_program(p, z, 16, 150, 7200);
  REQUIRE(s.size() == 1);
  CHECK(s[0].run_s == 1500);

  z[0].max_cycle_s = 400;
  z[0].soak_s = 100;
  s = expand_program(p, z, 16, 100, 7200);
  REQUIRE(s.size() == 3);
  CHECK(s[0].run_s == 334);
  CHECK(s[1].run_s == 333);
  CHECK(s[2].run_s == 333);
  CHECK(s[0].soak_s == 0);
  CHECK(s[1].soak_s == 100);

  p.cycle_soak = false;
  s = expand_program(p, z, 16, 100, 7200);
  REQUIRE(s.size() == 1);
  CHECK(s[0].run_s == 1000);
  p.cycle_soak = true;

  p.zone_seconds[0] = 50;
  CHECK(expand_program(p, z, 16, 100, 7200).empty());

  p.zone_seconds[0] = 9000;
  z[0].max_cycle_s = 0;
  s = expand_program(p, z, 16, 100, 7200);
  REQUIRE(s.size() == 1);
  CHECK(s[0].run_s == 7200);
  z[0].max_cycle_s = 3600;
  z[0].soak_s = 60;
  s = expand_program(p, z, 16, 100, 7200);
  REQUIRE(s.size() == 2);
  CHECK(s[0].run_s + s[1].run_s == 7200);

  p = default_program();
  p.zone_seconds[5] = 600;
  CHECK(expand_program(p, z, 5, 100, 7200).empty());
  CHECK(expand_program(p, z, 6, 100, 7200).size() == 1);
  z[5].enabled = false;
  CHECK(expand_program(p, z, 16, 100, 7200).empty());
  z[5].enabled = true;
  z[5].output = kNoOutput;
  CHECK(expand_program(p, z, 16, 100, 7200).empty());
}

TEST_CASE("project 3 days") {
  Program ps[kPrograms];
  for (auto& x : ps) x = default_program();
  ps[0] = daily_at(360);
  ps[0].zone_seconds[0] = 600;
  Zone z[kZones];
  zones_init(z);
  uint8_t monthly[12];
  for (auto& m : monthly) m = 100;
  const int32_t d0 = days_from_civil(2026, 11, 2);
  auto r = project(ps, z, 16, monthly, 7200, LocalDateTime{d0, 400}, 3,
                   [](const Program&, int32_t) -> uint16_t { return 80; });
  REQUIRE(r.size() == 3);
  CHECK(r[0].day == d0 + 1);
  CHECK(r[2].day == d0 + 3);
  CHECK(r[0].minute == 360);
  CHECK(r[0].total_s == 480);
  CHECK(r[0].pct == 80);
  CHECK(r[0].zone_count == 1);
  CHECK(r[0].program == 0);

  r = project(ps, z, 16, monthly, 7200, LocalDateTime{d0, 300}, 1,
              [](const Program&, int32_t) -> uint16_t { return 100; });
  REQUIRE(r.size() == 1);
  CHECK(r[0].day == d0);
}

TEST_CASE("effective pct half-up boundary") {
  CHECK(effective_pct(1, 50, 100) == 1);   // 0.5 rounds up
  CHECK(effective_pct(1, 49, 100) == 0);   // 0.49 rounds down
  CHECK(effective_pct(3, 50, 100) == 2);   // 1.5 rounds up
}

TEST_CASE("project applies hard cap and dedupes identical starts") {
  Program ps[kPrograms];
  for (auto& x : ps) x = default_program();
  ps[0] = daily_at(360);
  ps[0].start_minutes[1] = 360;  // identical second slot
  ps[0].zone_seconds[0] = 9000;
  ps[0].cycle_soak = false;
  Zone z[kZones];
  zones_init(z);
  uint8_t monthly[12];
  for (auto& m : monthly) m = 100;
  const int32_t d0 = days_from_civil(2026, 11, 2);
  auto r = project(ps, z, 16, monthly, 7200, LocalDateTime{d0, 0}, 1,
                   [](const Program&, int32_t) -> uint16_t { return 100; });
  REQUIRE(r.size() == 1);
  CHECK(r[0].total_s == 7200);
}
