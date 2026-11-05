#include "runlog.h"

#include <cstdio>

#include "civil.h"

namespace irrigation {

uint32_t RunLog::add(RunRecord r) {
  r.seq = ++seq_;
  buf_[head_] = r;
  head_ = (head_ + 1) % kCap;
  if (count_ < kCap) count_++;
  return r.seq;
}

std::vector<RunRecord> RunLog::since(uint32_t seq) const {
  std::vector<RunRecord> out;
  size_t start = (head_ + kCap - count_) % kCap;  // oldest
  for (size_t i = 0; i < count_; i++) {
    const RunRecord& r = buf_[(start + i) % kCap];
    if (r.seq > seq) out.push_back(r);
  }
  return out;
}

uint32_t RunLog::last_seq() const { return seq_; }

const char* result_name(Result r) {
  switch (r) {
    case Result::COMPLETED: return "completed";
    case Result::STOPPED: return "stopped";
    case Result::FLOW_HIGH: return "flow_high";
    case Result::FLOW_LOW: return "flow_low";
    case Result::MAX_RUNTIME: return "max_runtime";
    case Result::SKIPPED_CONTROLLER_OFF: return "skipped_controller_off";
    case Result::SKIPPED_RAIN_DELAY: return "skipped_rain_delay";
    case Result::SKIPPED_RAIN_SENSOR: return "skipped_rain_sensor";
    case Result::SKIPPED_WEATHER: return "skipped_weather";
  }
  return "unknown";
}

std::string record_json(const RunRecord& r) {
  char buf[224];
  snprintf(buf, sizeof(buf),
           "{\"seq\":%u,\"inst\":%u,\"p\":%u,\"z\":%u,\"ts\":%u,\"plan\":%u,\"act\":%u,\"l\":%.1f,\"r\":\"%s\"}",
           (unsigned) r.seq, (unsigned) r.instance, (unsigned) r.program, (unsigned) r.zone, (unsigned) r.start_ts,
           (unsigned) r.planned_s, (unsigned) r.actual_s, (double) r.litres, result_name(r.result));
  return buf;
}

std::string records_json(const std::vector<RunRecord>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); i++) {
    if (i) s += ",";
    s += record_json(v[i]);
  }
  s += "]";
  return s;
}

std::string truncate_utf8(const std::string& s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  size_t n = max_bytes;
  // Back up over continuation bytes (10xxxxxx) so the next kept byte starts a code point.
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
  return s.substr(0, n);
}

std::string plan_json(const std::vector<PlannedRun>& v, size_t max_entries, bool* truncated) {
  if (truncated) *truncated = v.size() > max_entries;
  std::string s = "[";
  for (size_t i = 0; i < v.size() && i < max_entries; i++) {
    const PlannedRun& p = v[i];
    int y;
    unsigned m, d;
    civil_from_days(p.day, y, m, d);
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"p\":%u,\"date\":\"%04d-%02u-%02u\",\"start\":\"%02d:%02d\",\"s\":%u,\"pct\":%u,\"zones\":%u}",
             (unsigned) p.program, y, m, d, p.minute / 60, p.minute % 60, (unsigned) p.total_s, (unsigned) p.pct,
             (unsigned) p.zone_count);
    if (i) s += ",";
    s += buf;
  }
  s += "]";
  return s;
}

}  // namespace irrigation
