<!-- SCREENSHOT: header.png (Task 20 Step 2, after Task 9/19) -->

# ESPHome Irrigation Controller

A 16-zone irrigation controller for Home Assistant, built on an ESP32-C6 running ESPHome.

Two design goals:

1. **Fully configurable through Home Assistant** with no YAML reload, no Home Assistant restart and
   no ESP reflash after install. Zone count and names, programs, every setting, and which output
   drives each zone are edited in the dashboard.
2. **Autonomous scheduled watering** when Home Assistant or Wi-Fi is unavailable. Schedules, rain
   and flow protection run on the controller itself.

## What it is

ESPHome firmware (built in the ESPHome Device Builder) plus a set of Home Assistant YAML packages.
It needs no Python, no add-on and no custom integration on the Home Assistant side. Home Assistant
holds the settings, weather adjustment, notifications and dashboards; the ESP holds and runs the
schedule.

## How it compares

Other projects solve parts of this problem, and some of this one is built on them. This is a short
description of what this project does differently, not a ranking; check each project's own page for
what it offers today.

- **[ESPHome Sprinkler](https://esphome.io/components/sprinkler/)** is an ESPHome component for
  sequencing valves and a pump. This project uses it for valve and pump ordering, and adds the
  schedule, programs, queue, flow and rain handling, and Home Assistant configuration on top.
- **[Irrigation Unlimited](https://github.com/rgc99/irrigation_unlimited)** is a Home Assistant
  custom integration that does its scheduling in Home Assistant. This project keeps the schedule on
  the ESP, so it keeps running when Home Assistant is down, and uses plain YAML packages instead of
  a custom integration.
- **[ESPyRain](https://github.com/juhurus/ESPyRain_Controller)** is an ESPHome controller with the
  same split between ESP and Home Assistant YAML packages, and it is the model for this project's
  structure and for choosing output pins at runtime. This project adds validation of those
  assignments, flow monitoring and cost, AI advice and a site plan.

## Key features

### Configuration

<!-- SCREENSHOT: configuration.png (Task 20 Step 2, after Task 9/19) -->

- 1 to 16 zones with names, enable flags and per-zone settings, edited from the dashboard.
- The relay output for each zone and for the master valve or pump (a board GPIO or an MCP23017
  channel) and the relay trigger level are chosen in Home Assistant. Invalid choices are rejected with
  the reason shown, and nothing changes.
- Config backup and restore through Home Assistant, to rebuild a replaced device.

### Scheduling

<!-- SCREENSHOT: scheduling.png (Task 20 Step 2, after Task 9/19) -->

- 8 programs (A to H), each with 4 start times, day modes and a per-zone run time, and a queue of
  up to 8 programs.
- Cycle and soak per zone, run by the controller. Home Assistant can suggest values from soil, slope
  and head type.
- Seasonal percentage per program and a monthly fallback table.
- Start and Stop buttons on the controller, with functions chosen in Home Assistant.

### Weather & AI

<!-- SCREENSHOT: weather-ai.png (Task 20 Step 2, after Task 9/19) -->

- Daily adjustment from your BOM weather entity, with skips for rain, rain probability, freeze,
  wind and cumulative rain.
- Optional AI advice that works with any AI Task entity or conversation agent you already use.
  Choose `AI Task`, `Conversation agent` or `Off`. With AI off, rule-based adjustment still works.

### Flow & water cost

<!-- SCREENSHOT: flow-cost.png (Task 20 Step 2, after Task 9/19) -->

- A flow meter gives litres per run and per zone, with learned baselines and high flow, low flow
  and leak alarms. A maximum run time is a separate failsafe.
- Daily, monthly and yearly use, and cost from a tariff you set (default 3.00 $/kL).

### Rain sensor

<!-- SCREENSHOT: rain-sensor.png (Task 20 Step 2, after Task 9/19) -->

- Type chosen in Home Assistant: none, switch NC, switch NO or tipping bucket.
- Rain delay by hours, from the dashboard or a button press. Rain is handled on the controller.

### Calendar

<!-- SCREENSHOT: calendar.png (Task 20 Step 2, after Task 9/19) -->

- Finished and skipped runs are written to a Local Calendar named `Irrigation`.
- Upcoming runs are shown from the controller's own plan, and a run history is kept.

### Site plan

<!-- SCREENSHOT: site-plan.png (Task 20 Step 2, after Task 9/19) -->

- An SVG plan of the property shown with ha-floorplan: zones colour by state, and a tap runs a zone.
  You draw your own zones and heads over a satellite image.

### Notifications

<!-- SCREENSHOT: notifications.png (Task 20 Step 2, after Task 9/19) -->

- Skips with the reason, run start and finish, flow alarms, leak, controller offline and clock
  invalid, to the notify targets you choose, with quiet hours.

### Dashboards

<!-- SCREENSHOT: dashboards.png (Task 20 Step 2, after Task 9/19) -->

- Views for overview, site plan, calendar, programs, zones, water and settings. The overview,
  programs and zones views use native cards that you can replace with Ultra Card exports.

## Hardware

- DFRobot FireBeetle 2 ESP32-C6
- One Keyestudio KS0011 relay per zone, plus one for an optional master valve or pump
- A hall-effect flow meter
- A rain sensor (reed switch or tipping bucket)
- Start and Stop push buttons
- Optional: status LED, DS3231 RTC, MCP23017 relay expander (for more than 6 zones)

| Function | Pin | Notes |
|---|---|---|
| Zone relays (first-boot defaults, reassignable in HA) | GPIO 1, 2, 3, 6, 7, 18 | One KS0011 per zone, active-high, 5 V relay supply, signal from the 3.3 V GPIO. |
| Master valve or pump relay (reassignable in HA) | GPIO 21 | Optional; set to `None` if not fitted. |
| Flow sensor | GPIO 23 | Pulses per litre configurable (default 450). A 5 V sensor needs a 10 k / 20 k divider. |
| Rain sensor | GPIO 22 | Type chosen in HA. |
| Stop button | GPIO 14 | Momentary to GND, internal pull-up. |
| Start button | GPIO 10 | Momentary to GND, internal pull-up. |
| Status LED (optional) | GPIO 11 | LED and 330 ohm resistor to GND. |
| I2C SDA / SCL | GPIO 19 / 20 | DS3231 RTC and the optional MCP23017. |
| Avoid | 0, 4, 5, 8, 9, 15, 12, 13, 16, 17 | Battery ADC, strapping, BOOT, LED, USB and UART pins. |

The pins are the design values and are being confirmed on the bench rig. Change them in the wrapper's
substitutions if your wiring differs.

<!-- SCREENSHOT: wiring.png (Task 20 Step 2, after Task 9/19) -->

## Quick start

Follow [INSTALL_CHECKLIST.md](INSTALL_CHECKLIST.md). It takes you from the release ZIP to a working
dashboard in eight steps, each with a check. The longer guide is in [docs/install.md](docs/install.md).

## Updating

- **Firmware:** bump `version` in the wrapper (`esphome/irrigation-controller.yaml`) and click
  Install in the ESPHome Device Builder. The update goes over Wi-Fi.
- **Home Assistant packages:** copy the new `packages/irrigation/` over the old one and restart
  Home Assistant.

Read [CHANGELOG.md](CHANGELOG.md) first for anything that needs your attention.

## Offline behaviour

- If Home Assistant is unreachable, the controller keeps running its programs. Zones, programs and
  settings are stored on the controller.
- The last weather adjustment stays valid for a limited time (30 hours by default). After that the
  multiplier falls back to the program and monthly tables.
- The rain sensor, flow alarms and the maximum-runtime failsafe stay active.
- Run records are buffered on the controller (up to 48) and sent to Home Assistant when it
  reconnects. A reboot while Home Assistant is away loses the unsent ones.
- A missing Home Assistant or Wi-Fi never reboots the controller mid-watering.
- Scheduled starts are suppressed while the clock is not valid (no RTC and no time sync yet), and
  past starts are never caught up.
- The controller's own web page gives local manual control without Home Assistant.

## Licence and credits

Licensed under the [Apache License 2.0](LICENSE) (Apache-2.0). See [NOTICE](NOTICE).

Thanks to the projects this one learned from:

- [ESPyRain_Controller](https://github.com/juhurus/ESPyRain_Controller): the split between ESP and
  Home Assistant YAML packages, the README and install checklist structure, and runtime output pins.
- The [ESPHome Sprinkler component](https://esphome.io/components/sprinkler/): valve and pump sequencing.
- [Irrigation Unlimited](https://github.com/rgc99/irrigation_unlimited): ideas for scheduling and
  adjustment in an irrigation integration.
