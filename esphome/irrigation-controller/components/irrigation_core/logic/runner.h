#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include "planner.h"
#include "types.h"

namespace irrigation {

struct ValveDriver {
  virtual void start(uint8_t zone, uint32_t run_s) = 0;
  virtual void stop_all() = 0;
  virtual bool is_idle() const = 0;
  virtual ~ValveDriver() = default;
};

struct StepEvent {
  uint32_t instance;
  uint8_t program;
  uint8_t zone;
  uint32_t start_s;
  uint32_t planned_s;
  uint32_t actual_s;
  Result result;
};

class Runner {
 public:
  explicit Runner(ValveDriver* d);
  // false: empty, duplicate, or 8 already queued
  bool enqueue_program(uint8_t program, std::vector<Step> steps, uint32_t now_s, bool manual = false);
  void run_manual_zone(uint8_t zone, uint32_t run_s, uint32_t now_s);
  // Current step -> STOPPED; queued programs dropped unlogged.
  void stop_all(uint32_t now_s);
  // Ends current step with r, continues with next step.
  void stop_zone_with(Result r, uint32_t now_s);
  // hold=true: stops and drops only instances with manual=false.
  void set_hold(bool hold, uint32_t now_s);
  void loop(uint32_t now_s);
  bool busy() const;
  std::optional<uint8_t> active_zone() const;
  std::optional<uint8_t> active_program() const;

  std::function<void(const StepEvent&)> on_step_started, on_step_finished;
  std::function<void(uint32_t instance, uint8_t program)> on_program_finished;

  static constexpr uint32_t kGraceS = 120;
  static constexpr size_t kMaxQueued = 8;
  static constexpr uint32_t kEarlyIdleIgnoreS = 5;

 private:
  enum class State : uint8_t { IDLE, WAIT_SOAK, RUNNING };
  struct Instance {
    uint32_t id;
    uint8_t program;
    bool manual;
    bool started;
    size_t idx;
    std::vector<Step> steps;
  };

  void try_start(uint32_t now_s);
  void finish_step(Result r, uint32_t now_s, bool end_instance = false);
  void end_instance_front();

  ValveDriver* driver_;
  std::deque<Instance> queue_;
  State state_ = State::IDLE;
  bool hold_ = false;
  bool seen_busy_ = false;
  uint32_t start_s_ = 0;
  uint32_t next_instance_ = 1;
  uint32_t last_end_s_[kZones] = {};
  bool has_end_[kZones] = {};
};

}  // namespace irrigation
