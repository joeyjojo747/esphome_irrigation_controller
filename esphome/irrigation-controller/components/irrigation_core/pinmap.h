#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "types.h"

namespace irrigation {

enum class OutputKind : uint8_t { NONE, GPIO, EXPANDER };
struct OutputRef {
  OutputKind kind;
  uint8_t index;  // GPIO n, or expander channel 0..15
};

OutputRef decode_output(int16_t code);  // invalid codes -> NONE
std::string output_label(int16_t code);  // "None", "GPIO 21", "MCP A0".."MCP B7"
int16_t parse_output_label(const std::string &label);  // inverse; unknown -> kNoOutput
// "None" first, GPIOs ascending, then MCP A0..B7 (only when has_expander)
std::vector<std::string> output_options(const std::vector<uint8_t> &candidates, bool has_expander);

enum class PinError : uint8_t { OK, NOT_CANDIDATE, RESERVED, DUPLICATE, NO_EXPANDER, INVALID };
struct BoardPins {
  std::vector<uint8_t> candidates;
  std::vector<uint8_t> reserved;
  bool has_expander;
};

// slot 0..15 = zones, 16 = master; assigned[] = current codes
PinError validate_assignment(uint8_t slot, int16_t code, const int16_t (&assigned)[kZones + 1], const BoardPins &b);
const char *pin_error_text(PinError e);

// Whether a slot may be switched on. A zone with no output never starts, and the
// master never energises while the active zone (-1 = none) has no output, so a
// pump or master valve cannot run against closed valves. Off is always allowed.
bool output_on_allowed(uint8_t slot, int active_zone, const int16_t (&assigned)[kZones + 1]);

enum class Pull : uint8_t { DOWN, UP };
// Released GPIO: input with pull to the OFF level (active-high: DOWN, active-low: UP).
Pull park_pull(bool relay_active_high);

}  // namespace irrigation
