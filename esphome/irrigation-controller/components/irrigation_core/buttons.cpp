#include "buttons.h"

namespace irrigation {

ButtonCmd resolve_button(ButtonFn fn, const ButtonState& s) {
  switch (fn) {
    case ButtonFn::STOP_ALL:
      return ButtonCmd::STOP_ALL;
    case ButtonFn::RUN_PROGRAM:
      return (!s.busy && s.controller_enabled) ? ButtonCmd::RUN_PROGRAM : ButtonCmd::NONE;
    case ButtonFn::NEXT_ZONE:
      return s.busy ? ButtonCmd::NEXT_ZONE : ButtonCmd::NONE;
    case ButtonFn::RAIN_DELAY:
      return s.rain_delay_active ? ButtonCmd::CLEAR_RAIN_DELAY : ButtonCmd::SET_RAIN_DELAY_24H;
    case ButtonFn::NONE:
    default:
      return ButtonCmd::NONE;
  }
}

PressKind classify_press(uint32_t held_ms) {
  if (held_ms <= 1000) return PressKind::SHORT;
  if (held_ms >= 2000) return PressKind::LONG;
  return PressKind::OTHER;
}

ButtonCmd resolve_stop_press(ButtonFn short_fn, ButtonFn long_fn, PressKind kind, const ButtonState& s) {
  if (s.busy) return ButtonCmd::STOP_ALL;
  switch (kind) {
    case PressKind::SHORT:
      return resolve_button(short_fn, s);
    case PressKind::LONG:
      return resolve_button(long_fn, s);
    case PressKind::DOWN:
    case PressKind::OTHER:
    default:
      return ButtonCmd::NONE;
  }
}

LedMode led_mode(const LedState& s) {
  if (!s.clock_valid) return LedMode::FAST_BLINK;
  if (s.watering) return LedMode::ON;
  if (s.rain_delay_or_hold) return LedMode::SLOW_BLINK;
  return LedMode::OFF;
}

bool led_level(LedMode m, uint32_t now_ms, uint32_t ack_until_ms) {
  bool base = false;
  switch (m) {
    case LedMode::ON:
      base = true;
      break;
    case LedMode::SLOW_BLINK:
      base = (now_ms % 2000) < 1000;
      break;
    case LedMode::FAST_BLINK:
      base = (now_ms % 400) < 200;
      break;
    case LedMode::OFF:
    default:
      base = false;
      break;
  }
  const bool ack = static_cast<int32_t>(ack_until_ms - now_ms) > 0;
  return ack ? !base : base;
}

}  // namespace irrigation
