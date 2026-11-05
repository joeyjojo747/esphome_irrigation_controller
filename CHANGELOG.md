# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [0.1.0] - Unreleased

First release.

### Added

- ESPHome firmware for the DFRobot FireBeetle 2 ESP32-C6 (ESP-IDF): 16 zone slots, 8 programs (A to H)
  with 4 start times each, a queue of 8 programs, cycle and soak, a master valve or pump, and a
  48-record run log. Schedules run on the controller, so they keep running without Home Assistant or Wi-Fi.
- Zone count, names, programs, settings and the output that drives each zone and the master valve
  (a board GPIO or an MCP23017 channel, plus relay trigger level) are all set from Home Assistant, with
  validation. No YAML reload, Home Assistant restart or reflash is needed after install.
- Physical Start and Stop buttons and an optional status LED, handled on the controller.
- Flow metering with litres per run and per zone, learned baselines, and high flow, low flow, leak and
  maximum-runtime protection. Water use and cost with utility meters and a tariff setting.
- Rain sensor (switch or tipping bucket), rain delay, and weather skips for rain, rain probability,
  freeze, wind and cumulative rain.
- Daily weather adjustment from the BOM weather entity, with an optional AI advisor that works with any
  AI Task entity or conversation agent already set up in Home Assistant. Rule-based adjustment works with AI off.
- Per-zone cycle and soak suggestion.
- Local Calendar of finished and skipped runs, an upcoming-runs sensor and a run history.
- Notifications to your chosen notify targets, with quiet hours.
- Dashboard with overview, programs, zones, calendar, water use and settings, plus a site plan
  (SVG shown with ha-floorplan).
- Config backup and restore through Home Assistant, so a replaced device can be rebuilt.
- Release ZIP that installs by copying folders, an install checklist and a detailed install guide.

### Known limitations

- The Ultra Card dashboard views are not finished: the overview, programs and zones views use native
  Home Assistant cards, marked for replacement with Ultra Card visual-editor exports on your own Home Assistant.
- Hardware bench testing has not been done: pin assignments and behaviour on the real board are unverified.
- After a crash (not a clean reboot) the controller can double-count up to 5 minutes of water in the totals.
- The `no_output` alert (a zone started with no output assigned) is shown in Last Error but does not send a notification.
- A zone set to `None` in the middle of a run stops the running step (fail-safe).
- Run records are buffered in RAM (48), so a reboot while Home Assistant is away loses the unsent ones.
- The site plan card wrapper and its tap confirmation have not been tested on a real Home Assistant.
- Remote packages (Option B in the wrapper) have not been tested against a published tag.
