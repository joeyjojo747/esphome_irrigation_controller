#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "planner.h"
#include "types.h"

namespace irrigation {

struct RunRecord {
  uint32_t seq;
  uint32_t instance;
  uint8_t program;
  uint8_t zone;
  uint32_t start_ts;
  uint32_t planned_s;
  uint32_t actual_s;
  float litres;
  Result result;
};

class RunLog {
 public:
  static constexpr size_t kCap = 48;
  uint32_t add(RunRecord r);  // assigns seq (starts 1)
  std::vector<RunRecord> since(uint32_t seq) const;
  uint32_t last_seq() const;

 private:
  RunRecord buf_[kCap]{};
  size_t count_ = 0;
  size_t head_ = 0;  // next write slot
  uint32_t seq_ = 0;
};

const char* result_name(Result r);
std::string record_json(const RunRecord& r);
std::string records_json(const std::vector<RunRecord>& v);
// At most max_entries runs are emitted; *truncated (if given) says whether any were dropped.
constexpr size_t kPlanMaxEntries = 256;
std::string plan_json(const std::vector<PlannedRun>& v, size_t max_entries = kPlanMaxEntries,
                      bool* truncated = nullptr);
// Cut s to at most max_bytes without splitting a UTF-8 code point.
std::string truncate_utf8(const std::string& s, size_t max_bytes);

}  // namespace irrigation
