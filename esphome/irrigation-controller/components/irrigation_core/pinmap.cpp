#include "pinmap.h"

#include <algorithm>

namespace irrigation {

namespace {
constexpr int16_t kMaxGpio = 30;
constexpr int16_t kMcpBase = 100;
constexpr int16_t kMcpCount = 16;
}  // namespace

OutputRef decode_output(int16_t code) {
  if (code >= 0 && code <= kMaxGpio) return OutputRef{OutputKind::GPIO, static_cast<uint8_t>(code)};
  if (code >= kMcpBase && code < kMcpBase + kMcpCount)
    return OutputRef{OutputKind::EXPANDER, static_cast<uint8_t>(code - kMcpBase)};
  return OutputRef{OutputKind::NONE, 0};
}

static std::string mcp_label(int ch) {
  std::string s = "MCP ";
  s += (ch < 8) ? 'A' : 'B';
  s += static_cast<char>('0' + (ch % 8));
  return s;
}

std::string output_label(int16_t code) {
  OutputRef r = decode_output(code);
  switch (r.kind) {
    case OutputKind::GPIO:
      return "GPIO " + std::to_string(r.index);
    case OutputKind::EXPANDER:
      return mcp_label(r.index);
    default:
      return "None";
  }
}

int16_t parse_output_label(const std::string &label) {
  if (label.rfind("GPIO ", 0) == 0) {
    std::string num = label.substr(5);
    if (num.empty() || num.size() > 2) return kNoOutput;
    int v = 0;
    for (char c : num) {
      if (c < '0' || c > '9') return kNoOutput;
      v = v * 10 + (c - '0');
    }
    return (v <= kMaxGpio) ? static_cast<int16_t>(v) : kNoOutput;
  }
  for (int ch = 0; ch < kMcpCount; ch++)
    if (label == mcp_label(ch)) return static_cast<int16_t>(kMcpBase + ch);
  return kNoOutput;
}

std::vector<std::string> output_options(const std::vector<uint8_t> &candidates, bool has_expander) {
  std::vector<std::string> out;
  out.push_back("None");
  std::vector<uint8_t> sorted = candidates;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  for (uint8_t g : sorted) out.push_back("GPIO " + std::to_string(g));
  if (has_expander)
    for (int ch = 0; ch < kMcpCount; ch++) out.push_back(mcp_label(ch));
  return out;
}

static bool contains(const std::vector<uint8_t> &v, uint8_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

PinError validate_assignment(uint8_t slot, int16_t code, const int16_t (&assigned)[kZones + 1], const BoardPins &b) {
  if (slot > kZones) return PinError::INVALID;
  if (code == kNoOutput) return PinError::OK;
  OutputRef r = decode_output(code);
  if (r.kind == OutputKind::NONE) return PinError::INVALID;
  if (r.kind == OutputKind::EXPANDER) {
    if (!b.has_expander) return PinError::NO_EXPANDER;
  } else {
    if (contains(b.reserved, r.index)) return PinError::RESERVED;
    if (!contains(b.candidates, r.index)) return PinError::NOT_CANDIDATE;
  }
  for (int i = 0; i <= kZones; i++)
    if (i != slot && assigned[i] == code) return PinError::DUPLICATE;
  return PinError::OK;
}

const char *pin_error_text(PinError e) {
  switch (e) {
    case PinError::OK:
      return "OK";
    case PinError::NOT_CANDIDATE:
      return "GPIO not available on this board";
    case PinError::RESERVED:
      return "pin is used by a sensor or bus";
    case PinError::DUPLICATE:
      return "already assigned to";
    case PinError::NO_EXPANDER:
      return "no expander fitted";
    default:
      return "invalid output";
  }
}

bool output_on_allowed(uint8_t slot, int active_zone, const int16_t (&assigned)[kZones + 1]) {
  if (slot < kZones) return assigned[slot] != kNoOutput;
  if (slot > kZones) return false;
  // Master: refused while the active zone has no valve output.
  if (active_zone >= 0 && active_zone < kZones) return assigned[active_zone] != kNoOutput;
  return true;
}

Pull park_pull(bool relay_active_high) { return relay_active_high ? Pull::DOWN : Pull::UP; }

}  // namespace irrigation
