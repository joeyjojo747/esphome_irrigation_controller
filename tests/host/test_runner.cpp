#include <string>
#include <utility>

#include "doctest.h"
#include "runner.h"

using namespace irrigation;

namespace {

struct FakeDriver : ValveDriver {
  struct Start {
    uint8_t zone;
    uint32_t run_s;
  };
  std::vector<Start> starts;
  std::vector<std::string> calls;  // "start" / "stop" in order
  int stop_calls = 0;
  bool idle = true;
  void start(uint8_t zone, uint32_t run_s) override {
    starts.push_back({zone, run_s});
    calls.push_back("start");
  }
  void stop_all() override {
    stop_calls++;
    calls.push_back("stop");
  }
  bool is_idle() const override { return idle; }
};

struct Rig {
  FakeDriver d;
  Runner r{&d};
  std::vector<StepEvent> started, finished;
  std::vector<std::pair<uint32_t, uint8_t>> done;
  Rig() {
    r.on_step_started = [this](const StepEvent& e) { started.push_back(e); };
    r.on_step_finished = [this](const StepEvent& e) { finished.push_back(e); };
    r.on_program_finished = [this](uint32_t i, uint8_t p) { done.push_back({i, p}); };
  }
};

}  // namespace

TEST_CASE("runner runs steps in order") {
  Rig g;
  CHECK(g.r.enqueue_program(0, {{0, 600, 0}, {1, 300, 0}}, 0));
  g.r.loop(0);
  REQUIRE(g.d.starts.size() == 1);
  CHECK(g.d.starts[0].zone == 0);
  CHECK(g.d.starts[0].run_s == 600);
  CHECK(g.started.size() == 1);
  g.d.idle = false;
  g.r.loop(300);
  CHECK(g.finished.empty());
  g.d.idle = true;
  g.r.loop(600);
  REQUIRE(g.finished.size() == 1);
  CHECK(g.finished[0].zone == 0);
  CHECK(g.finished[0].actual_s == 600);
  CHECK(g.finished[0].result == Result::COMPLETED);
  REQUIRE(g.d.starts.size() == 2);
  CHECK(g.d.starts[1].zone == 1);
  CHECK(g.d.starts[1].run_s == 300);
  CHECK(g.r.active_zone().value() == 1);
  CHECK(g.r.active_program().value() == 0);
}

TEST_CASE("runner soak wait") {
  Rig g;
  g.r.enqueue_program(0, {{0, 60, 0}, {0, 60, 300}}, 0);
  g.r.loop(0);
  g.d.idle = false;
  g.r.loop(30);
  g.d.idle = true;
  g.r.loop(60);
  CHECK(g.finished.size() == 1);
  CHECK(g.d.starts.size() == 1);
  g.r.loop(100);
  CHECK(g.d.starts.size() == 1);
  CHECK(g.r.busy());
  CHECK_FALSE(g.r.active_zone().has_value());
  g.r.loop(359);
  CHECK(g.d.starts.size() == 1);
  g.r.loop(360);
  CHECK(g.d.starts.size() == 2);
  CHECK(g.d.starts[1].zone == 0);
}

TEST_CASE("runner queue admission") {
  Rig g;
  CHECK(g.r.enqueue_program(0, {{0, 10, 0}}, 0));
  CHECK_FALSE(g.r.enqueue_program(0, {{0, 10, 0}}, 0));
  CHECK_FALSE(g.r.enqueue_program(1, {}, 0));
  Rig h;
  for (uint8_t p = 0; p < 8; p++) CHECK(h.r.enqueue_program(p, {{p, 10, 0}}, 0));
}

TEST_CASE("runner program finished fires once") {
  Rig g;
  g.r.enqueue_program(2, {{0, 10, 0}, {1, 10, 0}}, 0);
  g.r.loop(0);
  g.d.idle = false;
  g.r.loop(5);
  g.d.idle = true;
  g.r.loop(10);
  CHECK(g.done.empty());
  g.d.idle = false;
  g.r.loop(12);
  g.d.idle = true;
  g.r.loop(20);
  REQUIRE(g.done.size() == 1);
  CHECK(g.done[0].first == 1);
  CHECK(g.done[0].second == 2);
  g.r.loop(30);
  CHECK(g.done.size() == 1);
  CHECK_FALSE(g.r.busy());

  // stop_all ends an instance and reports it once
  Rig h;
  h.r.enqueue_program(3, {{0, 100, 0}, {1, 100, 0}}, 0);
  h.r.loop(0);
  h.d.idle = false;
  h.r.stop_all(10);
  REQUIRE(h.done.size() == 1);
  CHECK(h.done[0].second == 3);
  h.r.loop(20);
  CHECK(h.done.size() == 1);

  // manual run is its own instance
  Rig m;
  m.r.run_manual_zone(4, 30, 0);
  m.d.idle = false;
  m.r.loop(10);
  m.d.idle = true;
  m.r.loop(30);
  REQUIRE(m.done.size() == 1);
  CHECK(m.done[0].second == kManual);
}

TEST_CASE("runner FIFO") {
  Rig g;
  g.r.enqueue_program(0, {{0, 10, 0}, {1, 10, 0}}, 0);
  g.r.enqueue_program(1, {{5, 10, 0}}, 0);
  g.r.loop(0);
  g.d.idle = false;
  g.r.loop(5);
  g.d.idle = true;
  g.r.loop(10);
  CHECK(g.d.starts.size() == 2);
  CHECK(g.d.starts[1].zone == 1);
  g.d.idle = false;
  g.r.loop(12);
  g.d.idle = true;
  g.r.loop(20);
  REQUIRE(g.d.starts.size() == 3);
  CHECK(g.d.starts[2].zone == 5);
  CHECK(g.finished.back().program == 0);
}

TEST_CASE("runner manual run stops queue") {
  Rig g;
  g.r.enqueue_program(0, {{0, 600, 0}, {1, 300, 0}}, 0);
  g.r.enqueue_program(1, {{2, 60, 0}}, 0);
  g.r.loop(0);
  g.d.idle = false;
  g.r.loop(5);
  g.d.calls.clear();
  g.r.run_manual_zone(3, 120, 10);
  REQUIRE(g.finished.size() == 1);
  CHECK(g.finished[0].program == 0);
  CHECK(g.finished[0].zone == 0);
  CHECK(g.finished[0].actual_s == 10);
  CHECK(g.finished[0].result == Result::STOPPED);
  REQUIRE(g.d.calls.size() == 2);
  CHECK(g.d.calls[0] == "stop");
  CHECK(g.d.calls[1] == "start");
  CHECK(g.d.starts.back().zone == 3);
  CHECK(g.d.starts.back().run_s == 120);
  CHECK(g.started.back().program == kManual);
  CHECK(g.r.active_program().value() == kManual);
  // remaining steps are gone
  g.d.idle = true;
  g.r.loop(130);
  CHECK(g.d.starts.size() == 2);
  CHECK_FALSE(g.r.busy());
}

TEST_CASE("runner hold") {
  Rig g;
  g.r.enqueue_program(0, {{0, 600, 0}, {1, 300, 0}}, 0);
  g.r.enqueue_program(1, {{2, 60, 0}}, 0);
  g.r.loop(0);
  g.d.idle = false;
  g.r.set_hold(true, 20);
  REQUIRE(g.finished.size() == 1);
  CHECK(g.finished[0].result == Result::STOPPED);
  CHECK(g.finished[0].actual_s == 20);
  CHECK_FALSE(g.r.busy());
  CHECK(g.done.size() == 1);
  CHECK(g.r.enqueue_program(2, {{3, 10, 0}}, 21));  // glue decides skips
  g.r.run_manual_zone(7, 50, 30);                   // manual run during hold starts
  CHECK(g.d.starts.back().zone == 7);
  CHECK(g.r.busy());
}

TEST_CASE("runner hold spares manual instances") {
  Rig g;
  g.r.enqueue_program(0, {{0, 600, 0}}, 0);
  g.r.enqueue_program(1, {{1, 600, 0}}, 0);
  g.r.enqueue_program(2, {{2, 100, 0}}, 0, true);
  g.r.loop(0);
  g.d.idle = false;
  g.r.set_hold(true, 10);
  REQUIRE(g.finished.size() == 1);
  CHECK(g.finished[0].program == 0);
  CHECK(g.finished[0].result == Result::STOPPED);
  // manual=true instance now runs; P1 was dropped
  REQUIRE(g.d.starts.size() == 2);
  CHECK(g.d.starts[1].zone == 2);
  CHECK(g.r.active_program().value() == 2);
  g.d.idle = true;
  g.r.loop(110);
  CHECK(g.finished.back().result == Result::COMPLETED);
  CHECK_FALSE(g.r.busy());
  CHECK(g.d.starts.size() == 2);
}

TEST_CASE("runner hold keeps a running manual instance") {
  Rig g;
  g.r.enqueue_program(0, {{0, 100, 0}}, 0, true);
  g.r.enqueue_program(1, {{1, 100, 0}}, 0);
  g.r.loop(0);
  g.d.idle = false;
  g.r.set_hold(true, 10);
  CHECK(g.finished.empty());
  CHECK(g.r.active_zone().value() == 0);
  g.d.idle = true;
  g.r.loop(100);
  CHECK(g.d.starts.size() == 1);
  CHECK_FALSE(g.r.busy());
}

TEST_CASE("runner stop_all while idle still stops driver") {
  Rig g;
  g.r.stop_all(5);
  CHECK(g.d.stop_calls == 1);
  CHECK(g.finished.empty());
}

TEST_CASE("runner failsafe max runtime") {
  Rig g;
  g.r.enqueue_program(0, {{0, 100, 0}, {1, 50, 0}}, 0);
  g.r.loop(0);
  g.d.idle = false;
  g.r.loop(100);
  g.r.loop(100 + Runner::kGraceS - 1);
  CHECK(g.d.stop_calls == 0);
  g.r.loop(100 + Runner::kGraceS);
  CHECK(g.d.stop_calls == 1);
  REQUIRE(g.finished.size() == 1);
  CHECK(g.finished[0].result == Result::MAX_RUNTIME);
  CHECK(g.d.starts.size() == 2);
}

TEST_CASE("runner driver idle early is STOPPED") {
  Rig g;
  g.r.enqueue_program(0, {{0, 600, 0}}, 0);
  g.r.loop(0);
  g.d.idle = false;
  g.r.loop(10);
  g.d.idle = true;
  g.r.loop(50);
  REQUIRE(g.finished.size() == 1);
  CHECK(g.finished[0].result == Result::STOPPED);
  CHECK(g.finished[0].actual_s == 50);
}

TEST_CASE("runner ignores early idle right after start") {
  Rig g;  // driver never became busy and is idle
  g.r.enqueue_program(0, {{0, 600, 0}}, 0);
  g.r.loop(0);
  g.r.loop(3);
  CHECK(g.finished.empty());
  g.r.loop(6);
  REQUIRE(g.finished.size() == 1);
  CHECK(g.finished[0].result == Result::STOPPED);
  CHECK(g.finished[0].actual_s == 6);
}

TEST_CASE("runner instance numbering and stop_zone_with") {
  Rig g;
  g.r.enqueue_program(0, {{0, 100, 0}, {1, 100, 0}}, 0);
  g.r.enqueue_program(1, {{2, 100, 0}}, 0);
  g.r.loop(0);
  g.d.idle = false;
  g.r.stop_zone_with(Result::FLOW_HIGH, 30);
  REQUIRE(g.finished.size() == 1);
  CHECK(g.finished[0].result == Result::FLOW_HIGH);
  CHECK(g.finished[0].instance == 1);
  REQUIRE(g.d.starts.size() == 2);
  CHECK(g.d.starts[1].zone == 1);
  CHECK(g.started.back().instance == 1);
  g.r.stop_zone_with(Result::FLOW_HIGH, 40);
  CHECK(g.started.back().instance == 2);
  CHECK(g.done.size() == 1);
}
