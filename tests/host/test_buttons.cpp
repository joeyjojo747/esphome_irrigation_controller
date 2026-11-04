#include "doctest.h"
#include "logic/buttons.h"

using namespace irrigation;

TEST_CASE("stop is always allowed") {
  for (int m = 0; m < 8; m++) {
    ButtonState s{(m & 1) != 0, (m & 2) != 0, (m & 4) != 0};
    CHECK(resolve_button(ButtonFn::STOP_ALL, s) == ButtonCmd::STOP_ALL);
  }
}

TEST_CASE("start is ignored while busy or disabled") {
  CHECK(resolve_button(ButtonFn::RUN_PROGRAM, {true, true, false}) == ButtonCmd::NONE);
  CHECK(resolve_button(ButtonFn::RUN_PROGRAM, {true, false, false}) == ButtonCmd::RUN_PROGRAM);
  CHECK(resolve_button(ButtonFn::RUN_PROGRAM, {false, false, false}) == ButtonCmd::NONE);
}

TEST_CASE("next zone only while busy") {
  CHECK(resolve_button(ButtonFn::NEXT_ZONE, {true, true, false}) == ButtonCmd::NEXT_ZONE);
  CHECK(resolve_button(ButtonFn::NEXT_ZONE, {true, false, false}) == ButtonCmd::NONE);
}

TEST_CASE("rain delay toggles, even when disabled") {
  CHECK(resolve_button(ButtonFn::RAIN_DELAY, {true, false, false}) == ButtonCmd::SET_RAIN_DELAY_24H);
  CHECK(resolve_button(ButtonFn::RAIN_DELAY, {true, false, true}) == ButtonCmd::CLEAR_RAIN_DELAY);
  CHECK(resolve_button(ButtonFn::RAIN_DELAY, {false, false, false}) == ButtonCmd::SET_RAIN_DELAY_24H);
  CHECK(resolve_button(ButtonFn::RAIN_DELAY, {false, false, true}) == ButtonCmd::CLEAR_RAIN_DELAY);
}

TEST_CASE("none maps to none") {
  for (int m = 0; m < 8; m++) {
    ButtonState s{(m & 1) != 0, (m & 2) != 0, (m & 4) != 0};
    CHECK(resolve_button(ButtonFn::NONE, s) == ButtonCmd::NONE);
  }
}

TEST_CASE("led priority") {
  CHECK(led_mode({true, true, false}) == LedMode::FAST_BLINK);
  CHECK(led_mode({true, true, true}) == LedMode::ON);
  CHECK(led_mode({false, true, true}) == LedMode::SLOW_BLINK);
  CHECK(led_mode({false, false, true}) == LedMode::OFF);
}

TEST_CASE("led timing") {
  CHECK(led_level(LedMode::SLOW_BLINK, 0, 0));
  CHECK(led_level(LedMode::SLOW_BLINK, 999, 0));
  CHECK_FALSE(led_level(LedMode::SLOW_BLINK, 1000, 0));
  CHECK_FALSE(led_level(LedMode::SLOW_BLINK, 1999, 0));
  CHECK(led_level(LedMode::SLOW_BLINK, 2000, 0));
  CHECK(led_level(LedMode::FAST_BLINK, 199, 0));
  CHECK_FALSE(led_level(LedMode::FAST_BLINK, 200, 0));
  CHECK_FALSE(led_level(LedMode::FAST_BLINK, 399, 0));
  CHECK(led_level(LedMode::FAST_BLINK, 400, 0));
  CHECK(led_level(LedMode::OFF, 5000, 5001));
  CHECK_FALSE(led_level(LedMode::OFF, 5000, 0));
  CHECK(led_level(LedMode::ON, 5000, 0));
}

TEST_CASE("led ack inverts a steady on") {
  CHECK_FALSE(led_level(LedMode::ON, 5000, 5001));
  CHECK(led_level(LedMode::ON, 5000, 5000));
  CHECK(kLedAckMs == 150);
}

TEST_CASE("led ack is wrap safe") {
  CHECK(led_level(LedMode::OFF, 0xFFFFFFF0u, 0x00000010u));
  CHECK_FALSE(led_level(LedMode::OFF, 0x00000011u, 0x00000010u));
  CHECK_FALSE(led_level(LedMode::OFF, 0x00000010u, 0x00000010u));
}
