#pragma once
#include <cstdint>

namespace irrigation {

enum class DayMode : uint8_t { WEEKDAYS, ODD, EVEN, INTERVAL };
enum class HeadType : uint8_t { SPRAY, ROTOR, ROTATOR, DRIP, BUBBLER };
enum class Soil : uint8_t { SAND, LOAMY_SAND, SANDY_LOAM, LOAM, CLAY_LOAM, CLAY };
enum class Sun : uint8_t { FULL, PART, SHADE };
enum class Plant : uint8_t { LAWN, SHRUBS, TREES, GARDEN_BEDS, NATIVES, VEG };
enum class FlowAction : uint8_t { ALERT, ALERT_AND_STOP };
enum class RainType : uint8_t { NONE, SWITCH_NC, SWITCH_NO, TIPPING };
enum class Result : uint8_t {
  COMPLETED,
  STOPPED,
  FLOW_HIGH,
  FLOW_LOW,
  MAX_RUNTIME,
  SKIPPED_CONTROLLER_OFF,
  SKIPPED_RAIN_DELAY,
  SKIPPED_RAIN_SENSOR,
  SKIPPED_WEATHER
};

constexpr int kZones = 16;
constexpr int kPrograms = 8;
constexpr int kStarts = 4;
constexpr uint8_t kManual = 255;

struct Program {
  bool enabled;
  DayMode mode;
  uint8_t weekday_mask;  // bit 0 = Monday
  uint8_t interval_days;
  int32_t interval_anchor_day;
  int16_t start_minutes[kStarts];  // -1 = unused
  uint16_t zone_seconds[kZones];
  uint8_t seasonal_pct;
  bool cycle_soak;
  bool weather_adjust;
};

inline Program default_program() {
  Program p{};
  p.enabled = false;
  p.mode = DayMode::WEEKDAYS;
  p.weekday_mask = 0;
  p.interval_days = 1;
  p.interval_anchor_day = 0;
  for (int i = 0; i < kStarts; i++) p.start_minutes[i] = -1;
  for (int i = 0; i < kZones; i++) p.zone_seconds[i] = 0;
  p.seasonal_pct = 100;
  p.cycle_soak = true;
  p.weather_adjust = true;
  return p;
}

// Zone output codes: -1 none, 0..30 board GPIO n,
// 100..115 MCP23017 channel (A0..A7 = 100..107, B0..B7 = 108..115).
constexpr int16_t kNoOutput = -1;

struct Zone {
  bool enabled;
  HeadType head;
  float precip_mm_h;
  Soil soil;
  uint8_t slope_pct;
  Sun sun;
  Plant plant;
  float area_m2;
  uint16_t max_cycle_s;
  uint16_t soak_s;
  float flow_baseline_lpm;
  uint8_t flow_high_pct;
  uint8_t flow_low_pct;
  FlowAction flow_action;
  int16_t output;
};

inline Zone default_zone() {
  Zone z{};
  z.enabled = true;
  z.head = HeadType::SPRAY;
  z.precip_mm_h = 40.0f;
  z.soil = Soil::LOAM;
  z.slope_pct = 0;
  z.sun = Sun::FULL;
  z.plant = Plant::LAWN;
  z.area_m2 = 0;
  z.max_cycle_s = 0;
  z.soak_s = 0;
  z.flow_baseline_lpm = 0.0f;
  z.flow_high_pct = 130;
  z.flow_low_pct = 70;
  z.flow_action = FlowAction::ALERT;
  z.output = kNoOutput;
  return z;
}

struct LocalDateTime {
  int32_t day;     // days since 1970-01-01, local time
  int16_t minute;  // 0..1439
};

}  // namespace irrigation
