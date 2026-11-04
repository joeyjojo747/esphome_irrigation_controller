#include "runner.h"

namespace irrigation {

Runner::Runner(ValveDriver* d) : driver_(d) {}

bool Runner::enqueue_program(uint8_t program, std::vector<Step> steps, uint32_t, bool manual) {
  if (steps.empty() || program >= kPrograms) return false;
  if (queue_.size() >= kMaxQueued) return false;
  for (const auto& i : queue_)
    if (i.program == program) return false;
  queue_.push_back(Instance{next_instance_++, program, manual, false, 0, std::move(steps)});
  return true;
}

void Runner::end_instance_front() {
  Instance done = std::move(queue_.front());
  queue_.pop_front();
  state_ = State::IDLE;
  if (done.started && on_program_finished) on_program_finished(done.id, done.program);
}

void Runner::finish_step(Result r, uint32_t now_s) {
  // Precondition: state_ == RUNNING, queue_ non-empty.
  Instance& inst = queue_.front();
  const Step& s = inst.steps[inst.idx];
  StepEvent ev{inst.id, inst.program, s.zone, start_s_, s.run_s, now_s - start_s_, r};
  if (s.zone < kZones) {
    last_end_s_[s.zone] = now_s;
    has_end_[s.zone] = true;
  }
  inst.idx++;
  state_ = State::IDLE;
  bool last = inst.idx >= inst.steps.size();
  if (on_step_finished) on_step_finished(ev);
  if (last) end_instance_front();
}

void Runner::try_start(uint32_t now_s) {
  while (state_ != State::RUNNING && !queue_.empty()) {
    Instance& inst = queue_.front();
    if (inst.idx >= inst.steps.size()) {  // defensive
      end_instance_front();
      continue;
    }
    const Step& s = inst.steps[inst.idx];
    if (s.soak_s > 0 && s.zone < kZones && has_end_[s.zone] && now_s - last_end_s_[s.zone] < s.soak_s) {
      state_ = State::WAIT_SOAK;
      inst.started = true;
      return;
    }
    inst.started = true;
    state_ = State::RUNNING;
    start_s_ = now_s;
    seen_busy_ = false;
    driver_->start(s.zone, s.run_s);
    if (on_step_started) on_step_started(StepEvent{inst.id, inst.program, s.zone, now_s, s.run_s, 0, Result::COMPLETED});
    return;
  }
  if (queue_.empty()) state_ = State::IDLE;
}

void Runner::stop_all(uint32_t now_s) {
  driver_->stop_all();
  if (queue_.empty()) return;
  // Queued (not yet started) programs are dropped unlogged; only the front instance can be reported.
  queue_.erase(queue_.begin() + 1, queue_.end());
  if (state_ == State::RUNNING) {
    queue_.front().idx = queue_.front().steps.size() - 1;
    finish_step(Result::STOPPED, now_s);
  } else {
    end_instance_front();
  }
}

void Runner::stop_zone_with(Result r, uint32_t now_s) {
  if (state_ != State::RUNNING) return;
  driver_->stop_all();
  finish_step(r, now_s);
  try_start(now_s);
}

void Runner::run_manual_zone(uint8_t zone, uint32_t run_s, uint32_t now_s) {
  stop_all(now_s);
  queue_.push_back(Instance{next_instance_++, kManual, true, false, 0, {Step{zone, run_s, 0}}});
  try_start(now_s);
}

void Runner::set_hold(bool hold, uint32_t now_s) {
  hold_ = hold;
  if (!hold) return;
  // Drop queued non-manual instances behind the front.
  bool front_dropped = false;
  if (!queue_.empty() && !queue_.front().manual) front_dropped = true;
  for (auto it = queue_.begin(); it != queue_.end();) {
    if (!it->manual && !(it == queue_.begin())) it = queue_.erase(it);
    else ++it;
  }
  if (front_dropped) {
    if (state_ == State::RUNNING) {
      driver_->stop_all();
      Instance& inst = queue_.front();
      inst.idx = inst.steps.size() - 1;
      finish_step(Result::STOPPED, now_s);
    } else {
      end_instance_front();
    }
    try_start(now_s);
  }
}

void Runner::loop(uint32_t now_s) {
  if (state_ == State::RUNNING && !queue_.empty()) {
    const Step& s = queue_.front().steps[queue_.front().idx];
    uint32_t elapsed = now_s - start_s_;
    bool idle = driver_->is_idle();
    if (!idle) {
      seen_busy_ = true;
      if (elapsed >= s.run_s + kGraceS) {
        driver_->stop_all();
        finish_step(Result::MAX_RUNTIME, now_s);
      }
    } else if (elapsed >= s.run_s) {
      finish_step(Result::COMPLETED, now_s);
    } else if (seen_busy_ || elapsed > kEarlyIdleIgnoreS) {
      finish_step(Result::STOPPED, now_s);
    }
  }
  if (state_ != State::RUNNING) try_start(now_s);
}

bool Runner::busy() const { return !queue_.empty(); }

std::optional<uint8_t> Runner::active_zone() const {
  if (state_ != State::RUNNING || queue_.empty()) return std::nullopt;
  return queue_.front().steps[queue_.front().idx].zone;
}

std::optional<uint8_t> Runner::active_program() const {
  if (state_ == State::IDLE || queue_.empty()) return std::nullopt;
  return queue_.front().program;
}

}  // namespace irrigation
