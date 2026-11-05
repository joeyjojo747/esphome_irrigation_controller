# Smart Irrigation Controller — Solution Design

**Date:** 2026-11-01
**Requirements:** `reference_docs/smart-irrigation-requirements.md`
**Prior work:** `reference_docs/custom.yaml` (single-zone flow/cost sketch on ESP8266)
**Implementation plan:** `docs/superpowers/plans/2026-11-01-irrigation-controller.md`

---

## 1. Goals

Build a Home Assistant (HA) irrigation system on a DFRobot FireBeetle 2 ESP32-C6. It must:

1. Water on schedule **autonomously**, even when HA or Wi-Fi is down for hours or days.
2. Be **fully configurable from HA** after the first flash: zone count, relay output pins, names, programs, start times, durations, flow and rain settings. No YAML reload, HA restart or reflash.
3. Measure **flow, water use and cost**, and detect leaks and abnormal flow.
4. Use a **rain sensor** locally, plus **BOM forecasts and whichever AI integration is already installed in HA** (AI Task or conversation agent) for weather-based adjustment, analysis and programming suggestions.
5. Offer **calendar-style** viewing: completed and skipped runs in an HA Local Calendar, with the next 14 days of planned runs alongside.
6. Show an **SVG site plan** with controllers, zones and watering devices (pop-up spray, rotor, drip, bubbler).
7. Send **configurable notifications**.
8. Provide **high-quality dashboards** built with Ultra Card.
9. Allow extra sensors and buttons on the same board.

## 2. Research summary

### 2.1 Commercial best practice (Rain Bird, Hydrawise, Rachio, B-hyve)

| Area | Best practice adopted |
|---|---|
| Scheduling | Programs A–H. Each has up to 4 start times and a day mode (weekdays / odd / even / every N days), plus per-zone run times. Zones run one at a time from a FIFO queue. Master valve/pump with lead and lag. |
| Adjustment | Run time is the product of three percentages: per-program seasonal %, a **monthly fallback table** used when offline, and a daily weather % pushed from HA. Skips for rain, rain probability, freeze, wind and cumulative rain. Rain delay of N hours. |
| Cycle & soak | Set per zone (max cycle length + soak time). The ESP executes it, so it works offline (Hydrawise disables cycle & soak offline). HA suggests values from soil, slope and head type, like Rachio Smart Cycle. |
| Flow | Learned per-zone baseline with a settle time. High-flow (130%) and low-flow (70%) alarms, with optional zone shutdown. Leak alarm when there is flow with no zone open. Baseline relearn button. |
| Zone profile | Head type with precipitation rate, soil, slope, sun, plant type, area. Used for advice, AI context and map icons. |
| Notifications | Every skip with its reason and values, run start/finish, flow alarms, leak, controller offline > N min, clock invalid. |
| Reports | Scheduled vs actual per zone, litres per zone, daily, monthly and yearly totals, cost at a tariff, and a "why did/didn't it water" explanation. |
| Offline | Controller runs its last schedule locally. Results sync back on reconnect (Hydrawise reports offline runs; we replay the run log). |

### 2.2 Existing ESPHome/HA projects

- **ESPHome `sprinkler`**: good for valve sequencing, pump/master valve delays, pause/resume and the manual valve switches HA exposes. It has **no clock-based scheduler**, and its queue and pause state are lost on reboot. **Decision:** use it as the valve executor only.
- **ESPyRain_Controller** (the user's reference): schedules run on the ESP (16 stations, 8 schedules, day modes, FIFO queue, soak, pump delays, restore after restart, web server fallback). HA holds settings, the seasonal adjust, soil-moisture skip, notifications and dashboards. **Decision:** follow this split, with the same 16 zones and 8 programs.
- **Irrigation Unlimited**: rich HA-side scheduler, but nothing runs while HA is down, and edits made with `load_schedule` are lost on restart. It fails the autonomy requirement, so it is not used.
- **HA Smart Irrigation**: ET bucket model. It overlaps with the BOM + AI approach and needs an ET source BOM doesn't provide. It is not a dependency; the deterministic rule engine below covers the essentials.
- **Zroshua** add-on: confirms an SVG site map with zone shapes bound to live state is a good UX.

### 2.3 Issues in `custom.yaml` that this design fixes

1. **Secrets in plain text.** The API key and OTA password are committed. Rotate both and use `!secret`.
2. **Counter resets don't work.** `integration` sensors are "reset" with `publish_state(0)`, which doesn't reset the internal accumulator. "Daily" totals also never reset at midnight. The new design keeps monotonically increasing litre counters on the ESP and does period totals in HA from the run log.
3. **Zero tariff is silently replaced.** A tariff below 0.01 is swapped for 3.00, which hides a real 0 tariff. Cost now lives in HA options and is used exactly as entered.
4. **Not autonomous.** Single hard-coded zone, no scheduler, and ESP8266 instead of the C6.

## 3. Architecture

```
┌──────────────────────── FireBeetle 2 ESP32-C6 (ESPHome, ESP-IDF) ───────────────────────┐
│  irrigation_core (external C++ component)                                               │
│   ├─ Scheduler    start-time detection (DST-safe), day modes                            │
│   ├─ Planner      program → steps (pct, cycle & soak interleave), N-day projection       │
│   ├─ Runner       FIFO of programs, manual runs, hold/stop, failsafe timeout            │
│   ├─ FlowMonitor  baseline learning, high/low/leak; SafetyMonitor max runtime           │
│   ├─ RainWindow   tipping-bucket 24 h rolling mm                                        │
│   ├─ RunLog       48-record RAM ring (replayed to HA on reconnect)                      │
│   └─ Store        programs, zones, settings, weather adjust → NVS (ESPPreferences)      │
│  ESPHome sprinkler  16 valve slots + master/pump  ──► relay outputs (KS0011 / MCP23017)  │
│  Editor entities    Program editor, Zone editor, Settings (HA + local web UI)           │
│  Sensors            flow (pulse_counter/PCNT), rain (switch or tipping), DS3231 RTC      │
│  API actions        run_zone, run_program, stop_all, rain_delay, set_weather_adjust,     │
│                     set_program, set_zone, get_plan, get_run_log, get_config             │
│  Events → HA        esphome.irrigation_event {type, node, json}                         │
└─────────────────────────────▲──────────────────────────────┬──────────────────────────────┘
                              │ native API (actions)          │ events / entities
┌─────────────────────────────┴──────────────────────────────▼──────────────────────────────┐
│ Home Assistant (HA OS on Raspberry Pi 4) — YAML packages in packages/irrigation/           │
│   ├─ helpers       settings edited in the dashboard (thresholds, AI, targets, tariff)      │
│   ├─ sync          event router, outage replay (dedupe by ts+seq), history, backup/restore │
│   ├─ weather       BOM forecast/obs → skip + weather % → set_weather_adjust (daily 03:00)  │
│   ├─ ai            any ai_task entity or conversation agent → summary, bounded %, tips     │
│   ├─ calendar      Local Calendar history (completed/skipped) + upcoming-runs sensor       │
│   ├─ zone advisor  cycle & soak suggestions from soil/slope/head                           │
│   ├─ usage         utility meters + cost sensors (today/month/year, per zone)              │
│   └─ notify        category toggles, quiet hours, critical bypass, targets                 │
│  ESPHome Device Builder add-on builds/flashes the firmware                                 │
│  Dashboard (Ultra Card) + ha-floorplan SVG site plan + calendar card                       │
│  BOM (HACS bremor/bureau_of_meteorology) weather entity; user's existing AI integration    │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

### 3.1 Responsibility split (the core decision)

| Concern | ESP (source of truth) | HA |
|---|---|---|
| Programs, zones, settings | Stored in NVS; edited via ESP editor entities or bulk API actions | Displays and edits through ESP entities; backup/restore JSON |
| When to start | Scheduler + local clock (SNTP / HA time / DS3231) | — |
| Run-time % | program % × monthly % × weather % (weather % expires) | Computes weather % and skip once a day; pushes it with a validity window |
| Rain sensor | Local hold/skip, no HA needed | Notification and history |
| Flow alarms | Detects, stops zone (per zone setting), logs | Notification, history |
| AI | — | Analysis and suggestions; optional bounded % |
| Calendar / reports | Provides plan projection + run log | Renders calendar, usage, cost |

**Offline behaviour.** If HA is unreachable, the ESP keeps running its programs. The last weather % stays valid until its `valid_until` (default 30 h after it was pushed). After that the multiplier falls back to the program and monthly tables. The rain sensor, flow alarms and the max-runtime failsafe all stay active. Run records buffer in RAM (48) and replay when HA reconnects or restarts. `api.reboot_timeout` and `wifi.reboot_timeout` are set to `0s`, so a missing HA never reboots the controller mid-watering. The ESP web server (v3) gives local manual control and the editors without HA.

**Clock.** Time comes from HA time and SNTP, with an optional DS3231 on I²C that restores time after a power cut with no network. Scheduled starts are suppressed while the clock is invalid; manual runs still work. When the clock first becomes valid, past starts are **not** caught up.

## 4. Hardware

| Function | FireBeetle 2 C6 pin (verify on bench, Task 9) | Notes |
|---|---|---|
| Zone relays (first-boot defaults 1–6; **reassignable in HA**) | GPIO 1, 2, 3, 6, 7, 18 | One Keyestudio KS0011 per zone (SRD-05VDC-SL-C, 5 V coil, transistor driver, active-high). Relay VCC = 5 V (board VCC/USB 5 V or separate 5 V supply), common GND, signal from 3.3 V GPIO. NO contact switches the solenoid supply (24 VAC or 12/24 VDC; contacts rated 10 A). |
| Master valve / pump relay (default; reassignable in HA) | GPIO 21 | Same KS0011 type. Optional; set to `None` if not fitted. |
| Flow sensor | GPIO 23 (PCNT) | Any hall pulse meter; pulses/L configurable (default 450). For a 5 V sensor, use a 10 k/20 k divider on the signal. |
| Rain sensor | GPIO 22 | Type chosen in HA: None / Switch NC / Switch NO / Tipping bucket (mm per tip). |
| Stop button | GPIO 14 | Momentary to GND, internal pull-up. Short press: Stop all; long press: Rain delay 24 h on/off (both choosable in HA, §4.2). |
| Start button | GPIO 10 | Momentary to GND, internal pull-up. Short press: Run program (A by default); long press: Next zone (choosable in HA, §4.2). Confirm GPIO 10 is broken out (Task 9). |
| Status LED (optional) | GPIO 11 | LED + 330 Ω to GND. Confirm GPIO 11 is broken out (Task 9). |
| I²C SDA / SCL | GPIO 19 / 20 | DS3231 RTC (recommended); optional MCP23017 relay expander. |
| Avoid | 0 (battery ADC), 4, 5, 8, 9, 15 (strapping/BOOT/LED), 12, 13 (USB), 16, 17 (UART0) | |

### 4.1 Runtime output mapping (like ESPyRain, but validated)

ESPyRain stores a raw pin number per station and drives it with ESP-IDF `gpio_set_level`, active-low, with no checks. This design keeps that "set pins in HA" ability and adds safety:

- **Fixed slots.** The firmware always has 17 output slots (zones 1–16 and the master). They are `irrigation_core` output switches, which the `sprinkler` component uses as valve and pump switches.
- **Assignable outputs.** Each slot is set in HA to one of:
  - `None`
  - a board GPIO from the compile-time candidate list (default 1, 2, 3, 6, 7, 18, 21)
  - `MCP A0`–`MCP B7`, available only if the optional MCP23017 package is included
- **Validation.** An assignment is rejected, and nothing changes, if:
  - the pin is reserved (sensor or I²C pins, strapping, USB, UART or LED)
  - it isn't a candidate
  - it's already used by another slot
  - it's an MCP channel and no expander is fitted
  - The reason is shown in `Last Error`, and the HA select snaps back.
- **Applying a change.** Watering stops first. The old pin is *parked* as an input pulled to the relay's off level. This matters because `gpio_reset_pin` enables a pull-up, which would switch an active-high KS0011 on. The new pin is then configured at the off level, and the result is saved to NVS.
- **Relay trigger level** (Active high, the default for the KS0011, or Active low) is set in HA. Changing it re-drives every output.
- **Boot.** Outputs are driven off at `setup_priority::HARDWARE`, from NVS, before the scheduler or the sprinkler start.
- **Unassigned zones.** A zone set to `None` is skipped by programs, and any other start of it is refused, so the master valve or pump never runs without a zone valve: `run_zone` / Learn Flow reject it, and the HA `Zone N` switch is vetoed at the output (the master is held off and watering stops, with a `no_output` alert). Either way `Last Error` shows "Zone N has no output". The **Zone Count** (1–16) limits the zones shown and used.
- **Compile-time pins.** Sensor pins (flow, which needs PCNT, plus rain, the Start/Stop buttons, the status LED and I²C) stay compile-time substitutions, because the ESPHome sensor components bind pins at build time.
- **More than 6 zones.** Fitting an MCP23017 and including `expander.yaml` needs a reflash, since the hardware itself changes. After that, all 16 channels can be assigned from HA.

### 4.2 Physical Start/Stop buttons and status LED (work with HA offline)

- **Handled on the ESP.** Each press goes straight to `irrigation_core`. HA only stores which function each press has, and the ESP keeps that in NVS, so the buttons work with HA or Wi-Fi down.
- **Presses.** ESPHome `on_multi_click`, 50 ms debounce: a short press is released within 1 s; a long press is held for 2 s and fires while still held.
- **Functions**, set in HA per button and per press type:
  - `None`
  - `Run program`: runs the program chosen in `Button Program` (default A), the same as the `run_program` action. Ignored while watering, so repeated presses don't queue runs.
  - `Stop all`
  - `Next zone`: ends the current step as `stopped` and continues with the program's next step. Ignored when idle.
  - `Rain delay 24 h`: sets a 24 h rain delay, or clears it if one is active.
- **Defaults.**

  | Button | Short press | Long press |
  |---|---|---|
  | Start | Run program | Next zone |
  | Stop | Stop all | Rain delay 24 h |

- **Stop is always honoured.** While anything is watering or queued, every Stop press, of any length, runs `Stop all` as soon as it is pressed (`on_press`), whatever the Stop functions are set to. The short and long functions above apply only when idle, so a long press sets or clears the rain delay only when nothing is watering. `Stop all` works even when the controller is disabled or the clock is not set. `Run program` from a button respects `Controller Enabled` but, like a manual run from HA, ignores rain and weather skips.
- **Logging.** Button runs are logged like any other run (program runs as their program, manual steps as manual), so they reach the calendar and run history when HA reconnects. Every press also fires `button` events (`{"btn":"start|stop","press":"short|long|down","fn":N,"cmd":N}`, where `down` is a Stop press acted on at press while watering and `cmd` 0 means the press was ignored) for the HA log.
- **Status LED** (optional package `status_led.yaml`):
  - off when idle
  - on while watering
  - slow blink (1 s on, 1 s off) during a rain delay or rain hold
  - fast blink (0.2 s on/off) while the clock is not set
  - a 150 ms flash to acknowledge each recognised press
- **Wiring.** Buttons connect between the GPIO and GND. For an outdoor box use IP65 or better momentary buttons. If the cable is longer than about 2 m, add 100 nF from each button pin to GND.
- **If GPIO 10/11 are not broken out** on the FireBeetle 2 C6, change the `start_pin`/`led_pin` substitutions to another free pin, or take one from the relay candidate list (for example GPIO 18) and use the MCP23017 for the relays.

## 5. Data model (ESP)

```
Program[8]  enabled, name(text), day_mode{WEEKDAYS,ODD,EVEN,INTERVAL}, weekday_mask(bit0=Mon),
            interval_days(1–30), interval_anchor_day(days since 1970-01-01, local),
            start_minutes[4] (-1 unused), zone_seconds[16] (0 = not in program),
            seasonal_pct(0–200), cycle_soak(bool), weather_adjust(bool)
Zone[16]    enabled, name(text), head{SPRAY,ROTOR,ROTATOR,DRIP,BUBBLER}, precip_mm_h,
            soil{SAND,LOAMY_SAND,SANDY_LOAM,LOAM,CLAY_LOAM,CLAY}, slope_pct, sun{FULL,PART,SHADE},
            plant{LAWN,SHRUBS,TREES,GARDEN_BEDS,NATIVES,VEG}, area_m2,
            max_cycle_s (0 = no cycle&soak), soak_s, flow_baseline_lpm, flow_high_pct(130),
            flow_low_pct(70), flow_action{ALERT, ALERT_AND_STOP},
            output (-1 none | 0–30 GPIO | 100–115 MCP A0–B7)
Settings    master_output, relay_active_high(true), zone_count, controller_enabled, monthly_pct[12] (default 100), pulses_per_litre(450),
            leak_lpm(0.5), flow_settle_s(60), rain_type, mm_per_tip(0.2), rain_skip_mm(3.0),
            rain_sensor_stops_running(true), zone_hard_cap_s(7200),
            start_short_fn(RUN_PROGRAM), start_long_fn(NEXT_ZONE), stop_short_fn(STOP_ALL),
            stop_long_fn(RAIN_DELAY), button_program(0 = A)
            (ButtonFn{NONE, RUN_PROGRAM, STOP_ALL, NEXT_ZONE, RAIN_DELAY})
Volatile+NVS weather_adjust{pct, skip, reason[32], valid_until_ts}, rain_delay_until_ts
RAM only    RunLog[48] {seq, instance, program(255=manual), zone(255=program-level), start_ts,
            planned_s, actual_s, litres, result}
```

`result` ∈ `completed, stopped, flow_high, flow_low, max_runtime, skipped_controller_off, skipped_rain_delay, skipped_rain_sensor, skipped_weather`.

### 5.1 Key algorithms

- **Effective %** = `program.seasonal_pct × monthly_pct[month] × weather_pct / 10000`, clamped to 0–250. `weather_pct` is 100 when there is no valid push or `program.weather_adjust = false`.
- **Start decision**, checked in order: controller disabled → rain delay active → rain hold (switch active or tipping mm ≥ `rain_skip_mm`) → weather skip (valid, and the program opts in) → effective % = 0 → run.
- **Scheduler tick**, once per second, evaluated per local minute:
  - Scans every local minute index since the last evaluation, capped at 180 minutes. This lets a 02:30 start fire at 03:00 when DST springs forward.
  - Keeps `last_fired_day` per program and start slot, so a start fires once even if 02:30 occurs twice when DST falls back.
  - Resets on clock invalid; no catch-up after the reset.
- **Steps:**
  - For each zone in the program with `zone_seconds > 0`, enabled and `< zone_count`, total = round(seconds × pct / 100). Zones whose total is under 60 s are dropped. Totals are capped at `zone_hard_cap_s`.
  - With cycle & soak on and total > `max_cycle_s`: n = ceil(total / max_cycle_s), split evenly, with the remainder going to the earliest cycles.
  - Steps are emitted round-robin: cycle 1 of every zone, then cycle 2, and so on. From cycle 2 onward a step carries `soak_s`, and the runner waits idle (valves off) until that much time has passed since the zone's previous cycle ended.
- **Day modes:**
  - ODD = odd day-of-month except the 31st.
  - EVEN = even day-of-month.
  - INTERVAL = `(day − anchor) mod interval == 0` and day ≥ anchor. Saving an interval program sets anchor = today + "days until first run".
- **Flow:**
  - Checks start after `flow_settle_s`.
  - High alarm if flow > baseline × high% for 30 s continuously; low alarm if flow < baseline × low% for 30 s. Each fires once per step, and only when baseline > 0.
  - Leak alarm: no zone open, at least 60 s since the last valve closed (drain-down), and flow > `leak_lpm` for 60 s. It fires once until flow drops below the threshold.
  - Baseline learning: from steps with ≥ 120 s of post-settle time and no alarm. The first good run sets the baseline; after that it's an EMA, `0.7 × old + 0.3 × new`.
- **Manual run** = `stop_all` (log the remaining work as stopped) followed by a single step. Manual runs bypass weather, rain delay and rain hold, but not flow alarms or the hard cap.
- **Program queue:** FIFO of at most 8 programs. A program that is already running or queued is not queued again; the attempt is logged.

## 6. HA side: YAML packages (no custom integration)

This follows ESPyRain: the HA side is a set of YAML package files in `packages/irrigation/`. HA loads them with one `configuration.yaml` line, `homeassistant: packages: !include_dir_named packages/irrigation`, added once at install with one restart. After that, every setting is a helper **value** edited in the dashboard, so no restart or reload is needed.

**Runtime requirements:**
- HA Core 2026.10.4 on HA OS (Raspberry Pi 4).
- ESPHome Device Builder add-on, to build the firmware.
- Local Calendar integration.
- HACS for BOM, Ultra Card and ha-floorplan.
- No Python code, custom integration or extra add-on.

| Package file | Contents |
|---|---|
| `irrigation_helpers.yaml` | Settings helpers: weather entity, rain-observation entity, AI mode and entity, notify targets, rule thresholds, tariff, quiet hours, evaluation time, notification toggles, sync bookkeeping |
| `irrigation_sync.yaml` | Event router (`esphome.irrigation_event` → `irrigation_<type>` events), outage replay from `get_run_log` (dedupe by `(ts, seq)` so device reboots are safe), 7-day run history sensor, config backup/restore via `get_config` |
| `irrigation_calendar.yaml` | Writes completed and skipped runs to the user's **Local Calendar** `calendar.irrigation`; `sensor.irrigation_upcoming` holds the next 14 days from `get_plan` |
| `irrigation_weather.yaml` | `script.irrigation_evaluate_weather` (rules below), daily automation at the evaluation time, `sensor.irrigation_weather_decision` |
| `irrigation_ai.yaml` | `script.irrigation_ai_advice`, `sensor.irrigation_advice` |
| `irrigation_zone_advisor.yaml` | `script.irrigation_suggest_zone`: cycle & soak from soil, slope and precipitation rate |
| `irrigation_usage.yaml` | `utility_meter` day/month/year on the device's `total_increasing` water counters (total and per zone); cost template sensors from the tariff helper |
| `irrigation_notify.yaml` | `script.irrigation_notify` (category toggle, quiet hours, critical bypass, multiple targets) and the event → notification automation |

- **Weather rules** (defaults, metric). Every threshold is a helper.
  - Skip if observed 24 h rain + today's forecast ≥ 3 mm.
  - Skip if rain probability ≥ 80 % and forecast ≥ 2 mm.
  - Skip if forecast minimum ≤ 2 °C.
  - Skip if forecast wind ≥ 30 km/h; this rule is **off** by default.
  - Skip if 3-day observed rain ≥ 15 mm, taken from a daily snapshot of the observation sensor.
  - Temperature factor: forecast max < 18 °C → 70 %; > 32 °C → 130 %; otherwise 100 %.
  - The result is pushed with `valid_hours = 30`. If the weather entity is unavailable, **nothing is pushed** and the ESP falls back by itself.
- **AI: use whatever AI the user already has.** `input_select.irrigation_ai_mode` chooses one of:
  - **AI Task:** `ai_task.generate_data` against any `ai_task.*` entity (OpenAI, Google Gemini, Anthropic, Ollama, …), with a structured result.
  - **Conversation agent:** `conversation.process` against any agent id, including add-on or local agents without AI Task support. The reply must be JSON and is parsed defensively.
  - **Off.**
  - The AI gets the zones, the 7-day run history, the 5-day forecast and the rule decision. It returns `{summary, recommended_pct, program_suggestions[], warnings[]}`.
  - Output that fails validation is discarded.
  - With auto-apply on and no rule skip, the final % = `clamp(ai_pct, rule_pct − 30, min(rule_pct + 30, ai_max))`. The AI can never cancel a rule skip.
- **Calendar.** HA has no service to delete events, so the Local Calendar holds only what **happened**: completed program runs ("Program A · 3 zones · 42 min · 610 L", with ⚠ on alarms) and skips ("Program A skipped — rain sensor"). Planned runs are shown from `sensor.irrigation_upcoming` next to the calendar card, so they never go stale.
- **Usage:** litres and cost for today, month and year, plus per-zone month, from utility meters on the device counters. The ESP's total litres (`total_increasing`, `device_class: water`) also feeds the HA Energy dashboard.
- **Testing:** GitHub Actions loads the packages into a test HA instance (pytest-homeassistant-custom-component), mocks the device actions, weather, AI and notify services, and checks behaviour and exact message copy. `check_config` validates the packages.

## 7. UI

- **Dashboard** (`dashboards/irrigation-dashboard.yaml`). Views:
  - Overview: status, next run, active zone with countdown, flow, today's water and cost, weather + AI summary, quick actions
  - Site plan
  - Calendar
  - Programs: the program editor
  - Zones: the zone editor, with cycle & soak suggestions
  - Water & cost: statistics graphs
  - Settings: monthly %, flow, rain, notifications link
  - Layout cards are Ultra Card (HACS); the calendar card and statistics-graph are native.
- **Site plan** (`www/irrigation/siteplan.svg` + ha-floorplan):
  - Trace a Google satellite screenshot in Inkscape.
  - Element ids: `controller-1`, `zone-NN` (zone polygons, 01–16) and `head-NN-MM` (heads/emitters).
  - Head classes: `head-spray`, `head-rotor`, `head-rotator`, `head-drip`, `head-bubbler`.
  - Floorplan rules colour zones by state (`zone-running`, `zone-disabled`, `zone-alarm`, `zone-idle`). Tapping a zone opens a run dialog.
  - Placing devices on the plan means editing the SVG; nothing on the ESP changes.

## 8. Notification copy

| Category | Default | Title | Message |
|---|---|---|---|
| run_started | off | `Irrigation started` | `{program} started: {zones} zones, about {minutes} min ({pct}%).` |
| run_finished | on | `Irrigation finished` | `{program} finished: {minutes} min, {litres} L, ${cost}.` |
| skipped | on | `Irrigation skipped` | `{program} skipped: {reason}.` |
| flow_high | on (critical) | `High flow — {zone}` | `{zone} flow {flow} L/min is above {limit} L/min. {action}.` |
| flow_low | on | `Low flow — {zone}` | `{zone} flow {flow} L/min is below {limit} L/min. Check the valve, filter or supply.` |
| leak | on (critical) | `Possible leak` | `{flow} L/min flowing with no zone running.` |
| max_runtime | on (critical) | `Zone stopped — runtime limit` | `{zone} ran past its {limit} min limit and was stopped.` |
| offline | on | `Irrigation controller offline` | `No contact for {minutes} min. Schedules keep running on the controller.` |
| clock_invalid | on | `Irrigation clock not set` | `Scheduled watering is paused until the controller clock is set.` |
| daily_advice | on | `Irrigation outlook` | `{summary}` |

- `{action}` is `Zone stopped` or `Alert only`.
- Critical categories ignore quiet hours.

## 9. Repository and distribution

The project is published as a public GitHub repo, **`joeyjojo747/esphome_irrigation_controller`** (Apache-2.0), modelled on ESPyRain_Controller.

**Layout.** The repo root mirrors HA's `/config` folder, so a release ZIP installs by copying folders:
  - `esphome/irrigation-controller.yaml` is the user wrapper (board, secrets, timezone, sensor pins, optional expander include; relay outputs are set in HA). It includes `esphome/irrigation-controller/package.yaml`, which pulls in the packages and the `irrigation_core` external component.
  - `packages/irrigation/` holds the HA YAML packages (copied to `/config/packages/irrigation/`).
  - `www/irrigation/` holds the site plan.
  - `dashboards/` and `floorplan/` hold the dashboard and the floorplan rules.

**Ways to install:**
  - **ZIP:** copy the folders following `INSTALL_CHECKLIST.md`.
  - **ESPHome Device Builder:** paste the wrapper into a new device; it builds the firmware on the Pi and flashes it (USB first, then wirelessly).
  - **ESPHome remote packages:** `github://joeyjojo747/esphome_irrigation_controller/...@vX.Y.Z`. Updating the firmware then means bumping one version line.

**Versioning:** one SemVer number, in `VERSION`, matching the ESPHome `project.version` and the `irrigation_package_version` helper. Tags are `vX.Y.Z`.
  - CI runs on every push: host C++ tests, HA tests, ESPHome config + compile with and without the expander, and HA `check_config` on the packages. Tests run only in GitHub Actions; nothing is installed locally.
  - The release workflow attaches the install ZIP.

**Docs:**
  - `README.md` in ESPyRain's structure: goals, comparison, features with screenshots, hardware and wiring, quick start, offline behaviour, credits.
  - `INSTALL_CHECKLIST.md`, with a "Check:" line after each step.
  - `CHANGELOG.md`.
  - Issue templates.

**Secret hygiene:** `reference_docs/custom.yaml` contains live credentials. It is git-ignored and never published, and a test scans every tracked file for its key and password.

## 10. Out of scope (v1)

- In-HA drag-and-drop placement on the site plan. The SVG is edited in Inkscape.
- Editing schedules from the calendar, which stays read-only.
- Multiple controllers. The packages target the one device named `irrigation-controller`.
- Thread/Zigbee; Wi-Fi only.
- Soil-moisture sensors. The I²C bus (or a spare MCP23017 channel) allows adding one later with a reflash.
