#pragma once
#include <cstdint>

namespace irrigation {

enum class ButtonFn : uint8_t { NONE, RUN_PROGRAM, STOP_ALL, NEXT_ZONE, RAIN_DELAY };
enum class ButtonCmd : uint8_t { NONE, RUN_PROGRAM, STOP_ALL, NEXT_ZONE, SET_RAIN_DELAY_24H, CLEAR_RAIN_DELAY };

struct ButtonState {
  bool controller_enabled;
  bool busy;
  bool rain_delay_active;
};

ButtonCmd resolve_button(ButtonFn fn, const ButtonState& s);

// A Stop press: DOWN is the moment it is pressed (length not yet known); SHORT is
// released within 1 s, LONG is held 2 s; OTHER is anything in between.
enum class PressKind : uint8_t { DOWN, SHORT, LONG, OTHER };
PressKind classify_press(uint32_t held_ms);

// Stop is always honoured: while anything is watering or queued (s.busy), every
// Stop press, of any length, is STOP_ALL and is acted on at DOWN. Idle, SHORT and
// LONG run the configured functions (so a long press sets a rain delay only when
// idle), and DOWN / OTHER do nothing.
ButtonCmd resolve_stop_press(ButtonFn short_fn, ButtonFn long_fn, PressKind kind, const ButtonState& s);

enum class LedMode : uint8_t { OFF, ON, SLOW_BLINK, FAST_BLINK };

struct LedState {
  bool watering;
  bool rain_delay_or_hold;
  bool clock_valid;
};

// Priority: clock invalid > watering > rain/hold > off.
LedMode led_mode(const LedState& s);

constexpr uint32_t kLedAckMs = 150;

// ack is active while (int32_t)(ack_until_ms - now_ms) > 0 (wrap-safe).
// An active ack inverts the base level (ON + ack -> off, OFF + ack -> on).
bool led_level(LedMode m, uint32_t now_ms, uint32_t ack_until_ms);

}  // namespace irrigation
