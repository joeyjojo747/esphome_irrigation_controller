#include "doctest.h"
#include "logic/pinmap.h"

using namespace irrigation;

namespace {
BoardPins board(bool expander) {
  BoardPins b;
  b.candidates = {1, 2, 3, 6, 7, 18, 21};
  b.reserved = {10, 11, 14, 19, 20, 22, 23};
  b.has_expander = expander;
  return b;
}
void clear(int16_t (&a)[kZones + 1]) {
  for (int i = 0; i <= kZones; i++) a[i] = kNoOutput;
}
}  // namespace

TEST_CASE("pinmap labels round trip") {
  CHECK(output_label(21) == "GPIO 21");
  CHECK(output_label(100) == "MCP A0");
  CHECK(output_label(115) == "MCP B7");
  CHECK(output_label(-1) == "None");
  for (int16_t c : {int16_t(-1), int16_t(0), int16_t(21), int16_t(30), int16_t(100), int16_t(107), int16_t(108),
                    int16_t(115)})
    CHECK(parse_output_label(output_label(c)) == c);
  CHECK(parse_output_label("GPIO 99") == kNoOutput);
  CHECK(parse_output_label("bogus") == kNoOutput);
  CHECK(decode_output(50).kind == OutputKind::NONE);
  CHECK(decode_output(21).kind == OutputKind::GPIO);
  CHECK(decode_output(109).kind == OutputKind::EXPANDER);
  CHECK(decode_output(109).index == 9);
}

TEST_CASE("pinmap options") {
  std::vector<std::string> want = {"None", "GPIO 1", "GPIO 2", "GPIO 21"};
  CHECK(output_options({1, 2, 21}, false) == want);
  auto with = output_options({1, 2, 21}, true);
  CHECK(with.size() == 20);
  CHECK(with.back() == "MCP B7");
  CHECK(with[4] == "MCP A0");
}

TEST_CASE("pinmap validation without expander") {
  BoardPins b = board(false);
  int16_t a[kZones + 1];
  clear(a);
  a[0] = 1;
  a[kZones] = 21;
  CHECK(validate_assignment(1, 22, a, b) == PinError::RESERVED);
  CHECK(validate_assignment(1, 9, a, b) == PinError::NOT_CANDIDATE);
  CHECK(validate_assignment(1, 1, a, b) == PinError::DUPLICATE);
  CHECK(validate_assignment(1, 21, a, b) == PinError::DUPLICATE);
  CHECK(validate_assignment(1, 100, a, b) == PinError::NO_EXPANDER);
  CHECK(validate_assignment(1, 2, a, b) == PinError::OK);
  CHECK(validate_assignment(0, 1, a, b) == PinError::OK);
  CHECK(validate_assignment(5, kNoOutput, a, b) == PinError::OK);
  CHECK(validate_assignment(1, 50, a, b) == PinError::INVALID);
}

TEST_CASE("pinmap validation with expander") {
  BoardPins b = board(true);
  int16_t a[kZones + 1];
  clear(a);
  CHECK(validate_assignment(1, 100, a, b) == PinError::OK);
  a[0] = 100;
  CHECK(validate_assignment(1, 100, a, b) == PinError::DUPLICATE);
}

TEST_CASE("pinmap error text and park level") {
  CHECK(std::string(pin_error_text(PinError::OK)) == "OK");
  CHECK(std::string(pin_error_text(PinError::NOT_CANDIDATE)) == "GPIO not available on this board");
  CHECK(std::string(pin_error_text(PinError::RESERVED)) == "pin is used by a sensor or bus");
  CHECK(std::string(pin_error_text(PinError::DUPLICATE)) == "already assigned to");
  CHECK(std::string(pin_error_text(PinError::NO_EXPANDER)) == "no expander fitted");
  CHECK(std::string(pin_error_text(PinError::INVALID)) == "invalid output");
  CHECK(park_pull(true) == Pull::DOWN);
  CHECK(park_pull(false) == Pull::UP);
}
