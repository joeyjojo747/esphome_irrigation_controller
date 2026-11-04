#include "doctest.h"
#include "logic/flow.h"

using namespace irrigation;

namespace {
FlowMonitor started() {
  FlowMonitor m;
  m.configure(FlowSettings{});
  m.zone_started(1, 10.0f, 130, 70, 0);
  return m;
}
}  // namespace

TEST_CASE("flow: no high during settle") {
  auto m = started();
  for (uint32_t t = 0; t < 60; t++) CHECK(m.update(20.0f, t) == FlowEvent::NONE);
}

TEST_CASE("flow: high once at t=90") {
  auto m = started();
  int n = 0;
  for (uint32_t t = 0; t < 200; t++) {
    FlowEvent e = m.update(t < 60 ? 10.0f : 14.0f, t);
    if (e == FlowEvent::HIGH) {
      n++;
      CHECK(t == 90);
    } else {
      CHECK(e == FlowEvent::NONE);
    }
  }
  CHECK(n == 1);
}

TEST_CASE("flow: low once at t=90") {
  auto m = started();
  int n = 0;
  for (uint32_t t = 0; t < 200; t++) {
    FlowEvent e = m.update(t < 60 ? 10.0f : 6.0f, t);
    if (e == FlowEvent::LOW) {
      n++;
      CHECK(t == 90);
    } else {
      CHECK(e == FlowEvent::NONE);
    }
  }
  CHECK(n == 1);
}

TEST_CASE("flow: zero flow is low, never leak") {
  auto m = started();
  int low = 0;
  for (uint32_t t = 0; t < 400; t++) {
    FlowEvent e = m.update(0.0f, t);
    CHECK(e != FlowEvent::LEAK);
    CHECK(e != FlowEvent::HIGH);
    if (e == FlowEvent::LOW) low++;
  }
  CHECK(low == 1);
}

TEST_CASE("flow: no baseline never high or low") {
  FlowMonitor m;
  m.zone_started(1, 0.0f, 130, 70, 0);
  for (uint32_t t = 0; t < 300; t++) CHECK(m.update(t % 2 ? 0.0f : 50.0f, t) == FlowEvent::NONE);
}

TEST_CASE("flow: learning") {
  {
    FlowMonitor m;
    m.zone_started(1, 0.0f, 130, 70, 0);
    for (uint32_t t = 0; t < 300; t++) m.update(12.0f, t);
    auto r = m.zone_stopped(300);
    REQUIRE(r.has_value());
    CHECK(*r == doctest::Approx(12.0f));
  }
  {
    auto m = started();
    for (uint32_t t = 0; t < 300; t++) m.update(12.0f, t);
    auto r = m.zone_stopped(300);
    REQUIRE(r.has_value());
    CHECK(*r == doctest::Approx(10.6f).epsilon(0.01));
  }
  {
    auto m = started();
    for (uint32_t t = 0; t < 160; t++) m.update(10.0f, t);
    CHECK_FALSE(m.zone_stopped(160).has_value());
  }
  {
    auto m = started();
    for (uint32_t t = 0; t < 300; t++) m.update(t < 60 ? 10.0f : 14.0f, t);
    CHECK_FALSE(m.zone_stopped(300).has_value());
  }
}

TEST_CASE("flow: no leak during drain-down, leak after") {
  auto m = started();
  for (uint32_t t = 0; t < 300; t++) m.update(10.0f, t);
  m.zone_stopped(300);
  for (uint32_t t = 300; t < 360; t++) CHECK(m.update(2.0f, t) == FlowEvent::NONE);
  int n = 0;
  for (uint32_t t = 360; t < 500; t++) {
    FlowEvent e = m.update(2.0f, t);
    if (e == FlowEvent::LEAK) {
      n++;
      CHECK(t == 420);
    } else {
      CHECK(e == FlowEvent::NONE);
    }
  }
  CHECK(n == 1);
  m.update(0.0f, 500);
  int again = 0;
  for (uint32_t t = 501; t < 700; t++)
    if (m.update(2.0f, t) == FlowEvent::LEAK) again++;
  CHECK(again == 1);
}

TEST_CASE("safety: exceeded at cap + 10") {
  SafetyMonitor s;
  CHECK_FALSE(s.exceeded(uint8_t{3}, 0, 7200));
  CHECK_FALSE(s.exceeded(uint8_t{3}, 7209, 7200));
  CHECK(s.exceeded(uint8_t{3}, 7210, 7200));
  CHECK_FALSE(s.exceeded(uint8_t{4}, 7211, 7200));  // zone changed: reset
  CHECK(s.exceeded(uint8_t{4}, 14421, 7200));
  CHECK_FALSE(s.exceeded(std::nullopt, 14422, 7200));
  CHECK_FALSE(s.exceeded(uint8_t{4}, 14423, 7200));  // reset after nullopt
}

TEST_CASE("rain window") {
  RainWindow r;
  for (int i = 0; i < 10; i++) r.add_tip(0);
  CHECK(r.mm_last_24h(0, 0.2f) == doctest::Approx(2.0f));
  CHECK(r.mm_last_24h(86400 + 3600, 0.2f) == doctest::Approx(0.0f));
}
