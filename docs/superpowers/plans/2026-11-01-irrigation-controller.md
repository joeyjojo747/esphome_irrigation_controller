# ESPHome Smart Irrigation Controller Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an autonomous 1–16 zone irrigation controller: ESPHome firmware for a FireBeetle 2 ESP32-C6, built and flashed with ESPHome Device Builder and fully configured from Home Assistant. HA YAML packages add BOM and AI weather adjustment, a calendar, water usage and cost, and notifications. An Ultra Card dashboard and an SVG site plan complete the UI.

**Architecture:**
- An external ESPHome C++ component (`irrigation_core`) owns programs, zones, settings and the relay output map in NVS. It schedules and sequences watering on the device and uses the stock ESPHome `sprinkler` component to switch the valves.
- All decision logic is pure C++ in `logic/`, unit-tested in CI.
- The HA side is ESPyRain-style YAML packages: helpers, scripts, automations and trigger-template sensors. They compute a daily weather % and skip, ask whichever AI integration the user already has, and push the result to the device. They also turn the device's run log and plan into calendar entries, an upcoming-runs sensor, usage and cost sensors, and notifications.
- There is no Python custom integration and no add-on.

**Tech Stack:**
- **Firmware:** ESPHome (ESP-IDF framework), C++17, built in the **ESPHome Device Builder** add-on on the user's HA (HA OS, Core 2026.10.4, Raspberry Pi 4).
- **HA:** Home Assistant YAML packages and Jinja templates. AI runs through `ai_task.generate_data` (any AI Task entity) or `conversation.process` (any conversation agent).
- **Tests:** GitHub Actions only. Doctest for C++, pytest with pytest-homeassistant-custom-component to load and exercise the YAML packages, and ESPHome config and compile checks. Nothing is installed on the user's PC.
- **HACS (frontend and weather only):** bremor/bureau_of_meteorology, Ultra Card, ha-floorplan.
- **GitHub:** public repo `joeyjojo747/esphome_irrigation_controller`, tagged releases with an install ZIP, and ESPHome remote packages.

**Spec:** `docs/design/2026-11-01-irrigation-solution-design.md` (sections are cited as §N). Original requirements: `reference_docs/smart-irrigation-requirements.md`.

## Global Constraints

- **Device:** DFRobot FireBeetle 2 ESP32-C6, `esp32: variant: esp32c6`, `framework: type: esp-idf`. Wi-Fi only, no Thread.
- **ESPHome:** ≥ 2025.10. It must support `api.respond` action responses; Task 0 verifies this.
  - Pin the exact version in `tools/requirements-ci.txt`, matching the user's Device Builder add-on version or lower.
  - The firmware must build unmodified in Device Builder on a Raspberry Pi 4.
- **Home Assistant:** the user runs Core 2026.10.4; the minimum is 2025.8, for `ai_task`. Pin `pytest-homeassistant-custom-component` to the release matching HA 2026.10.
- **HA side is YAML only:**
  - Every HA feature is a file in `packages/irrigation/`, loaded with `homeassistant: packages: !include_dir_named packages/irrigation`. That needs a single restart at install.
  - Settings are helper values edited in the dashboard, so no restart or YAML reload is needed later.
  - There is no custom integration, no add-on and no Python running in HA.
- **AI is provider-agnostic.** The user picks `AI Task` (any `ai_task.*` entity, e.g. OpenAI, Google Gemini, Anthropic, Ollama) or `Conversation agent` (any agent id) in helpers. `Off` disables AI, and rule-based weather adjustment still works.
- **Capacity:** 16 zone slots, 8 programs (A–H), 4 start times per program, FIFO of 8 queued programs, run log of 48 records.
- **No reflash after install:** zone count, names, programs and every §5 setting must be editable from HA.
  - That includes **which output drives each zone and the master valve/pump** (a board GPIO or an MCP23017 channel), plus the relay trigger level. This is ESPyRain's runtime station pins, with validation added.
  - Compile-time substitutions are limited to the sensor pins (flow, rain, Start/Stop buttons, status LED, I²C), whether an expander is fitted, the board's candidate output GPIOs and the timezone.
- **Autonomy settings:**
  - `api: reboot_timeout: 0s` and `wifi: reboot_timeout: 0s`.
  - Every relay `restore_mode: ALWAYS_OFF`.
  - Scheduled starts are suppressed while the clock is invalid.
  - No catch-up of past starts.
- **Secrets:** all secrets come from Device Builder's `secrets.yaml`, which is git-ignored. Only `esphome/secrets.yaml.example` is committed. Never copy the key or password from `reference_docs/custom.yaml`.
- **Units:** metric (L, L/min, mm, °C, km/h, m²); currency `$`, default tariff 3.00 $/kL.
- **Timezone:** substitution `timezone: Australia/Sydney`; the user changes it at install.
- **Entity naming:** device `name: irrigation-controller`, `friendly_name: Irrigation`.
  - Entity ids are therefore `<domain>.irrigation_<entity_name_slug>`, and ESPHome actions are `esphome.irrigation_controller_<action>`.
  - The packages rely on these names, so the wrapper header says not to change them.
- **Events:** the device fires exactly one HA event type, `esphome.irrigation_event`, with data `{node, type, json}`.
- **Running checks (no local tooling):** a step written `CI: <job>` means the following.
  - Commit to branch `dev` and `git push`.
  - Run `gh run watch --exit-status $(gh run list --branch dev --limit 1 --json databaseId -q '.[0].databaseId')`.
  - Read the named job's log with `gh run view --log` (or `--log-failed`). Its result must match "Expected".
  - Jobs: `host-tests`, `ha-tests`, `esphome` (Task 0). When a task's checks are green, merge `dev` into `main`.
- **GitHub repository**, modelled on https://github.com/juhurus/ESPyRain_Controller:
  - Public repo `https://github.com/joeyjojo747/esphome_irrigation_controller`, licence Apache-2.0, default branch `main`.
  - The repo root mirrors the HA `/config` folder, so the release ZIP installs by copying folders: `esphome/` → `/config/esphome/`, `packages/irrigation/` → `/config/packages/irrigation/`, `www/` → `/config/www/`.
  - One version number, in `VERSION` (SemVer, starting at `0.1.0`), must match the ESPHome `project.version` and the `input_text.irrigation_package_version` initial value. Releases are tagged `v<VERSION>`.
- **Never commit `reference_docs/custom.yaml`**: it contains a live API key and OTA password. It is git-ignored from Task 0. `reference_docs/smart-irrigation-requirements.md` is committed.

## Review Focus

1. **HA or Wi-Fi down for days.** Programs keep running at program % × monthly %. The weather % stops applying after `valid_until`, and the device never reboots for lack of HA. Runs made while HA was down appear in the calendar after reconnect, even if the device rebooted meanwhile. Tests: Task 2 `weather adjust expires`, Task 7 `test_autonomy_settings`, Task 11 `test_replay_after_outage`.
2. **Power cut, then boot with no network.** Valves stay off. No scheduled start fires while the clock is invalid, and when the clock becomes valid at 07:10 the 06:00 start is not run late. Test: Task 1 `no catch-up after clock becomes valid`.
3. **DST in Australia** (first Sunday of October 02:00→03:00, first Sunday of April 03:00→02:00). A 02:30 start fires exactly once on both days. Tests: Task 1 `spring forward` and `fall back`.
4. **Changing a relay output from HA.** The user may pick a strapping, USB or sensor pin, give two zones the same pin, pick an MCP channel with no expander fitted, or change a pin or the trigger level while a zone is watering. The bad assignments must be rejected with a readable reason and leave nothing changed. A change during watering stops watering first. A released pin must not energise its relay, because ESP-IDF `gpio_reset_pin` enables a pull-up, which turns an active-high KS0011 **on**. Tests: Task 5A `rejects reserved/duplicate/no-expander`, `released pin parks at off level`; Task 9 bench items 10–13; Task 11 `test_restore_swapped_outputs`.
5. **AI or weather provider misbehaves.** The weather entity is unavailable, or the AI returns prose instead of JSON, an out-of-range %, or an error. Nothing harmful is pushed: no push at all without weather, a rule-only decision without valid AI output, and AI never cancels a rule skip. Tests: Task 13 `test_unavailable_weather_pushes_nothing`, Task 14 `test_conversation_agent_non_json_gives_null`, `test_never_overrides_skip`, `test_provider_error_null`.
6. **Physical buttons with HA offline.** Start and Stop must work with HA and Wi-Fi down, using the functions last set in HA (stored in NVS). Stop all must work even when the controller is disabled or the clock is not set, and repeated Start presses must not queue extra runs. Tests: Task 5B `stop always allowed`, `start ignored while busy`; Task 9 bench items 14–16.

---

## File Structure

```
README.md                                   ESPyRain-style: goals, features, screenshots, hardware, quick start, links
INSTALL_CHECKLIST.md                        step-by-step install: Device Builder + copy packages into /config
LICENSE                                     Apache-2.0
VERSION                                     single source of the release version
CHANGELOG.md                                Keep a Changelog format
.github/workflows/ci.yml                    jobs host-tests, ha-tests (pytest + check_config), esphome (config+compile, with and without expander)
.github/workflows/release.yml               on tag v*: verify versions, attach ZIP of install folders to the GitHub release
.github/ISSUE_TEMPLATE/bug_report.yml, feature_request.yml
images/screenshots/                         dashboard and wiring images used by README
.gitignore                                  secrets, build dirs, .esphome/, reference_docs/custom.yaml
tools/ci/host-tests.sh, ha-tests.sh, esphome.sh   what each CI job runs
tools/requirements-ci.txt                   pinned esphome, pytest-homeassistant-custom-component, pyyaml
tools/build_release_zip.py                  release ZIP builder (runs in CI)
esphome/irrigation-controller.yaml          user wrapper pasted into Device Builder (like espyrain_controller_wrapper.yaml)
esphome/secrets.yaml.example
esphome/irrigation-controller/package.yaml  lists every package file below; the one file the wrapper includes
esphome/irrigation-controller/VERSION       copy of /VERSION (checked by test_release.py)
esphome/irrigation-controller/base.yaml                  esp32, wifi, api, ota, logger, web_server, time, i2c, rtc, preferences
esphome/irrigation-controller/outputs.yaml               zone_out_1..16 + master_out as irrigation_core output switches (runtime-mapped)
esphome/irrigation-controller/expander.yaml              optional MCP23017 @0x20; uncomment its include in package.yaml (merges expander_id into irrigation_core)
esphome/irrigation-controller/sprinkler.yaml             sprinkler controller with 16 valves
esphome/irrigation-controller/core.yaml                  irrigation_core instance, status sensors, event forwarding
esphome/irrigation-controller/sensors.yaml               flow + rain + Start/Stop buttons
esphome/irrigation-controller/status_led.yaml            optional status LED; uncomment its include in package.yaml
esphome/irrigation-controller/editor_program.yaml        program editor entities
esphome/irrigation-controller/editor_zone.yaml           zone editor entities
esphome/irrigation-controller/settings.yaml              settings entities (monthly %, flow, rain, zone count, outputs, ...)
esphome/irrigation-controller/api_actions.yaml           API actions
esphome/irrigation-controller/components/irrigation_core/__init__.py   codegen + schema + triggers
esphome/irrigation-controller/components/irrigation_core/irrigation_core.h / .cpp   ESPHome glue (Component)
esphome/irrigation-controller/components/irrigation_core/logic/types.h         enums + PODs (§5)
esphome/irrigation-controller/components/irrigation_core/logic/civil.h/.cpp    date math
esphome/irrigation-controller/components/irrigation_core/logic/schedule.h/.cpp is_due, Scheduler, decide_start, effective_pct
esphome/irrigation-controller/components/irrigation_core/logic/planner.h/.cpp  expand_program, project
esphome/irrigation-controller/components/irrigation_core/logic/runner.h/.cpp   Runner + ValveDriver interface
esphome/irrigation-controller/components/irrigation_core/logic/flow.h/.cpp     FlowMonitor, SafetyMonitor, RainWindow
esphome/irrigation-controller/components/irrigation_core/logic/runlog.h/.cpp   RunLog ring + JSON
esphome/irrigation-controller/components/irrigation_core/logic/pinmap.h/.cpp   output codes, labels, assignment validation, park level
esphome/irrigation-controller/components/irrigation_core/logic/buttons.h/.cpp  button press -> command, status LED pattern
esphome/irrigation-controller/components/irrigation_core/switch/__init__.py     `switch: platform: irrigation_core` output switch (slot 0-15 zones, 16 master)
packages/irrigation/irrigation_helpers.yaml       settings helpers (thresholds, AI, notify targets, tariff, quiet hours)
packages/irrigation/irrigation_sync.yaml          event router, outage replay, run history, backup/restore
packages/irrigation/irrigation_calendar.yaml      calendar writer, upcoming-runs sensor
packages/irrigation/irrigation_weather.yaml       weather rules script, daily adjust automation, decision sensor
packages/irrigation/irrigation_ai.yaml            AI advice script (AI Task / conversation agent), advice sensor
packages/irrigation/irrigation_zone_advisor.yaml  cycle & soak suggestion script
packages/irrigation/irrigation_usage.yaml         utility meters + cost sensors
packages/irrigation/irrigation_notify.yaml        notify script + notification automation
tests/host/CMakeLists.txt, doctest.h, test_*.cpp
tests/compile/*.yaml                        ESPHome fixtures (*_bad_*.yaml must fail config)
tests/ha/                                   pytest: package behaviour, firmware YAML static checks, siteplan, dashboard, release
dashboards/irrigation-dashboard.yaml
www/irrigation/siteplan.svg, siteplan.css
floorplan/irrigation-floorplan.yaml
docs/install.md
```

---

### Task 0: GitHub repository, CI and toolchain smoke tests

**Files:**
- Create: `.gitignore`, `LICENSE` (Apache-2.0, copyright `2026 joeyjojo747`), `VERSION` (`0.1.0`), `README.md` (stub: title, one-paragraph goal, "work in progress")
- Create: `.github/workflows/ci.yml`, `tools/ci/host-tests.sh`, `tools/ci/ha-tests.sh`, `tools/ci/esphome.sh`, `tools/requirements-ci.txt`
- Create: `tests/host/CMakeLists.txt`, `tests/host/doctest.h` (doctest 2.4.11 single header), `tests/host/test_smoke.cpp`
- Create: `tests/compile/respond_smoke.yaml`, `tests/ha/test_smoke.py`, `pytest.ini`

**Interfaces:**
- Produces three CI jobs. Later steps refer to them as `CI: <job>`.
  - **`host-tests`** (ubuntu-latest, `apt-get install g++ cmake`) runs `tools/ci/host-tests.sh`: configure and build `build/host`, then `ctest --output-on-failure`.
    - The CMake target globs `tests/host/test_*.cpp` and every `logic/*.cpp` into one executable, `host_tests`.
  - **`ha-tests`** (`actions/setup-python` 3.13, `pip install -r tools/requirements-ci.txt`) runs `tools/ci/ha-tests.sh`, which is `pytest` with `pytest.ini` setting `testpaths = tests/ha`. Task 10 adds `check_config`.
  - **`esphome`** (setup-python 3.13, `pip install esphome==<pin>`, cache `~/.platformio` and `.esphome`) runs `tools/ci/esphome.sh`:
    - `esphome config` and `esphome compile` on every `tests/compile/*.yaml` except `*_bad_*.yaml`.
    - Each `*_bad_*.yaml` must **fail** `esphome config`, and its expected message is grepped from a `# expect:` comment in that file.
    - From Task 7 on, the script also builds the wrapper with and without the expander.
- `tools/requirements-ci.txt` pins three things:
  - `esphome==<latest stable>`
  - `pytest-homeassistant-custom-component==<release matching HA 2026.10>`, which also installs the matching `homeassistant` and is used to test the YAML packages; no custom component is involved
  - `pyyaml`

- [ ] **Step 1:** Run `git init -b main`, then write `.gitignore`. It must ignore `esphome/secrets.yaml`, `secrets.yaml`, `.esphome/`, `build/`, `__pycache__/`, `.pytest_cache/` and `reference_docs/custom.yaml`. Run `git status --short`. Expected: `reference_docs/custom.yaml` is not listed.
- [ ] **Step 2:** Write the smoke tests:
  - `test_smoke.cpp`: `TEST_CASE("smoke") { CHECK(1 + 1 == 2); }`
  - `test_smoke.py`: `def test_smoke(): assert True`
  - `respond_smoke.yaml`: an esp32c6/esp-idf node with `api: actions: - action: ping, supports_response: only, then: - api.respond: data: !lambda 'root["ok"] = true;'`
- [ ] **Step 3:** Write the three `tools/ci/*.sh` scripts and `ci.yml` (triggers: `push` to any branch, and `pull_request`).
- [ ] **Step 4:** Commit: `git add -A && git commit -m "chore: repo scaffold and CI"`. Run `git show --stat HEAD`. Expected: no `custom.yaml`, no `secrets.yaml`.
- [ ] **Step 5:** Create the GitHub repo and push. **Ask the user to confirm first:** this creates a public repo.
  - `gh repo create joeyjojo747/esphome_irrigation_controller --public --description "ESP32-C6 smart irrigation controller for Home Assistant: autonomous ESPHome scheduling, flow, rain, BOM + AI weather adjustment" --source . --remote origin --push`
  - Then `gh repo edit joeyjojo747/esphome_irrigation_controller --add-topic esphome --add-topic home-assistant --add-topic irrigation --add-topic esp32`.
  - Create the work branch: `git switch -c dev && git push -u origin dev`.
- [ ] **Step 6:** CI: host-tests, ha-tests and esphome. Expected: all green.
  - `esphome` shows `Configuration is valid!` for `respond_smoke.yaml`.
  - If `api.respond` is rejected, stop and report it. Tasks 8, 11 and 12 then fall back to publishing JSON through `esphome.irrigation_event` with `type: "plan"`, `"runlog"` or `"config"`.

---

### Task 1: Date math, day modes and DST-safe scheduler

**Files:**
- Create: `logic/types.h`, `logic/civil.h`, `logic/civil.cpp`, `logic/schedule.h`, `logic/schedule.cpp` (under `esphome/irrigation-controller/components/irrigation_core/`)
- Test: `tests/host/test_schedule.cpp`

**Interfaces:**
- Produces in `types.h` (namespace `irrigation`):
  - `enum class DayMode : uint8_t { WEEKDAYS, ODD, EVEN, INTERVAL };`
  - `enum class HeadType : uint8_t { SPRAY, ROTOR, ROTATOR, DRIP, BUBBLER };`
  - `enum class Soil : uint8_t { SAND, LOAMY_SAND, SANDY_LOAM, LOAM, CLAY_LOAM, CLAY };`
  - `enum class Sun : uint8_t { FULL, PART, SHADE };`
  - `enum class Plant : uint8_t { LAWN, SHRUBS, TREES, GARDEN_BEDS, NATIVES, VEG };`
  - `enum class FlowAction : uint8_t { ALERT, ALERT_AND_STOP };`
  - `enum class RainType : uint8_t { NONE, SWITCH_NC, SWITCH_NO, TIPPING };`
  - `enum class Result : uint8_t { COMPLETED, STOPPED, FLOW_HIGH, FLOW_LOW, MAX_RUNTIME, SKIPPED_CONTROLLER_OFF, SKIPPED_RAIN_DELAY, SKIPPED_RAIN_SENSOR, SKIPPED_WEATHER };`
  - `constexpr int kZones = 16, kPrograms = 8, kStarts = 4; constexpr uint8_t kManual = 255;`
  - `struct Program { bool enabled; DayMode mode; uint8_t weekday_mask; uint8_t interval_days; int32_t interval_anchor_day; int16_t start_minutes[kStarts]; uint16_t zone_seconds[kZones]; uint8_t seasonal_pct; bool cycle_soak; bool weather_adjust; };`
    - `Program default_program()` returns: disabled, WEEKDAYS, mask 0, interval 1, anchor 0, starts all -1, zones all 0, pct 100, cycle_soak true, weather_adjust true.
  - `struct Zone { bool enabled; HeadType head; float precip_mm_h; Soil soil; uint8_t slope_pct; Sun sun; Plant plant; float area_m2; uint16_t max_cycle_s; uint16_t soak_s; float flow_baseline_lpm; uint8_t flow_high_pct; uint8_t flow_low_pct; FlowAction flow_action; int16_t output; };`
    - `constexpr int16_t kNoOutput = -1;`
    - `output` codes: `-1` none, `0..30` = board GPIO n, `100..115` = MCP23017 channel (A0..A7 = 100..107, B0..B7 = 108..115).
    - `Zone default_zone()` returns: enabled, SPRAY, 40.0, LOAM, 0, FULL, LAWN, 0, 0, 0, 0.0, 130, 70, ALERT, `kNoOutput`.
  - `struct LocalDateTime { int32_t day; int16_t minute; };`, where `day` is days since 1970-01-01 in local time and `minute` is 0–1439.
- Produces in `civil.h`:
  - `int32_t days_from_civil(int y, unsigned m, unsigned d);`
  - `void civil_from_days(int32_t z, int& y, unsigned& m, unsigned& d);`
  - `unsigned weekday_mon0(int32_t day);` (Monday = 0)
  - Use the Howard Hinnant algorithms.
- Produces in `schedule.h`:
  - `bool is_due_on(const Program& p, int32_t day);`
  - `struct DueStart { uint8_t program; uint8_t slot; };`
  - `class Scheduler { public: std::vector<DueStart> tick(LocalDateTime now, bool time_valid, const Program (&progs)[kPrograms]); void reset(); };`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("civil") {
  CHECK(days_from_civil(1970,1,1) == 0);
  CHECK(weekday_mon0(days_from_civil(2026,11,2)) == 0);   // Monday
  int y; unsigned m, d; civil_from_days(days_from_civil(2028,2,29), y, m, d);
  CHECK((y == 2028 && m == 2 && d == 29));
}
TEST_CASE("day modes") {
  Program p = default_program(); p.enabled = true;
  p.mode = DayMode::WEEKDAYS; p.weekday_mask = 0b0000101;            // Mon, Wed
  CHECK(is_due_on(p, days_from_civil(2026,11,2)));                   // Mon
  CHECK_FALSE(is_due_on(p, days_from_civil(2026,11,3)));             // Tue
  p.mode = DayMode::ODD;
  CHECK(is_due_on(p, days_from_civil(2026,10,29)));
  CHECK_FALSE(is_due_on(p, days_from_civil(2026,10,31)));            // 31st excluded
  p.mode = DayMode::EVEN; CHECK(is_due_on(p, days_from_civil(2026,10,30)));
  p.mode = DayMode::INTERVAL; p.interval_days = 3; p.interval_anchor_day = days_from_civil(2026,11,1);
  CHECK_FALSE(is_due_on(p, days_from_civil(2026,10,29)));            // before anchor
  CHECK(is_due_on(p, days_from_civil(2026,11,1)));
  CHECK_FALSE(is_due_on(p, days_from_civil(2026,11,2)));
  CHECK(is_due_on(p, days_from_civil(2026,11,4)));
}
// helper in tests/host/test_helpers.h (shared by later test files):
// Program daily_at(int16_t minute) -> enabled, WEEKDAYS mask 0x7F, start_minutes[0]=minute
TEST_CASE("fires once at start minute") {
  Program ps[kPrograms]; for (auto& p : ps) p = default_program(); ps[0] = daily_at(360);
  Scheduler s; int32_t d = days_from_civil(2026,11,2);
  CHECK(s.tick({d, 359}, true, ps).empty());
  auto due = s.tick({d, 360}, true, ps); REQUIRE(due.size() == 1); CHECK(due[0].program == 0);
  CHECK(s.tick({d, 360}, true, ps).empty());                          // same minute again
}
TEST_CASE("spring forward fires skipped 02:30 at 03:00") {
  /* ps[0]=daily_at(150); tick {d,119} then {d,180} -> exactly one DueStart; d = 2026-10-04 */
}
TEST_CASE("fall back fires 02:30 only once") {
  /* d = 2027-04-04; ticks 149,150,151,...,179 then clock goes back to 120, ticks 120..151 -> total DueStarts == 1 */
}
TEST_CASE("no catch-up after clock becomes valid") {
  /* ps[0]=daily_at(360); tick({d,0},false) ; tick({d,430},true) -> empty ; tick({d,431}) -> empty */
}
TEST_CASE("gap over 180 minutes does not fire old starts") {
  /* tick {d,100}; tick {d,400} with start 360 -> empty */
}
TEST_CASE("disabled program and unused slot never fire") { /* enabled=false; start_minutes all -1 */ }
```

- [ ] **Step 2:** CI: host-tests. Expected: FAIL to compile (`days_from_civil` undeclared).
- [ ] **Step 3: Implement the logic.** The tick algorithm is §5.1:
  - Keep `last_eval_` as a minute index (`day*1440+minute`), with -1 meaning unset.
  - On invalid time, reset to -1.
  - When unset, set `last_eval_ = cur-1`.
  - When `cur <= last_eval_`, set `last_eval_ = cur` and return empty.
  - When `cur - last_eval_ > 180`, set `last_eval_ = cur-1`.
  - Scan the minutes in `(last_eval_, cur]` against each enabled program's slots. Use `is_due_on` and the `int32_t last_fired_day_[kPrograms][kStarts]` guard, initialised to INT32_MIN.
- [ ] **Step 4:** CI: host-tests. Expected: all PASS.
- [ ] **Step 5:** Commit: `git commit -am "feat(core): date math, day modes, DST-safe scheduler"` (add the new files first).

---

### Task 2: Effective %, start decision, program expansion and plan projection

**Files:**
- Modify: `logic/schedule.h/.cpp` (add `effective_pct`, `decide_start`)
- Create: `logic/planner.h`, `logic/planner.cpp`
- Test: `tests/host/test_planner.cpp`

**Interfaces:**
- Consumes: Task 1 types, `is_due_on`, `civil_from_days`.
- Produces in `schedule.h`:
  - `uint16_t effective_pct(uint8_t program_pct, uint8_t month_pct, uint16_t weather_pct);` returns `program × month × weather / 10000`, rounded half up and clamped to 0..250.
  - `struct StartContext { bool controller_enabled; bool rain_delay_active; bool rain_hold; bool weather_skip_valid; };`
  - `enum class StartDecision : uint8_t { RUN, SKIP_CONTROLLER_OFF, SKIP_RAIN_DELAY, SKIP_RAIN_SENSOR, SKIP_WEATHER };`
  - `StartDecision decide_start(const Program& p, const StartContext& c, uint16_t pct);` uses the order in §5.1. A weather skip applies only if `p.weather_adjust`; `pct == 0` gives SKIP_WEATHER.
  - `Result to_result(StartDecision d);` maps each SKIP_* value to the matching SKIPPED_* result.
  - `struct WeatherAdjust { uint16_t pct; bool skip; char reason[32]; uint32_t valid_until_ts; };`
  - `uint16_t active_weather_pct(const WeatherAdjust& w, const Program& p, uint32_t now_ts);` returns `w.pct` only if `now_ts < w.valid_until_ts && p.weather_adjust`, else 100.
  - `bool active_weather_skip(const WeatherAdjust& w, uint32_t now_ts);` returns `w.skip && now_ts < w.valid_until_ts`.
- Produces in `planner.h`:
  - `struct Step { uint8_t zone; uint32_t run_s; uint32_t soak_s; };`
  - `std::vector<Step> expand_program(const Program& p, const Zone (&zones)[kZones], uint8_t zone_count, uint16_t pct, uint32_t hard_cap_s);`
  - `struct PlannedRun { uint8_t program; int32_t day; int16_t minute; uint32_t total_s; uint16_t pct; uint8_t zone_count; };`
  - `std::vector<PlannedRun> project(const Program (&progs)[kPrograms], const Zone (&zones)[kZones], uint8_t zone_count, const uint8_t (&monthly_pct)[12], LocalDateTime from, int days, std::function<uint16_t(const Program&, int32_t day)> weather_pct_for);`
  - `project` covers every start whose date and time fall within `[from, from + days)`, ordered by (day, minute, program). Starts whose expanded steps are empty are omitted.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("effective pct") {
  CHECK(effective_pct(100,100,100) == 100);
  CHECK(effective_pct(150,120,130) == 234);
  CHECK(effective_pct(200,200,200) == 250);            // clamp
  CHECK(effective_pct(100,0,100) == 0);
}
TEST_CASE("decide_start order") {
  Program p = default_program();
  CHECK(decide_start(p, {false,true,true,true}, 100) == StartDecision::SKIP_CONTROLLER_OFF);
  CHECK(decide_start(p, {true,true,true,true}, 100)  == StartDecision::SKIP_RAIN_DELAY);
  CHECK(decide_start(p, {true,false,true,true}, 100) == StartDecision::SKIP_RAIN_SENSOR);
  CHECK(decide_start(p, {true,false,false,true}, 100)== StartDecision::SKIP_WEATHER);
  p.weather_adjust = false;
  CHECK(decide_start(p, {true,false,false,true}, 100)== StartDecision::RUN);
  CHECK(decide_start(p, {true,false,false,false}, 0) == StartDecision::SKIP_WEATHER);
}
TEST_CASE("weather adjust expires") {
  Program p = default_program(); WeatherAdjust w{60, true, "Rain", 1000};
  CHECK(active_weather_pct(w, p, 999) == 60);   CHECK(active_weather_skip(w, 999));
  CHECK(active_weather_pct(w, p, 1000) == 100); CHECK_FALSE(active_weather_skip(w, 1000));
  p.weather_adjust = false; CHECK(active_weather_pct(w, p, 999) == 100);
}
TEST_CASE("expand interleaves cycle and soak") {
  Zone z[kZones]; for (auto& x : z) x = assigned_zone();   // test_helpers.h: default_zone() with output = 1
  z[0].max_cycle_s = 600; z[0].soak_s = 1800;
  Program p = default_program(); p.zone_seconds[0] = 1200; p.zone_seconds[1] = 300;
  auto s = expand_program(p, z, 16, 100, 7200);
  REQUIRE(s.size() == 3);
  CHECK((s[0].zone==0 && s[0].run_s==600 && s[0].soak_s==0));
  CHECK((s[1].zone==1 && s[1].run_s==300 && s[1].soak_s==0));
  CHECK((s[2].zone==0 && s[2].run_s==600 && s[2].soak_s==1800));
}
TEST_CASE("expand pct, remainder, minimum, cap, zone_count, disabled") {
  /* zone_seconds 1000 @150% -> one step 1500
     cycle_soak max 400, total 1000 -> 334,333,333
     total 50 s -> zone dropped
     total 9000 cap 7200 -> 7200 (split if cycle_soak)
     zone index >= zone_count dropped; z.enabled=false dropped; z.output == kNoOutput dropped;
     p.cycle_soak=false -> single step */
}
TEST_CASE("project 3 days") {
  /* ps[0] = daily_at(360), zone 0 = 600 s; monthly all 100; weather_pct_for returns 80
     from {2026-11-02, 400} days=3 -> runs on 11-03, 11-04, 11-05 at 360 (11-02 06:00 already past;
     window is [11-02 06:40, 11-05 06:40)) -> size 3
     total_s == 480, pct == 80 */
}
```

- [ ] **Step 2:** CI: host-tests. Expected: FAIL (undeclared).
- [ ] **Step 3:** Implement the functions with the signatures above. The split rule is §5.1: even split, remainder to the earliest cycles.
- [ ] **Step 4:** CI: host-tests. Expected: all PASS.
- [ ] **Step 5:** Commit `feat(core): effective pct, start decision, planner`.

---

### Task 3: Runner (program queue, soak waits, manual, hold, failsafe)

**Files:**
- Create: `logic/runner.h`, `logic/runner.cpp`
- Test: `tests/host/test_runner.cpp` (contains `FakeDriver`)

**Interfaces:**
- Consumes: `Step`, `Result`, `kManual`.
- Produces:
```cpp
struct ValveDriver { virtual void start(uint8_t zone, uint32_t run_s) = 0; virtual void stop_all() = 0;
                     virtual bool is_idle() const = 0; virtual ~ValveDriver() = default; };
struct StepEvent { uint32_t instance; uint8_t program; uint8_t zone; uint32_t start_s; uint32_t planned_s; uint32_t actual_s; Result result; };
class Runner {
 public:
  explicit Runner(ValveDriver* d);
  bool enqueue_program(uint8_t program, std::vector<Step> steps, uint32_t now_s); // false: empty, duplicate, or 8 already queued
  void run_manual_zone(uint8_t zone, uint32_t run_s, uint32_t now_s);
  void stop_all(uint32_t now_s);                  // current step -> STOPPED, queued programs dropped (not logged per step)
  void stop_zone_with(Result r, uint32_t now_s);  // ends current step with r, continues with next step
  void set_hold(bool hold, uint32_t now_s);       // hold=true: stops scheduled step (STOPPED) and drops scheduled queue; manual continues
  void loop(uint32_t now_s);                      // now_s = monotonic seconds
  bool busy() const; std::optional<uint8_t> active_zone() const; std::optional<uint8_t> active_program() const;
  std::function<void(const StepEvent&)> on_step_started, on_step_finished;
  std::function<void(uint32_t instance, uint8_t program)> on_program_finished;  // after last step of an instance (any result)
  static constexpr uint32_t kGraceS = 120;
};
```

- [ ] **Step 1: Write the failing tests**
  - `FakeDriver` records `start` calls and has a settable `idle` flag.
  - **Runs steps in order:** enqueue P0 `[{0,600,0},{1,300,0}]`.
    - `loop(0)` → driver starts zone 0 for 600 and `on_step_started` fires once.
    - Driver goes busy, then idle at t=600; `loop(600)` → `on_step_finished{zone0, actual 600, COMPLETED}` and zone 1 starts.
  - **Soak wait:** steps `[{0,60,0},{0,60,300}]`.
    - After step 1 finishes at t=60, `loop(100)` starts nothing and `busy()` is still true.
    - `loop(360)` starts zone 0.
  - **Duplicate program not queued:** `enqueue_program(0,...)` twice gives true, then false. Programs 0–7 can all be queued at once (true ×8). An empty step list returns false.
  - **Program finished:** `on_program_finished(instance, program)` fires exactly once after an instance's last step, including when `stop_all` ends it. A manual run counts as its own instance with `program == kManual`.
  - **FIFO:** P1 enqueued while P0 runs starts only after P0's last step.
  - **Manual run stops queue:**
    - While P0 step 1 runs at t=10, `run_manual_zone(3,120,10)` → finished event `{P0, zone0, actual 10, STOPPED}`.
    - The driver receives `stop_all` and then `start(3,120)`, and P0's remaining steps are gone.
    - The manual event has `program == kManual`.
  - **Hold:** `set_hold(true)` during P0 → current step STOPPED and the queue is cleared; `enqueue_program` while held still returns true (glue decides skips). A manual run during the hold starts.
  - **Failsafe:** the driver never reports idle. At `start + run_s + kGraceS` → `stop_all` is called and the step finishes with `MAX_RUNTIME`.
  - **Driver idle early** (external shutdown) → step finishes `STOPPED` with `actual_s` set to the elapsed time. This applies only when idle is seen *after* the driver had been non-idle, or more than 5 s after start.
  - **instance:** steps of the same enqueued program share `instance`. Each enqueue increments it, starting at 1.
- [ ] **Step 2:** CI: host-tests. Expected: FAIL.
- [ ] **Step 3:** Implement `Runner` as a state machine with states IDLE, WAIT_SOAK and RUNNING. Track `last_end_s_[kZones]` for soak.
- [ ] **Step 4:** CI: host-tests. Expected: PASS.
- [ ] **Step 5:** Commit `feat(core): runner state machine`.

---

### Task 4: Flow monitor, safety monitor, rain window

**Files:**
- Create: `logic/flow.h`, `logic/flow.cpp`
- Test: `tests/host/test_flow.cpp`

**Interfaces:**
- Produces:
```cpp
enum class FlowEvent : uint8_t { NONE, HIGH, LOW, LEAK };
struct FlowSettings { uint16_t settle_s = 60; float leak_lpm = 0.5f; uint16_t leak_hold_s = 60; uint16_t drain_s = 60; uint16_t sustain_s = 30; uint16_t min_learn_s = 120; };
class FlowMonitor {
 public:
  void configure(const FlowSettings& s);
  void zone_started(uint8_t zone, float baseline_lpm, uint8_t high_pct, uint8_t low_pct, uint32_t now_s);
  std::optional<float> zone_stopped(uint32_t now_s);   // returns new baseline if learnable (§5.1 EMA), else nullopt
  FlowEvent update(float flow_lpm, uint32_t now_s);     // call every second
};
class SafetyMonitor { public: bool exceeded(std::optional<uint8_t> active_zone, uint32_t now_s, uint32_t cap_s); };
class RainWindow { public: void add_tip(uint32_t now_s); float mm_last_24h(uint32_t now_s, float mm_per_tip) const; }; // 24 hourly buckets
```

- [ ] **Step 1: Write the failing tests** (baseline 10 L/min, high 130, low 70, defaults otherwise):
  - **No high during settle:** 20 L/min for t=0..59 → NONE every time.
  - **High:** 14 L/min sustained from t=60 → HIGH exactly once at t=90; NONE afterwards.
  - **Low:** 6 L/min from t=60 → LOW once at t=90.
  - **Zero flow is low, not leak:** 0 L/min from t=60 → LOW only, never LEAK.
  - **No baseline:** baseline 0 → never HIGH or LOW.
  - **Learning:**
    - First run of 300 s at a steady 12 L/min returns 12.0.
    - With baseline 10 and an average of 12 it returns 10.6 (±0.01).
    - A run with 100 s after settle returns nullopt, and so does a run that raised HIGH.
  - **No leak during drain-down:** after `zone_stopped(t=300)`, 2 L/min for t=300..359 → NONE.
  - **Leak:** 2 L/min from t=360 → LEAK once at t=420. After it drops to 0, a new 2 L/min episode can raise LEAK again.
  - **Safety:** zone active since t=0, cap 7200 → `exceeded` is false at 7199 and true at 7200. It resets when `active_zone` changes or becomes nullopt.
  - **RainWindow:** 10 tips at t=0 with 0.2 mm/tip → 2.0. At t=86400+3600 → 0.0.
- [ ] **Step 2:** CI: host-tests. Expected: FAIL.
- [ ] **Step 3:** Implement the classes. The average flow used for learning is the mean of the `update` samples after settle.
- [ ] **Step 4:** CI: host-tests. Expected: PASS.
- [ ] **Step 5:** Commit `feat(core): flow, safety, rain window`.

---

### Task 5: Run log ring and JSON serialisation

**Files:**
- Create: `logic/runlog.h`, `logic/runlog.cpp`
- Test: `tests/host/test_runlog.cpp`

**Interfaces:**
- Produces:
```cpp
struct RunRecord { uint32_t seq; uint32_t instance; uint8_t program; uint8_t zone; uint32_t start_ts; uint32_t planned_s; uint32_t actual_s; float litres; Result result; };
class RunLog { public: static constexpr size_t kCap = 48; uint32_t add(RunRecord r); // assigns seq (starts 1)
               std::vector<RunRecord> since(uint32_t seq) const; uint32_t last_seq() const; };
const char* result_name(Result r);            // "completed","stopped","flow_high","flow_low","max_runtime","skipped_controller_off","skipped_rain_delay","skipped_rain_sensor","skipped_weather"
std::string record_json(const RunRecord& r);  // {"seq":1,"inst":1,"p":0,"z":2,"ts":1793520000,"plan":600,"act":598,"l":61.2,"r":"completed"}
std::string records_json(const std::vector<RunRecord>& v);   // JSON array
std::string plan_json(const std::vector<PlannedRun>& v);     // [{"p":0,"date":"2026-11-03","start":"06:00","s":480,"pct":80,"zones":1}]
```
- The `"l"` value is printed with one decimal place. Strings are produced with `snprintf`; no JSON library.

- [ ] **Step 1: Write the failing tests:**
  - The exact `record_json` string above.
  - `plan_json` with one run on day `days_from_civil(2026,11,3)`, minute 360 gives the exact string above.
  - After 50 adds, `since(0).size() == 48`, the first returned seq is 3, and `since(49)` returns only seq 50.
  - `records_json({})` returns `"[]"`.
- [ ] **Step 2:** CI: host-tests. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** CI: host-tests. Expected: PASS.
- [ ] **Step 5:** Commit `feat(core): run log and json`.

---

### Task 5A: Runtime output map (pin codes, labels, validation, park level)

**Files:**
- Create: `logic/pinmap.h`, `logic/pinmap.cpp`
- Test: `tests/host/test_pinmap.cpp`

**Interfaces:**
- Consumes: `kNoOutput`, `kZones`, the output code scheme in `types.h`.
- Produces:
```cpp
enum class OutputKind : uint8_t { NONE, GPIO, EXPANDER };
struct OutputRef { OutputKind kind; uint8_t index; };            // GPIO n, or expander channel 0..15
OutputRef decode_output(int16_t code);                          // invalid codes -> NONE
std::string output_label(int16_t code);                         // "None", "GPIO 21", "MCP A0".."MCP B7"
int16_t parse_output_label(const std::string& label);           // inverse; unknown -> kNoOutput
std::vector<std::string> output_options(const std::vector<uint8_t>& candidates, bool has_expander); // "None" first, GPIOs ascending, then MCP A0..B7
enum class PinError : uint8_t { OK, NOT_CANDIDATE, RESERVED, DUPLICATE, NO_EXPANDER, INVALID };
struct BoardPins { std::vector<uint8_t> candidates; std::vector<uint8_t> reserved; bool has_expander; };
// slot 0..15 = zones, 16 = master; assigned[17] = current codes
PinError validate_assignment(uint8_t slot, int16_t code, const int16_t (&assigned)[kZones + 1], const BoardPins& b);
const char* pin_error_text(PinError e);  // "OK","GPIO not available on this board","pin is used by a sensor or bus","already assigned to <other>" (caller appends slot name),"no expander fitted","invalid output"
enum class Pull : uint8_t { DOWN, UP };
Pull park_pull(bool relay_active_high);   // released GPIO: input with pull to the OFF level -> active-high: DOWN, active-low: UP
```
- `kNoOutput` is always valid, so any number of slots can be unassigned. RESERVED is checked before NOT_CANDIDATE. Assigning a slot the code it already has is OK.

- [ ] **Step 1: Write the failing tests:**
  - **Round trips:** `output_label(21) == "GPIO 21"`, `output_label(100) == "MCP A0"`, `output_label(115) == "MCP B7"`, `output_label(-1) == "None"`. `parse_output_label` reverses each of them, and `parse_output_label("GPIO 99")` returns `kNoOutput`.
  - **Options:** `output_options({1,2,21}, false) == {"None","GPIO 1","GPIO 2","GPIO 21"}`. With the expander the list has 20 entries, ending `"MCP B7"`.
  - **Rejects reserved/duplicate/no-expander.** Board: candidates {1,2,3,6,7,18,21}, reserved {10,11,14,19,20,22,23}, no expander, zone 0 = 1, master = 21:
    - zone 1 → 22 gives RESERVED; → 9 gives NOT_CANDIDATE; → 1 gives DUPLICATE; → 21 gives DUPLICATE (clashes with the master).
    - zone 1 → 100 gives NO_EXPANDER.
    - zone 1 → 2 gives OK; zone 0 → 1 gives OK (unchanged).
    - zone 5 → `kNoOutput` gives OK.
    - code 50 gives INVALID.
  - **With the expander:** zone 1 → 100 gives OK. If zone 0 = 100, zone 1 → 100 gives DUPLICATE.
  - **Released pin parks at the off level:** `park_pull(true) == Pull::DOWN` and `park_pull(false) == Pull::UP`.
- [ ] **Step 2:** CI: host-tests. Expected: FAIL.
- [ ] **Step 3:** Implement the functions above.
- [ ] **Step 4:** CI: host-tests. Expected: PASS.
- [ ] **Step 5:** Commit `feat(core): runtime output map and validation`.

---

### Task 5B: Button commands and status LED pattern

**Files:**
- Create: `logic/buttons.h`, `logic/buttons.cpp`
- Test: `tests/host/test_buttons.cpp`

**Interfaces:**
- Consumes: nothing beyond `<cstdint>`.
- Produces:
```cpp
enum class ButtonFn : uint8_t { NONE, RUN_PROGRAM, STOP_ALL, NEXT_ZONE, RAIN_DELAY };
enum class ButtonCmd : uint8_t { NONE, RUN_PROGRAM, STOP_ALL, NEXT_ZONE, SET_RAIN_DELAY_24H, CLEAR_RAIN_DELAY };
struct ButtonState { bool controller_enabled; bool busy; bool rain_delay_active; };
ButtonCmd resolve_button(ButtonFn fn, const ButtonState& s);
enum class LedMode : uint8_t { OFF, ON, SLOW_BLINK, FAST_BLINK };
struct LedState { bool watering; bool rain_delay_or_hold; bool clock_valid; };
LedMode led_mode(const LedState& s);                       // clock invalid > watering > rain > off
bool led_level(LedMode m, uint32_t now_ms, uint32_t ack_until_ms); // ack flash overrides: on while now_ms < ack_until_ms
constexpr uint32_t kLedAckMs = 150;
```

- [ ] **Step 1: Write the failing tests:**
  - **Stop always allowed:** `STOP_ALL` → `ButtonCmd::STOP_ALL` for every combination of `controller_enabled`, `busy` and `rain_delay_active`.
  - **Start ignored while busy:** `RUN_PROGRAM` with `busy` → `NONE`; idle and enabled → `RUN_PROGRAM`; idle and disabled → `NONE`.
  - **Next zone:** busy → `NEXT_ZONE`; idle → `NONE`.
  - **Rain delay toggles:** inactive → `SET_RAIN_DELAY_24H`; active → `CLEAR_RAIN_DELAY`. Both are allowed while the controller is disabled.
  - **None:** `NONE` → `NONE` always.
  - **LED priority:** `{watering:true, rain:true, clock_valid:false}` → `FAST_BLINK`; `{true, true, true}` → `ON`; `{false, true, true}` → `SLOW_BLINK`; `{false, false, true}` → `OFF`.
  - **LED timing:** `SLOW_BLINK` is on for `now_ms % 2000 < 1000`; `FAST_BLINK` for `now_ms % 400 < 200`; `OFF` with `ack_until_ms = now_ms + 1` → true; `ON` is always true.
- [ ] **Step 2:** CI: host-tests. Expected: FAIL.
- [ ] **Step 3:** Implement the functions above.
- [ ] **Step 4:** CI: host-tests. Expected: PASS.
- [ ] **Step 5:** Commit `feat(core): button commands and status led pattern`.

---

### Task 6: ESPHome component glue and persistence

**Files:**
- Create: `esphome/irrigation-controller/components/irrigation_core/__init__.py`, `irrigation_core.h`, `irrigation_core.cpp`, `switch/__init__.py`
- Test fixtures: `tests/compile/core_expander.yaml`, `tests/compile/core_bad_pins.yaml`
- Test: `tests/compile/core_minimal.yaml` (esp32c6, sntp time, 2 template switches as valves, sprinkler, `irrigation_core`)

**Interfaces:**
- Consumes: all `logic/` APIs; `sprinkler::Sprinkler` (confirm `start_single_valve(size_t, optional<uint32_t>)`, `shutdown(bool)` and `active_valve()` in the pinned ESPHome source); `time::RealTimeClock`.
- Produces the YAML schema:
```yaml
irrigation_core:
  id: irr
  sprinkler_id: sprinkler_ctrl
  time_id: local_time
  flow_sensor: flow_rate        # optional sensor id, L/min
  output_candidates: [1, 2, 3, 6, 7, 18, 21]   # board GPIOs the user may assign (from substitution)
  reserved_pins: [19, 20, 22, 23, 14, 10, 11]  # sensor/I2C/button/LED pins in use (from substitutions)
  expander_id: mcp_relays       # optional; enables MCP channel codes 100..115
  default_outputs: [1, 2, 3, 6, 7, 18]         # first-boot zone 1..N outputs
  default_master_output: 21
  on_event:                     # Trigger<std::string, std::string>  variables: type, json
    - homeassistant.event: { event: esphome.irrigation_event, data: { node: "${name}", type: !lambda 'return type;', json: !lambda 'return json;' } }
```
- Produces C++ `IrrigationCore : Component` (public, used by YAML lambdas in Tasks 7–8):
  - Program access:
    - `Program& program(uint8_t i); void save_program(uint8_t i);`
    - `void set_program_interval_offset(uint8_t i, uint8_t days_until_first);`: anchor = today + offset.
  - Zones and settings:
    - `Zone& zone(uint8_t i); void save_zone(uint8_t i);`
    - `Settings& settings(); void save_settings();`
    - `Settings` = §5 Settings fields as a POD in `irrigation_core.h`, plus `int16_t master_output` and `bool relay_active_high` (default true, for the KS0011).
    - Button fields: `ButtonFn start_short_fn = RUN_PROGRAM, start_long_fn = NEXT_ZONE, stop_short_fn = STOP_ALL, stop_long_fn = RAIN_DELAY; uint8_t button_program = 0;`
  - Outputs (Task 5A logic):
    - `PinError assign_zone_output(uint8_t zone, int16_t code);` and `PinError assign_master_output(int16_t code);` validate with `validate_assignment`. On OK they call `stop_all()` if watering, park the old output, configure the new one at the off level, save, and emit `config_changed`. On error they change nothing, publish `last_error`, and log WARN.
    - `PinError set_relay_active_high(bool v);` calls `stop_all()`, then re-drives every assigned output to the new off level, then saves.
    - `void write_output(uint8_t slot, bool on);` is called by the output switch: slot 0–15 are zones, 16 is the master. Slots with no output log WARN and do nothing.
    - `std::string last_error() const;`
  - Output switch platform `IrrigationOutputSwitch : switch_::Switch`:
    - Config `slot: 1..16 | master`.
    - `write_state` → `parent->write_output(slot, state)`, then `publish_state(state)`.
    - Hardware access: GPIO via ESP-IDF `gpio_config` and `gpio_set_level`. Expander via `expander->pin_mode(ch, gpio::FLAG_OUTPUT)` and `digital_write(ch, level)`; confirm these are public on the pinned `mcp23x17_base`.
    - The level written is `on == relay_active_high`.
  - Setup: `setup_priority::HARDWARE`. Load NVS, then drive every assigned output to the off level **before** any other component starts. A failed load uses `default_outputs`/`default_master_output`.
  - Commands:
    - `void run_zone(uint8_t zone, uint32_t seconds); void run_program(uint8_t p); void stop_all();`
    - `void set_rain_delay_hours(uint16_t h); void set_weather_adjust(uint16_t pct, bool skip, const std::string& reason, uint16_t valid_hours);`
    - `void learn_flow(uint8_t zone);`: baseline set to 0, then `run_zone(zone, 180)`.
    - `void rain_tip(); void set_rain_switch(bool raw_state);`
    - `void next_zone();`: `runner.stop_zone_with(Result::STOPPED, now)`.
    - `bool rain_delay_active() const; void clear_rain_delay();`
    - `void button_press(bool is_start, bool is_long);`: picks the `ButtonFn` from settings, calls `resolve_button` (Task 5B), runs the command (`RUN_PROGRAM` → `run_program(settings().button_program)`), sets `led_ack_until_ms_ = millis() + kLedAckMs` when the command isn't `NONE`, and emits `button`.
    - `bool status_led_level() const;`: `led_level(led_mode({...}), millis(), led_ack_until_ms_)`.
  - JSON getters:
    - `std::string plan_json(int days); std::string runlog_json(uint32_t since_seq);`
    - `std::string config_json();`: programs, zones and settings, with keys mirroring the §5 field names. Outputs are emitted as `output_label` strings (`"output":"GPIO 1"`, `"master_output":"GPIO 21"`), so backup and restore round-trip through `set_zone` and `set_settings` unchanged.
  - Status getters:
    - `bool clock_valid() const; bool rain_hold() const; std::string status_text() const;`
    - `status_text` is one of `Idle`, `Watering <zone> (<program>)`, `Soaking`, `Rain delay`, `Rain hold`, `Clock not set`, `Disabled`.
  - Watering getters:
    - `uint16_t current_weather_pct() const; int32_t next_run_ts() const;`
    - `float zone_litres_total(uint8_t z) const; float total_litres() const;`
- Event `type` values and their `json` payloads:
  - `run_started`: `record_json` with `act:0`.
  - `run_finished`: `record_json`.
  - `skipped`: `record_json` with `z:255`.
  - `alert`: `{"kind":"flow_high|flow_low|leak|max_runtime|clock_invalid","z":N,"flow":x.x,"limit":y.y}`.
  - `program_finished`: `{"inst":N,"p":P,"zones":n,"act":total_s,"l":litres,"ts":first_start_ts,"warn":bool}` (from `Runner::on_program_finished`). `warn` is true if any step of the instance ended `flow_high`, `flow_low`, `max_runtime` or `stopped`.
  - `config_changed`: `{}`.
  - `button`: `{"btn":"start|stop","press":"short|long","fn":N,"cmd":N}` (`cmd` 0 means the press was ignored).
- Persistence:
  - Use `global_preferences->make_preference<T>(fnv1_hash("irr_programs"|"irr_zones"|"irr_settings"|"irr_weather"|"irr_litres"), true)`. `save_*` calls `pref.save()` and then `global_preferences->sync()`.
  - Litres are saved every 5 min while flow > 0, and at step end.
  - On first boot (load fails) use `default_program()`, `default_zone()` and the defaults in Settings §5.
- `loop()` runs at 1 Hz:
  1. Build `LocalDateTime` from `time->now()` (`is_valid()`, `timestamp`, local fields → `days_from_civil`).
  2. Run `Scheduler::tick`. For each due start, compute the effective pct (month from local date, weather pct when `now < valid_until` and the program opts in), then `decide_start`.
     - On a skip, add a RunLog record with `z=255` and emit `skipped`.
     - Otherwise run `expand_program` → `Runner::enqueue_program`.
  3. `runner.loop`; `FlowMonitor::update` with flow L/min; on HIGH, `stop_zone_with(FLOW_HIGH)` if the zone action is ALERT_AND_STOP; emit an alert for every event.
  4. `SafetyMonitor` on `sprinkler.active_valve()` with `zone_hard_cap_s` → `stop_all` + `max_runtime` alert.
  5. `runner.set_hold(rain_hold && rain_sensor_stops_running)`.
  6. Emit `clock_invalid` once per invalid period, 120 s after boot.
- Litres are attributed to the zone from `active_valve()`, so manual HA valve toggles count too.

- [ ] **Step 1:** Write `core_minimal.yaml`. Use 2 `irrigation_core` output switches as the valves and a master output switch, with no expander. Also write `core_expander.yaml`: the same plus `mcp23017` and `expander_id`. Then CI: esphome. Expected: FAIL `Component not found: irrigation_core`.
- [ ] **Step 2:** Implement `__init__.py`:
  - `CONFIG_SCHEMA` with the keys above.
  - The `on_event` automation, using `cg.std_string` arguments `type` and `json`.
  - `cg.add_build_flag("-std=gnu++17")` only if the pinned version needs it.
  - Build the `logic/*.cpp` sources. ESPHome compiles every `.cpp` in the component directory tree; confirm `logic/` is included, and if not, move the files flat.
- [ ] **Step 3:** Implement `irrigation_core.h/.cpp`. The `ValveDriver` adapter wraps the sprinkler: `start` → `start_single_valve(zone, run_s)`; `stop_all` → `shutdown(true)`; `is_idle` → `!active_valve().has_value()`.
- [ ] **Step 4:** CI: esphome (builds every fixture, including `core_minimal.yaml` and `core_expander.yaml`). Expected: `Configuration is valid!`, then `SUCCESS` for each.
  - Also check that `irrigation_core` rejects an overlap between `output_candidates` and `reserved_pins` at config time. A fixture `core_bad_pins.yaml` with 22 in both must fail `esphome config` with `pin 22 is both an output candidate and reserved`.
  - In `assign_zone_output`, the `stop_all()` call before re-mapping (`output change stops watering`) is verified on hardware in Task 9 item 10.
- [ ] **Step 5:** CI: host-tests. Expected: still PASS, since the glue does not change `logic/`.
- [ ] **Step 6:** Commit `feat(core): esphome glue, persistence, events`.

---

### Task 7: Device firmware packages (base, outputs, sprinkler, sensors, settings)

**Files:**
- Create: `esphome/irrigation-controller.yaml`, `esphome/secrets.yaml.example`, `esphome/irrigation-controller/{package,base,outputs,expander,sprinkler,core,sensors,status_led,settings}.yaml`
- Modify: `.github/workflows/ci.yml` (job `esphome`: replace the smoke config with `tools/ci/esphome.sh`, which now also builds the wrapper with and without the expander)
- Test: `tests/compile/with_expander.yaml` (the wrapper plus `expander.yaml`; `tools/ci/esphome.sh` builds it and the plain wrapper), `tests/ha/test_firmware_yaml.py` (static YAML assertions via a loader that ignores `!secret`/`!lambda` tags)

**Interfaces:**
- Consumes: Task 6 schema and methods.
- Produces these entity names (used by Tasks 8, 10, 18 and 19):
  - **Switches:** `Zone 1`..`Zone 16` (sprinkler valves), `Controller Enabled`
  - **Sensors:**
    - `Flow Rate` (L/min)
    - `Total Water` (L, `total_increasing`, `device_class: water`)
    - `Zone N Water` (L, `total_increasing`)
    - `Weather Adjust` (%)
    - `Rain Last 24h` (mm)
    - `Next Run` (timestamp)
    - `Remaining Time` (s)
  - **Text sensors:** `Status`, `Active Zone`, `Active Program`, `Zone N Name` (mirror)
  - **Binary sensors:** `Rain Hold`, `Leak`, `Flow Alarm`, `Clock Valid`, `HA Connected`
  - **Settings** (`entity_category: config`):
    - number `Zone Count` (1–16). Zones with output `None` are skipped by programs whatever the count.
    - selects `Zone 1 Output`..`Zone 16 Output` and `Master Output`. Options come from `output_options(candidates, has_expander)`. The current value is `output_label(code)`.
      - `set_action` calls `assign_*_output(parse_output_label(x))`. On an error the select re-publishes the old label, so HA snaps back.
    - select `Relay Trigger Level` (Active high, Active low), default Active high
    - text sensor `Last Error`, which shows `pin_error_text` + slot names, e.g. `Zone 2: GPIO 21 already assigned to Master`
    - numbers `Monthly Adjust Jan`..`Dec` (0–200 %)
    - `Pulses per Litre` (1–5000, default 450)
    - `Leak Threshold` (0.1–20 L/min)
    - `Flow Settle Time` (10–300 s)
    - select `Rain Sensor Type` (None, Switch NC, Switch NO, Tipping bucket)
    - `mm per Tip` (0.1–1.0)
    - `Rain Skip Threshold` (0–50 mm)
    - switch `Rain Sensor Stops Watering`
    - number `Zone Runtime Limit` (5–240 min)
    - selects `Start Button Short Press`, `Start Button Long Press`, `Stop Button Short Press` and `Stop Button Long Press`, each with options None, Run Program, Stop All, Next Zone, Rain Delay 24h. Defaults: Run Program, Next Zone, Stop All, Rain Delay 24h.
    - select `Button Program` (A–H), default A
  - **Buttons:** `Stop All`, `Rain Delay 24h`, `Clear Rain Delay`
- Settings entities read their state from `id(irr).settings()` in `on_boot` (priority -100) and write it back in `set_action`/`on_value` followed by `save_settings()`. They use `optimistic: true`, with no `restore_value`; the core is the source of truth.

- [ ] **Step 1:** Write `test_firmware_yaml.py`:
  - `test_autonomy_settings`: `base.yaml` has `api.reboot_timeout == "0s"` and `wifi.reboot_timeout == "0s"`.
  - `test_relays_always_off`: every `irrigation_core` output switch in `outputs.yaml` has `restore_mode: ALWAYS_OFF` and `internal: true`. There are 17 of them: slots 1–16 plus master.
  - `test_no_gpio_relay_switches`: no `platform: gpio` switch exists anywhere, because all relays go through the runtime map.
  - `test_pins_not_reused`: the substitutions `flow_pin`, `rain_pin`, `start_pin`, `stop_pin`, `led_pin`, `sda_pin` and `scl_pin` don't appear in `output_candidates`, are all different, and all appear in `reserved_pins`.
  - `test_buttons_local`: the Start and Stop binary sensors in `sensors.yaml` call `id(irr).button_press(...)` in their lambdas and contain no `homeassistant.` action, so they never depend on HA.
  - `test_no_plaintext_secrets`: the `api.encryption.key` value is `!secret api_key` and the `ota` password is `!secret ota_password`. No tracked file (`git ls-files`) contains a `key:` followed by a 44-character base64 literal, or a `password:` followed by a 32-hex-digit literal. Never write real secret values into any repo file, including this plan.
  - `test_sixteen_valves`: `sprinkler.yaml` has 16 valves.
  - Then CI: ha-tests. Expected: FAIL (files missing).
- [ ] **Step 2:** Write `base.yaml`:
  - `esp32: board: esp32-c6-devkitc-1, variant: esp32c6, framework: esp-idf`
  - `api` with encryption from secrets and `reboot_timeout: 0s`; `wifi` with `reboot_timeout: 0s` and a fallback AP
  - `web_server: version: 3`, `ota`, `logger`
  - `preferences: flash_write_interval: 5min`
  - `time:` with `homeassistant` + `sntp` (id `local_time`, `timezone: ${timezone}`); `i2c` on SDA 19 / SCL 20
  - `ds1307` RTC (works with DS3231) behind substitution `rtc_enabled`. Its `on_time_sync` on sntp calls `ds1307.write_time`, and `on_boot` calls `ds1307.read_time`.
- [ ] **Step 3:** Write the output packages:
  - `outputs.yaml` declares `switch: - platform: irrigation_core` with ids `zone_out_1..16` (`slot: 1..16`) and `master_out` (`slot: master`).
  - `expander.yaml` declares `mcp23017: - id: mcp_relays, address: 0x20` and `irrigation_core: expander_id: mcp_relays`. ESPHome's package merge folds this into the core block.
  - Board substitutions, defined in the wrapper and passed to `core.yaml`:
    - `output_candidates: "1, 2, 3, 6, 7, 18, 21"`
    - `flow_pin: "23"`, `rain_pin: "22"`, `start_pin: "10"`, `stop_pin: "14"`, `led_pin: "11"`, `sda_pin: "19"`, `scl_pin: "20"`
    - `default_outputs: "1, 2, 3, 6, 7, 18"`, `default_master_output: "21"`
- [ ] **Step 4:** Write `sprinkler.yaml`:
  - Controller `sprinkler_ctrl` with `main_switch: Irrigation Main` (internal), `auto_advance_switch` (internal) and `pump_start_pump_delay: 2s` / `pump_stop_valve_delay: 2s`.
  - Valves 1..16: `valve_switch: "Zone N"`, `valve_switch_id: zone_out_N`, `pump_switch_id: master_out`, `run_duration: 15min`.
  - No `multiplier_number`; the core owns run times.
- [ ] **Step 5:** Write `sensors.yaml`:
  - `pulse_counter` on `${flow_pin}` with `internal_filter: 13us`, `update_interval: 1s` and `total`.
  - Template `flow_rate` = pulses × 60 / `settings().pulses_per_litre`.
  - Rain `${rain_pin}` binary sensor (`allow_other_uses: true`) → `id(irr).set_rain_switch(x)`; the same binary sensor's `on_press` (filter `delayed_on: 50ms`) → `id(irr).rain_tip()`. The core ignores tips unless the type is Tipping, and ignores the switch state when it is Tipping or None.
  - Start `${start_pin}` and Stop `${stop_pin}` binary sensors: pin `mode: { input: true, pullup: true }` and `inverted: true`, filter `delayed_on_off: 50ms`. Each has `on_multi_click`:
    - short: `ON for at most 1s` then `OFF for at least 50ms` → `id(irr).button_press(is_start, false)`
    - long: `ON for at least 2s` → `id(irr).button_press(is_start, true)` (fires while held)
  - Write `status_led.yaml`: a `gpio` output on `${led_pin}` and an `interval: 50ms` lambda that writes `id(irr).status_led_level()`. It is a commented-out include in `package.yaml`, like `expander`.
- [ ] **Step 6:** Write `core.yaml` (the `irrigation_core` block, status sensors with `update_interval: 5s` lambdas) and `settings.yaml`. 
  - Write `irrigation-controller/package.yaml` with `packages:` `!include` entries for base, outputs, sprinkler, core, sensors, settings, editor_program, editor_zone and api_actions. Leave the last three commented out until Task 8. `expander` and `status_led` are commented-out entries; the wrapper explains when to uncomment them ("fit an MCP23017 for more than 6 zones", "fit an LED on `led_pin`"). The `with_expander.yaml` fixture also includes `status_led.yaml`, so CI compiles both.
  - It also holds `external_components: - source: { type: local, path: components }`.
  - Write the wrapper `irrigation-controller.yaml`, modelled on ESPyRain's `espyrain_controller_wrapper.yaml`:
    - A header comment block tells the user what to change: secrets, board, timezone, static IP, sensor pins and the expander include. It also says relay outputs are **not** set here; they are set in HA under Settings → Hardware.
    - Then come substitutions `name`, `friendly_name`, `timezone`, `rtc_enabled` and `version: "0.1.0"`, and `esphome: project: { name: "joeyjojo747.irrigation-controller", version: "${version}" }`.
    - **Option A (default, local copy):** `packages: irrigation: !include irrigation-controller/package.yaml`.
    - **Option B (remote, commented out):** `packages: irrigation: github://joeyjojo747/esphome_irrigation_controller/esphome/irrigation-controller/package.yaml@v${version}`, plus `external_components: - source: github://joeyjojo747/esphome_irrigation_controller@v${version}` with `path: esphome/irrigation-controller/components` and `components: [irrigation_core]`.
- [ ] **Step 7:** CI: ha-tests and esphome. Expected: 7 passed; both builds `SUCCESS`.
- [ ] **Step 8:** Commit `feat(fw): device packages with runtime-mapped outputs`.

---

### Task 8: Program/zone editors and API actions

**Files:**
- Create: `esphome/irrigation-controller/editor_program.yaml`, `editor_zone.yaml`, `api_actions.yaml`; uncomment them in `irrigation-controller/package.yaml`
- Test: extend `tests/ha/test_firmware_yaml.py`; compile with and without the expander

**Interfaces:**
- Consumes: Task 6 methods and Task 7 settings pattern.
- **Program editor** entities (prefix `Program Editor`; all `entity_category: config` except Save/Revert):
  - select `Program Editor Program` (A–H): loads the buffer from `program(i)`; a template text `Program N Name` exists per program.
  - switch `Program Editor Enabled`
  - select `Program Editor Day Mode` (Weekdays, Odd days, Even days, Every N days)
  - switches `Program Editor Mon`..`Sun`
  - numbers `Program Editor Interval Days` (1–30) and `Program Editor Days Until First Run` (0–29)
  - datetimes `Program Editor Start 1`..`4` (`type: time`), plus switches `Program Editor Start N Active`
  - numbers `Program Editor Zone N Minutes` (0–240, step 1)
  - number `Program Editor Seasonal Adjust` (0–200 %)
  - switches `Program Editor Cycle Soak` and `Program Editor Weather Adjust`
  - buttons `Program Editor Save` and `Program Editor Revert`
  - The buffer is a `globals` `irrigation::Program` held in RAM. Save copies it into `program(i)`, calls `save_program` and `set_program_interval_offset`, and emits `config_changed`.
- **Zone editor** entities (prefix `Zone Editor`):
  - select `Zone Editor Zone` (1–16)
  - text `Zone Editor Name` (max 24): writes the `Zone N Name` text, which uses `restore_value`
  - switch `Zone Editor Enabled`
  - selects `Zone Editor Head Type`, `Zone Editor Soil`, `Zone Editor Sun` and `Zone Editor Plant`; labels are the enum names in title case
  - numbers:
    - `Zone Editor Precipitation` (1–100 mm/h)
    - `Zone Editor Slope` (0–50 %)
    - `Zone Editor Area` (0–5000 m²)
    - `Zone Editor Max Cycle` (0–60 min, 0 = off)
    - `Zone Editor Soak` (0–120 min)
    - `Zone Editor Flow Baseline` (0–200 L/min)
    - `Zone Editor High Flow` (100–300 %) and `Zone Editor Low Flow` (0–100 %)
  - select `Zone Editor Flow Action` (Alert, Alert and stop)
  - select `Zone Editor Output`: same options and validation as `Zone N Output`. It applies immediately when changed, not on Save, because a pin change must stop watering and re-drive the hardware.
  - buttons `Zone Editor Save`, `Zone Editor Revert`, `Zone Editor Learn Flow` and `Zone Editor Test Run` (3 min)
- **API actions** (`api: actions:`), argument names exact:
  - `run_zone(zone:int 1-based, minutes:int)`, `run_program(program:int 0-7)`, `stop_all()`
  - `rain_delay(hours:int)`
  - `set_weather_adjust(pct:int, skip:bool, reason:string, valid_hours:int)`
  - `set_program(program:int, enabled:bool, mode:int, weekday_mask:int, interval_days:int, days_until_first:int, starts:int[] (minutes, -1 unused), zone_minutes:int[], seasonal_pct:int, cycle_soak:bool, weather_adjust:bool)`
  - `set_zone(zone:int 1-based, name:string, enabled:bool, head:int, precip:float, soil:int, slope:int, sun:int, plant:int, area:float, max_cycle_min:int, soak_min:int, baseline:float, high_pct:int, low_pct:int, action:int, output:string)`: `output` is an `output_label`; `""` leaves the output unchanged. A validation error rejects the whole call (WARN, `Last Error` set, nothing changed).
  - `set_settings(zone_count:int, controller_enabled:bool, monthly_pct:int[] (12), pulses_per_litre:float, leak_lpm:float, settle_s:int, rain_type:int, mm_per_tip:float, rain_skip_mm:float, rain_stops:bool, hard_cap_min:int, start_short_fn:int, start_long_fn:int, stop_short_fn:int, stop_long_fn:int, button_program:int, master_output:string, relay_active_high:bool)`: a `monthly_pct` array that isn't 12 long is rejected (WARN, no change).
  - Response actions (`supports_response: only`): `get_plan(days:int)` → `{"plan": <plan_json>}`, `get_run_log(since:int)` → `{"records": <runlog_json>, "last": N}`, `get_config()` → `<config_json>`.
  - Out-of-range arguments are clamped, and they are logged at WARN.

- [ ] **Step 1:** Add tests:
  - `test_actions_present` checks the exact action names above.
  - `test_editor_save_buttons` checks that both Save buttons exist.
  - CI: ha-tests. Expected: FAIL.
- [ ] **Step 2:** Write the three packages. Action lambdas write through `program(i)`, `zone(i)` and `settings()` and then call the matching `save_*`; `set_program`/`set_zone` also emit `config_changed`.
- [ ] **Step 3:** CI: ha-tests and esphome. Expected: PASS; both `SUCCESS`.
- [ ] **Step 4:** Commit `feat(fw): program/zone editors and api actions`.

---

### Task 9: Bench test on hardware (manual acceptance)

**Files:**
- Create: `docs/install.md` (wiring + flashing sections), `docs/bench-test.md` (checklist with results)

- [ ] **Step 1:** Check the pin table in design §4 against the DFRobot DFR1075 pinout diagram, and record any changes in `docs/install.md`. In particular, confirm GPIO 10 (Start button) and GPIO 11 (status LED) are broken out; if not, use the fallback in design §4.2. If a pin is not broken out, change the `output_candidates` / `default_outputs` / sensor-pin substitutions in the wrapper and re-run Task 7 Step 7.
- [ ] **Step 2:** Wire one KS0011 to GPIO 1 (VCC 5 V, GND, IN). Then, in the **ESPHome Device Builder** add-on on the user's HA:
  1. Copy `esphome/irrigation-controller/` to `/config/esphome/irrigation-controller/` with the File editor or Samba. Alternatively use the wrapper's Option B remote packages from the `dev` branch.
  2. Click New device, paste in `esphome/irrigation-controller.yaml`, and fill in the Secrets editor.
  3. Click Install → "Plug into this computer" with the C6 on USB to the PC running the browser. Later installs use "Wirelessly".
  4. Adopt the device in HA and enable "Allow the device to perform Home Assistant actions".
  - Expected: the device appears in HA with the Task 7 and 8 entities. Record the first compile time on the Pi 4 in `docs/install.md`.
- [ ] **Step 3:** Run the checklist in `docs/bench-test.md`, recording pass or fail for each item:
  1. Zone 1 switch clicks the relay, and Status shows `Watering`.
  2. If a KS0011 does not switch at 3.3 V, record it and add a 2N2222/level shifter to install.md.
  3. A program for 2 min at now+2 runs at the right minute. Calendar `get_plan` shows it. Call `esphome.irrigation_controller_get_plan` in Developer Tools and check the response JSON.
  4. Disconnect Wi-Fi (power off the AP) for 30 min with a program due. It runs, the device doesn't reboot (uptime keeps increasing), and after reconnect `get_run_log` contains the run.
  5. Power-cycle with no Wi-Fi and the DS3231 fitted. The clock is valid and the next program runs. Without the RTC, `Clock Valid` is off and nothing scheduled runs.
  6. Shorting the rain input (Switch NO) gives `Rain Hold` on; a due program is logged `skipped_rain_sensor`.
  7. A signal-generator or fan on the flow input with no zone on gives a Leak alert after drain + hold time.
  8. Zone Runtime Limit at 5 min with a manual 10 min run stops at 5 min and raises a `max_runtime` alert.
  9. Change the Zone Count from 6 to 3 without a reflash; zones 4–6 are excluded from programs (`get_plan` totals drop).
  10. **Output change stops watering.** While Zone 1 waters on GPIO 1, set `Zone 1 Output` to `GPIO 2` in HA and move the relay IN wire to GPIO 2.
      - Watering stops and is logged `stopped`. The relay on GPIO 1 stays **off**; check with a multimeter that GPIO 1 reads ≈ 0 V.
      - A test run then clicks the relay on GPIO 2. All of this happens with no reflash.
  11. **Validation.** Setting `Zone 2 Output` to `GPIO 2` (taken) or to `MCP A0` with no expander snaps the select back. `Last Error` shows the reason.
  12. **Trigger level.** Switching `Relay Trigger Level` to Active low turns every relay **on** while idle, which shows the level change took effect; switching back to Active high turns them off. Record the result. Install docs must tell users with active-low boards to set this before wiring solenoids.
  13. **Boot state.** Power-cycle 5 times with Zone 1 mapped to GPIO 1 and watch the relay LED. Expected: no click or flash at boot. If it glitches, add a 10 k pull-down from IN to GND to the install docs.
  14. **Buttons with HA offline.** Set Program A to zones 1–2 at 2 min each, wire Start to GPIO 10, Stop to GPIO 14 and the LED to GPIO 11 (with `status_led.yaml` included), then stop HA Core (or power off the AP).
      - A short Start press runs Program A, and the LED stays on while watering. A second short press while it runs does nothing.
      - A long Start press (2 s) moves to the next zone.
      - A short Stop press stops all watering at once.
      - A long Stop press sets a 24 h rain delay (LED slow blink); a second long press clears it.
      - After HA is back, the runs appear in `get_run_log` and the calendar.
  15. **Stop always works.** With `Controller Enabled` off, a short Start press does nothing, but a Stop press during a manual zone run from the web UI still stops it.
  16. **Functions from HA.** Set `Start Button Short Press` to Stop All in HA, then disconnect Wi-Fi and power-cycle. A short Start press now stops watering, which shows the setting was kept in NVS. Set it back to Run Program.
- [ ] **Step 4:** Commit `docs: install wiring and bench test results`.

---

### Task 10: HA package skeleton, helpers, test harness, config check

**Files:**
- Create: `packages/irrigation/irrigation_helpers.yaml`
- Create: `tests/ha/conftest.py`, `tests/ha/test_helpers_pkg.py`, `tests/ha/config/configuration.yaml` (minimal config: `homeassistant: packages: !include_dir_named ../../../packages/irrigation`, `default_config:` omitted)
- Modify: `.github/workflows/ci.yml` (job `ha-tests` adds `python -m homeassistant --script check_config -c tests/ha/config`)

**Interfaces:**
- HA loads the package with one line in `configuration.yaml`, added once at install: `homeassistant: packages: !include_dir_named packages/irrigation`. Every later task adds one file to `packages/irrigation/`.
- The ESPHome actions this package calls are fixed: `esphome.irrigation_controller_<action>` (device name `irrigation-controller`, see Global Constraints).
- **Helpers** (exact ids; all `irrigation_` prefixed; YAML-defined helpers whose *values* are edited in the dashboard):
  - `input_text`:
    - `irrigation_weather_entity` (initial `weather.home`), `irrigation_rain_obs_entity` (initial `""`)
    - `irrigation_ai_entity` (initial `""`): an `ai_task.*` entity id, or a conversation `agent_id`
    - `irrigation_notify_targets` (initial `""`): comma-separated `notify` service names, e.g. `mobile_app_brads_phone`
    - `irrigation_package_version` (initial = `VERSION`)
  - `input_select.irrigation_ai_mode`: `Off`, `AI Task`, `Conversation agent` (initial `Off`)
  - `input_datetime` (`has_date: false`): `irrigation_eval_time` 03:00, `irrigation_quiet_start` 22:00, `irrigation_quiet_end` 07:00
  - `input_number`, defaults from design §6:
    - Rain: `irrigation_rain_skip_mm` 3.0, `irrigation_rain_prob_pct` 80, `irrigation_rain_prob_mm` 2.0, `irrigation_cumulative_days` 3, `irrigation_cumulative_mm` 15
    - Freeze and wind: `irrigation_freeze_c` 2, `irrigation_wind_kmh` 30
    - Temperature: `irrigation_cool_c` 18, `irrigation_cool_pct` 70, `irrigation_hot_c` 32, `irrigation_hot_pct` 130
    - Push and AI: `irrigation_valid_hours` 30, `irrigation_ai_max_pct` 150
    - Other: `irrigation_tariff_per_kl` 3.00, `irrigation_offline_minutes` 30
    - Sync bookkeeping: `irrigation_last_ts` and `irrigation_last_seq` (0–4e9, `mode: box`)
  - `input_boolean`:
    - `irrigation_wind_enabled` off, `irrigation_ai_auto_apply` off
    - `irrigation_notify_<category>` for every category in design §8, with the §8 defaults
- `tests/ha/conftest.py` provides these fixtures:
  - `pkg_hass`: loads every `packages/irrigation/*.yaml` with `homeassistant.util.yaml.load_yaml`, merges them with `homeassistant.config.merge_packages_config`, and sets up each domain present (`input_*`, `template`, `script`, `automation`, `utility_meter`).
  - `device`: registers mock `esphome.irrigation_controller_*` services with `SupportsResponse`. Calls are recorded in `device.calls[action]` and responses are set with `device.respond(action, dict)`. It also enforces DUPLICATE on `set_zone` outputs, for the Task 11 restore test.
  - `mocks`: mock `weather.get_forecasts`, `ai_task.generate_data`, `conversation.process`, `calendar.create_event` and `notify.mobile_app_test`, each with a settable response and a call list.

- [ ] **Step 1:** Write `test_helpers_pkg.py`:
  - `test_helpers_defaults`: after `pkg_hass`, `input_number.irrigation_rain_skip_mm == 3.0`, `input_select.irrigation_ai_mode == "Off"` and `input_boolean.irrigation_notify_run_started == off`.
  - `test_every_notify_category_has_toggle`: one toggle for each of the 10 §8 categories.
  - `test_package_version_matches`: the helper matches `VERSION`.
- [ ] **Step 2:** CI: ha-tests. Expected: FAIL (package missing).
- [ ] **Step 3:** Write the helpers file and the minimal test config.
- [ ] **Step 4:** CI: ha-tests. Expected: 3 passed and `check_config` reports no errors.
- [ ] **Step 5:** Commit `feat(ha): irrigation package helpers and test harness`.

---

### Task 11: Event routing, run-log sync after outages, backup and restore

**Files:**
- Create: `packages/irrigation/irrigation_sync.yaml`
- Test: `tests/ha/test_sync.py`

**Interfaces:**
- **Automation `irrigation_event_router`:**
  - Trigger: event `esphome.irrigation_event` with `event_data.node == "irrigation-controller"`.
  - It parses `trigger.event.data.json | from_json` and re-fires `irrigation_<type>`, with the parsed payload as event data, for Tasks 12–17 to consume.
  - For `run_finished` and `skipped`, it advances `irrigation_last_ts` / `irrigation_last_seq` when `(ts, seq)` is newer.
  - For `config_changed`, it runs `script.irrigation_refresh_plan` (Task 12) and `script.irrigation_backup_config`.
- **Script `irrigation_sync_runlog`:**
  - Calls `get_run_log(since: 0)` with a response. A device reboot resets `seq`, so it filters by time, not seq.
  - For every record with `ts > last_ts`, or `ts == last_ts and seq > last_seq`, in ascending order, it fires `irrigation_replayed_record` and advances the helpers.
  - It then fires `irrigation_replay_done` with the replayed records grouped by `inst`: `{inst, p, zones, act, l, warn, ts}`.
- **Automation `irrigation_reconnect`:** when `sensor.irrigation_status` changes from `unavailable` to any other state, it runs `irrigation_sync_runlog` and then `irrigation_refresh_plan`.
- **Run history:** trigger-based template sensor `sensor.irrigation_run_history`. It triggers on `irrigation_run_finished` and `irrigation_replayed_record`.
  - Attribute `records`: the last 7 days of records, capped at 300.
  - State: the record count.
  - Exclude it from the recorder in the package (`recorder: exclude: entities:`).
- **Backup and restore:**
  - Script `irrigation_backup_config` calls `get_config` and fires `irrigation_config_backup`.
  - Trigger template sensor `sensor.irrigation_config_backup`: state = backup time, attribute `config` = the JSON object. It is restored across restarts and included in HA backups.
  - Script `irrigation_restore_config` replays `config` in this order:
    1. `set_zone` ×16 with only `output: "None"`
    2. `set_settings`
    3. `set_program` ×8
    4. `set_zone` ×16 with full values

- [ ] **Step 1: Write the failing tests:**
  - `test_router_refires_typed_event`
  - `test_router_ignores_other_nodes`
  - `test_replay_after_outage`: helpers at ts 1000, seq 5; the device returns seq 1–3 with ts 900/1100/1200; exactly 2 records are replayed and last_ts becomes 1200.
  - `test_replay_groups_by_instance`
  - `test_history_capped_and_7_days`
  - `test_backup_restore_roundtrip`: the call counts are 32 `set_zone`, 1 `set_settings` and 8 `set_program`, in the order above; the final arguments equal the backup.
  - `test_restore_swapped_outputs`: the device fixture enforces DUPLICATE, and every call is accepted.
- [ ] **Step 2:** CI: ha-tests. Expected: FAIL.
- [ ] **Step 3:** Write `irrigation_sync.yaml`.
- [ ] **Step 4:** CI: ha-tests. Expected: PASS.
- [ ] **Step 5:** Commit `feat(ha): event routing, outage replay, backup/restore`.

---

### Task 12: Calendar history and upcoming-runs sensor

**Files:**
- Create: `packages/irrigation/irrigation_calendar.yaml`
- Test: `tests/ha/test_calendar_pkg.py`

**Interfaces:**
- The user creates a **Local Calendar** named `Irrigation` in the UI at install, giving `calendar.irrigation`. The package only writes to it with `calendar.create_event`.
  - HA has no service to delete calendar events, so **future** runs are not put in the calendar. They live in the upcoming sensor below, which keeps the calendar a read-only record of what happened.
- **Automation `irrigation_calendar_writer`:**
  - **`irrigation_program_finished`** (live) and each `irrigation_replay_done` group create an event:
    - start = `ts`, end = `ts + act`
    - summary `"{name} · {zones} zones · {round(act/60)} min · {l|round(0)} L"`, with `" ⚠"` appended when `warn`
    - `{name}` is `Program X` (A–H), or the program's name from `sensor.irrigation_config_backup` when it is set; `Manual` for `p == 255`
  - **`irrigation_skipped`** creates a 15-min event `"{name} skipped — {label}"`. The labels are `rain sensor`, `rain delay`, `weather` and `controller off`.
- **Script `irrigation_refresh_plan`:**
  - Calls `get_plan(days: 14)` and fires `irrigation_plan`.
  - Trigger template sensor `sensor.irrigation_upcoming` (device_class `timestamp`): state = the next start as local ISO, or `unknown`. Attribute `runs` = `[{program, name, start (ISO local), minutes, pct}]`.
  - An automation runs it every 30 min, at HA start and after the daily adjust.

- [ ] **Step 1: Write the failing tests:**
  - `test_finished_event_summary`: exact string `"Program A · 3 zones · 42 min · 610 L"`
  - `test_warning_marker`
  - `test_skipped_event_label`
  - `test_manual_named_manual`
  - `test_replay_groups_create_one_event_each`
  - `test_upcoming_sensor_from_plan`: plan item `{"p":0,"date":"2026-11-03","start":"06:00","s":480,"pct":80}` in `Australia/Sydney` gives state `2026-11-03T06:00:00+11:00` and minutes 8.
- [ ] **Step 2:** CI: ha-tests. Expected: FAIL.
- [ ] **Step 3:** Write the package file.
- [ ] **Step 4:** CI: ha-tests. Expected: PASS.
- [ ] **Step 5:** Commit `feat(ha): irrigation calendar history and upcoming runs`.

---

### Task 13: Weather rules and daily push

**Files:**
- Create: `packages/irrigation/irrigation_weather.yaml`
- Test: `tests/ha/test_weather_pkg.py`

**Interfaces:**
- **Script `irrigation_evaluate_weather`** (`response_variable`). It returns `{skip: bool, pct: int|null, reason: str, inputs: {...}}`.
  - It calls `weather.get_forecasts` (`type: daily`) on `states('input_text.irrigation_weather_entity')` and takes today's entry: `templow`, `temperature`, `precipitation`, `precipitation_probability`, `wind_speed`.
  - Observed 24 h rain comes from `input_text.irrigation_rain_obs_entity`, if one is set.
  - The cumulative total is taken from `sensor.irrigation_rain_history`, the sum of the last `cumulative_days` values. That is a trigger template that snapshots the observation sensor daily at 08:59 and keeps 14 days.
  - Rules, in the first-match order of design §6, read every threshold from the Task 10 helpers. Reason strings are exact:
    - `"Rain {x:.1f} mm (24h + forecast)"`
    - `"Rain chance {p}%"`
    - `"Frost risk {t:.1f}°C"`
    - `"Wind {w:.0f} km/h"`
    - `"Rain {x:.1f} mm in {n} days"`
  - Non-skip pct is cool → `cool_pct`, hot → `hot_pct`, else 100; missing values give 100.
  - If the weather entity is unavailable, or today is missing, it returns `pct: null` and `reason: "weather unavailable"`.
- **Automation `irrigation_daily_adjust`:**
  - Trigger: time `input_datetime.irrigation_eval_time`.
  - It runs evaluate. If `pct` is null, it fires `irrigation_decision` with the error and **pushes nothing**.
  - Otherwise it runs `script.irrigation_ai_advice` (Task 14, which returns the final decision), then calls `set_weather_adjust(pct, skip, reason, valid_hours)` and fires `irrigation_decision`.
- Trigger template `sensor.irrigation_weather_decision`: state = `skip` or the pct; attributes `reason`, `inputs`, `pushed_at` and `ai_applied`.
- BOM entities come from the HACS `bremor/bureau_of_meteorology` integration. Any weather entity with daily forecasts works.

- [ ] **Step 1: Write the failing tests.** Each case is a script call with mocked forecasts:
  - rain 2.5 obs + 1.0 forecast → skip, `"Rain 3.5 mm (24h + forecast)"`
  - prob 85 / 2.0 mm → skip; prob 85 / 1.0 mm → none
  - min 1.5 → `"Frost risk 1.5°C"`; 2.1 → none
  - wind 35 → none by default; with `irrigation_wind_enabled` on → `"Wind 35 km/h"`
  - cumulative 15.0 → `"Rain 15.0 mm in 3 days"`
  - max 17.9 → 70; 32.1 → 130; 25 → 100; None → 100
  - rule order
  - `test_daily_adjust_pushes`
  - `test_unavailable_weather_pushes_nothing` (no `set_weather_adjust` call)
- [ ] **Step 2:** CI: ha-tests. Expected: FAIL.
- [ ] **Step 3:** Write the package file.
- [ ] **Step 4:** CI: ha-tests. Expected: PASS.
- [ ] **Step 5:** Commit `feat(ha): weather rules and daily adjustment push`.

---

### Task 14: AI advisor using any installed AI integration

**Files:**
- Create: `packages/irrigation/irrigation_ai.yaml`
- Test: `tests/ha/test_ai_pkg.py`

**Interfaces:**
- **Script `irrigation_ai_advice`** (`response_variable`). Fields: `decision` (the evaluate response) and `forecast` (the list). It returns `{final: {skip, pct, reason}, advice: {summary, recommended_pct, program_suggestions, warnings} | null}`.
- **Prompt:**
  - It is built from `sensor.irrigation_config_backup.config` (enabled zones: name, head, soil, sun, plant, area, precipitation), `sensor.irrigation_run_history.records` (7-day litres and minutes per zone), the 5-day forecast table and the rule decision.
  - It ends with the sentence: "Return a summary of at most 80 words for a home gardener in Australia, a recommended run-time percentage, suggestions per program, and warnings. Do not recommend watering during a rule-based skip."
- **Provider**, chosen by `input_select.irrigation_ai_mode`:
  - `Off`: no call.
  - `AI Task`: `ai_task.generate_data` with `entity_id` from `input_text.irrigation_ai_entity`, `task_name: irrigation_daily_advice`, and a `structure` of `summary` (text, required), `recommended_pct` (number 0–200), `program_suggestions` (list of `{program, suggestion}`) and `warnings` (list). This works with every integration that provides an AI Task entity, e.g. OpenAI, Google Gemini, Anthropic and Ollama.
  - `Conversation agent`: `conversation.process` with `agent_id` from the helper. The prompt adds `Reply with only a JSON object with keys summary, recommended_pct, program_suggestions, warnings.` The reply's `response.speech.plain.speech` is parsed with `from_json(default=none)`. This covers add-on or local agents without AI Task support.
  - Any error (`continue_on_error: true`), or a parse failure, gives `advice: null`.
- **Validation:**
  - The summary must be a string, truncated to 400 characters.
  - `recommended_pct` is dropped unless it is a number in 0..200; 95.0 → 95.
- **Combine:** a rule skip is returned unchanged. If auto-apply is off or there is no pct, the decision is unchanged. Otherwise pct = `clamp(ai, pct-30, min(pct+30, ai_max))` and the reason is `""`.
- It fires `irrigation_advice`, read by trigger template `sensor.irrigation_advice`: state = the first 255 characters of the summary; attributes the full summary, suggestions, warnings, generated_at and provider.

- [ ] **Step 1: Write the failing tests:**
  - `test_ai_task_valid`
  - `test_conversation_agent_json_parsed`
  - `test_conversation_agent_non_json_gives_null`
  - `test_missing_summary_null`
  - `test_pct_out_of_range_dropped` (250, "abc", 95.0 → 95)
  - `test_never_overrides_skip`
  - `test_bounds`: 100 with AI 180, max 150 → 130; AI 40 → 70; 130 with AI 170 → 150
  - `test_auto_apply_off_unchanged`
  - `test_mode_off_no_call`
  - `test_provider_error_null`
  - `test_prompt_contains_zone_and_forecast`
- [ ] **Step 2:** CI: ha-tests. Expected: FAIL.
- [ ] **Step 3:** Write the package file.
- [ ] **Step 4:** CI: ha-tests. Expected: PASS, including Task 13's `test_daily_adjust_pushes`.
- [ ] **Step 5:** Commit `feat(ha): ai advisor for any ai_task or conversation agent`.

---

### Task 15: Zone advisor (cycle and soak suggestions)

**Files:**
- Create: `packages/irrigation/irrigation_zone_advisor.yaml`
- Test: `tests/ha/test_zone_advisor_pkg.py`

**Interfaces:**
- **Script `irrigation_suggest_zone`** (`response_variable`). Fields: `zone` (1–16) and `apply` (bool, default false). It returns `{max_cycle_min, soak_min}`.
- Inputs come from the zone's soil, slope and precipitation in `sensor.irrigation_config_backup.config`. The script calls `irrigation_backup_config` first so the data is fresh.
- Tables and formula:
  - Intake (mm/h): SAND 50, LOAMY_SAND 30, SANDY_LOAM 25, LOAM 13, CLAY_LOAM 8, CLAY 5.
  - Slope factor: <5 % → 1.0, <8 % → 0.8, ≤12 % → 0.6, >12 % → 0.4.
  - `cycle = max(3, floor(intake × factor / precip × 60))`; `soak = clamp(cycle × 2, 20, 60)`. If cycle ≥ 60, return `(0, 0)`.
- When `apply` is true, it calls `set_zone` with the current zone values and the new `max_cycle_min` and `soak_min`.

- [ ] **Step 1: Write the failing tests:**
  - LOAM/0/40 → (19, 38)
  - CLAY/10/40 → (4, 20)
  - LOAM/0/12 → (0, 0)
  - CLAY/20/50 → (3, 20)
  - `test_apply_calls_set_zone`
- [ ] **Step 2:** CI: ha-tests. Expected: FAIL.
- [ ] **Step 3:** Write the package file.
- [ ] **Step 4:** CI: ha-tests. Expected: PASS.
- [ ] **Step 5:** Commit `feat(ha): zone cycle/soak advisor script`.

---

### Task 16: Water usage and cost

**Files:**
- Create: `packages/irrigation/irrigation_usage.yaml`
- Test: `tests/ha/test_usage_pkg.py`

**Interfaces:**
- **Total water** (`utility_meter` on `sensor.irrigation_total_water`, from Task 7): `irrigation_water_today` (daily), `irrigation_water_month` (monthly), `irrigation_water_year` (yearly). These are the entity ids the dashboard uses.
- **Per zone:** `irrigation_zone_N_water_month` (monthly) for N = 1..16, on `sensor.irrigation_zone_N_water`.
- **Template cost sensors:** `sensor.irrigation_cost_today`, `_month` and `_year` = litres × `input_number.irrigation_tariff_per_kl` / 1000, rounded to 2 dp, `device_class: monetary`, unit `$`.
  - A tariff of 0 gives cost 0.00. There is no fallback, which fixes design §2.3 issue 3.
- The device counters are `total_increasing`, so HA handles device reboots correctly. Water totals do not use the run log.

- [ ] **Step 1: Write the failing tests:**
  - `test_cost_rounding`: 1234.5 L at 3.00 → 3.70
  - `test_zero_tariff_is_zero`
  - `test_sixteen_zone_meters` (static config check)
  - `test_meters_use_total_increasing_source`
- [ ] **Step 2:** CI: ha-tests. Expected: FAIL.
- [ ] **Step 3:** Write the package file.
- [ ] **Step 4:** CI: ha-tests. Expected: PASS.
- [ ] **Step 5:** Commit `feat(ha): water usage and cost sensors`.

---

### Task 17: Notifications

**Files:**
- Create: `packages/irrigation/irrigation_notify.yaml`
- Test: `tests/ha/test_notify_pkg.py`

**Interfaces:**
- **Script `irrigation_notify`.** Fields: `category`, `title`, `message`.
  - It sends only if `input_boolean.irrigation_notify_<category>` is on.
  - Non-critical categories are suppressed between `irrigation_quiet_start` and `irrigation_quiet_end`, a window that may wrap past midnight. `CRITICAL = flow_high, leak, max_runtime`.
  - For each name in `irrigation_notify_targets` it calls `notify.<name>` with `{title, message}`. Critical messages add `data: {push: {interruption-level: critical}}` for `mobile_app_*` targets.
- **Automation `irrigation_notifications`** maps events to categories. Titles and messages are the **exact** copy from design §8, rendered in Jinja.
  - `irrigation_run_started` → `run_started`; `irrigation_program_finished` → `run_finished` (cost = `l` × tariff / 1000); `irrigation_skipped` → `skipped`
  - `irrigation_alert` → its `kind`: `flow_high`, `flow_low`, `leak`, `max_runtime` or `clock_invalid`
  - `irrigation_advice` → `daily_advice`
  - `sensor.irrigation_status` → `unavailable` held `for: minutes: "{{ states('input_number.irrigation_offline_minutes')|int }}"` → `offline`. This fires once per outage.
  - Zone names come from `sensor.irrigation_config_backup`; the fallback is `Zone N`.

- [ ] **Step 1: Write the failing tests:**
  - `test_flow_high_copy`: exact `"Front Lawn flow 18.2 L/min is above 13.0 L/min. Zone stopped."`
  - `test_quiet_hours_wrap` (23:30 is quiet, 07:30 is not)
  - `test_critical_ignores_quiet_hours`
  - `test_category_disabled_not_sent`
  - `test_offline_once_per_outage`
  - `test_run_finished_uses_program_totals`
  - `test_multiple_targets`
- [ ] **Step 2:** CI: ha-tests. Expected: FAIL.
- [ ] **Step 3:** Write the package file.
- [ ] **Step 4:** CI: ha-tests. Expected: the whole suite PASSES.
- [ ] **Step 5:** Commit `feat(ha): configurable notifications`.

---

### Task 18: SVG site plan and floorplan rules

**Files:**
- Create: `www/irrigation/siteplan.svg`, `www/irrigation/siteplan.css`, `floorplan/irrigation-floorplan.yaml`
- Modify: `docs/install.md` (site-plan section)
- Test: `tests/ha/test_siteplan.py`

**Interfaces:**
- **SVG** (viewBox `0 0 1600 1000`):
  - Layers: `<g id="layer-background">` with `<image id="satellite" href="satellite.jpg">`, `<g id="layer-zones">`, `<g id="layer-heads">` and `<g id="layer-controllers">`.
  - The template has `controller-1`, zones `zone-01`..`zone-06` (polygons with `data-zone="1"`) and 2 sample heads per zone. Head ids are `head-NN-MM`, with classes in `head-spray|head-rotor|head-rotator|head-drip|head-bubbler`.
  - Legend group `legend` shows one symbol per head type: circle = spray, circle with an arc = rotor, triangle = rotator, dashed line = drip, square = bubbler.
- **CSS** classes:
  - Zone state: `zone-idle` (opacity .25), `zone-running` (blue pulse animation), `zone-disabled` (grey hatch), `zone-alarm` (red).
  - Head colours: one per type.
- **Floorplan rules:**
  - Each `zone-NN` maps to `switch.irrigation_zone_N`. Its class is `zone-alarm` if `binary_sensor.irrigation_flow_alarm` is on and the active zone equals N; `zone-running` if on; otherwise `zone-idle`.
  - Tapping a zone calls `esphome.irrigation_controller_run_zone` with `zone: N, minutes: 5` after a confirmation.
  - `controller-1` shows `sensor.irrigation_status` as a hover tooltip.
- `install.md` documents the Inkscape workflow: import a Google Maps satellite screenshot as `satellite.jpg`, draw polygons on `layer-zones`, duplicate heads, set ids and classes in the XML editor, and copy the files to `/config/www/irrigation/`.

- [ ] **Step 1: Write the failing tests** (they parse the SVG with `xml.etree`):
  - `test_required_layers`
  - `test_zone_ids_pattern_and_data_zone`
  - `test_head_ids_and_classes_valid`
  - `test_floorplan_rules_cover_every_zone_in_svg`: the YAML lists a rule for every `zone-NN` id present.
- [ ] **Step 2:** CI: ha-tests. Expected: FAIL.
- [ ] **Step 3:** Create the files. CI: ha-tests (the siteplan tests parse the SVG, so malformed XML fails them).
- [ ] **Step 4:** CI: ha-tests. Expected: PASS.
- [ ] **Step 5:** Commit `feat(ui): svg site plan and floorplan rules`.

---

### Task 19: Ultra Card dashboard, install guide, end-to-end acceptance

**Files:**
- Create: `dashboards/irrigation-dashboard.yaml`; complete `docs/install.md`; create `docs/acceptance.md`
- Test: `tests/ha/test_dashboard.py`

**Interfaces:**
- **Dashboard views**, in order (paths are exact):
  - `overview`
    - Ultra Card: status, active zone with countdown (`sensor.irrigation_remaining_time`), flow, `Irrigation Water Today`, `Irrigation Cost Today`, next run, weather decision, advice summary.
    - Quick action buttons: Stop All, Rain Delay 24h, Run Program A.
    - Alarm banner, conditional on `binary_sensor.irrigation_leak` / `irrigation_flow_alarm` / `irrigation_rain_hold`.
  - `siteplan`: a `custom:floorplan-card` panel using Task 18 files.
  - `calendar`:
    - A native calendar card on `calendar.irrigation` (the user's Local Calendar, written by Task 12), in list and month views.
    - Beside it, an "Upcoming watering" list rendered from `sensor.irrigation_upcoming.attributes.runs`.
  - `programs`: Ultra Card containing the program editor entities, with conditional visibility:
    - day switches only when Day Mode = Weekdays
    - interval numbers only when Every N days
    - zone-minute rows only for zones ≤ Zone Count
  - `zones`: the zone editor, with Learn Flow and Test Run buttons.
    - A "Suggest cycle & soak" button calls `script.irrigation_suggest_zone` with `apply: false` and shows the response.
    - An "Apply suggestion" button calls it with `apply: true`.
  - `water`:
    - statistics-graph of `sensor.irrigation_total_water` (day/week/month)
    - the `sensor.irrigation_water_today/month/year` and `sensor.irrigation_cost_today/month/year` sensors
    - a per-zone month bar from `sensor.irrigation_zone_N_water_month`
  - `settings`:
    - monthly adjust, flow, rain, zone count
    - a Hardware section: Zone 1–16 Output, Master Output, Relay Trigger Level, Last Error
    - a Buttons section: Start/Stop short and long press functions, Button Program
    - controller enabled
    - Weather & AI: weather entity, rain observation entity, AI mode, AI entity, rule thresholds, auto-apply, AI max %
    - Notifications: targets, category toggles, quiet hours
    - tariff
    - Backup config and Restore config buttons (`script.irrigation_backup_config` / `script.irrigation_restore_config`)
- Build the Ultra Card sections in the Ultra Card visual editor, then paste the exported YAML into the file. No Ultra Card schema is hand-written.

- [ ] **Step 1:** Write `test_dashboard.py`:
  - `test_views_present`: the 7 paths above.
  - `test_referenced_entities_exist`: every `irrigation_*` entity or script id in the YAML is in an allow-list. The allow-list is built from two sources:
    - the Task 7/8 device entity names, using the slug rule in Global Constraints
    - the entities and scripts defined in `packages/irrigation/*.yaml`, parsed by the test
  - CI: ha-tests. Expected: FAIL.
- [ ] **Step 2:** Create the dashboard. CI: ha-tests. Expected: PASS.
- [ ] **Step 3:** Finish `docs/install.md`, the detailed companion to `INSTALL_CHECKLIST.md`:
  - **Device Builder:** create the device from the wrapper, fill in secrets, do the first USB flash with "Plug into this computer", and allow the device to perform Home Assistant actions.
  - **Packages:** copy them in, add the `configuration.yaml` line, and restart once.
  - **Calendar:** create the Local Calendar named `Irrigation`.
  - **HACS:** install the BOM integration, Ultra Card and ha-floorplan.
  - **AI:** use any AI Task-capable integration already installed (e.g. OpenAI, Google Gemini, Anthropic, Ollama) or any conversation agent. Set AI mode and entity in Settings. Explain how to find an `ai_task.*` entity id or a conversation agent id.
  - **Dashboard:** import it, then press Backup config.
- [ ] **Step 4:** Run `docs/acceptance.md` on the installed system, recording each result:
  1. Create Program A in the dashboard (Mon/Wed/Fri 06:00, zones 1–3) with no reflash or restart. It appears in Upcoming watering within 1 min.
  2. Running `automation.irrigation_daily_adjust` pushes a decision. `Weather Adjust` on the device matches `Irrigation Weather Decision`.
  3. Set the rain threshold to 0 mm and run the daily adjust. The next start is logged as skipped, and a notification and a calendar "skipped — weather" event appear.
  4. With AI mode set to the user's AI integration, `sensor.irrigation_advice` gets a summary. Switching to a conversation agent also works.
  5. Stop HA for 1 h over a start time, using a host restart or HA Core stop. The program runs. After HA starts, the calendar shows the run and the usage sensors include its litres.
  6. A leak simulation raises a critical notification during quiet hours.
  7. The site plan zone turns blue while running.
  8. The dashboard has no "custom element doesn't exist" errors.
- [ ] **Step 5:** Commit `feat(ui): dashboard, install guide, acceptance results`.

---

### Task 20: GitHub release packaging (README, install checklist, release workflow, v0.1.0)

**Files:**
- Create: `INSTALL_CHECKLIST.md`, `CHANGELOG.md`, `.github/workflows/release.yml`, `.github/ISSUE_TEMPLATE/bug_report.yml`, `.github/ISSUE_TEMPLATE/feature_request.yml`, `esphome/irrigation-controller/VERSION`, `tools/build_release_zip.py`, `images/screenshots/*.png`
- Modify: `README.md` (full content)
- Test: `tests/ha/test_release.py`

**Interfaces:**
- Consumes: every earlier task's files, plus the Task 9 and Task 19 results for the screenshots.
- **`README.md` sections**, in order (ESPyRain's README structure):
  1. Header image `images/screenshots/header.png`.
  2. Title, then the two design goals quoted from the requirements: fully configurable through HA with no YAML reload, HA restart or ESP reflash; and autonomous scheduled watering when HA or Wi-Fi is unavailable.
  3. What it is: ESPHome firmware (built in ESPHome Device Builder) plus HA YAML packages. It needs no Python, add-ons or custom integration.
  4. A comparison with ESPHome Sprinkler, Irrigation Unlimited and ESPyRain.
  5. Key features, grouped as Configuration (including relay pins set in HA), Scheduling, Weather & AI ("works with any AI Task or conversation agent you already use"), Flow & water cost, Rain sensor, Calendar, Site plan, Notifications, Dashboards. Each group has a screenshot.
  6. Hardware: FireBeetle 2 ESP32-C6, KS0011 relays, flow meter, rain sensor, Start/Stop buttons, optional status LED, DS3231 and MCP23017; the pin table from design §4; a wiring photo.
  7. Quick start, linking to `INSTALL_CHECKLIST.md`.
  8. Updating: bump `version` in the wrapper and click Install in Device Builder; copy the new `packages/irrigation/` and restart.
  9. Offline behaviour.
  10. Licence (Apache-2.0) and credits, linking ESPyRain_Controller, the ESPHome Sprinkler component and Irrigation Unlimited.
- **`INSTALL_CHECKLIST.md`** is modelled on ESPyRain's checklist. Each step ends with a "Check:" line.
  1. Download the ZIP from the latest GitHub release.
  2. Copy the folders with the File editor or Samba add-on: `packages/irrigation/` → `/config/packages/irrigation/` and `www/irrigation/` → `/config/www/irrigation/`. For a local firmware install, also copy `esphome/irrigation-controller/` → `/config/esphome/irrigation-controller/`. Show the final `/config` tree.
  3. Add `homeassistant: packages: !include_dir_named packages/irrigation` to `configuration.yaml`, check the config and restart HA. This is the only restart; later changes need none.
  4. Create the Local Calendar integration named `Irrigation`.
  5. In ESPHome Device Builder:
     - Create a new device and paste in `esphome/irrigation-controller.yaml`.
     - Fill in Device Builder's Secrets editor from `secrets.yaml.example`.
     - Click Install → "Plug into this computer" for the first USB flash; after that it updates wirelessly.
     - Adopt the device in HA and enable "Allow the device to perform Home Assistant actions".
  6. Install with HACS: the BOM integration, Ultra Card and ha-floorplan.
  7. Optional: pick your AI. Set `AI mode` and `AI entity` in the dashboard's Settings to any AI Task entity or conversation agent you already have.
  8. Import the dashboard (raw configuration editor). Set the weather entity, notify targets and relay outputs in Settings. Press Backup config.
- **`release.yml`**, triggered on a pushed tag `v*`:
  - Runs `pytest tests/ha/test_release.py` and fails if the tag ≠ `v` + `VERSION`.
  - Builds `esphome_irrigation_controller-<version>.zip` with `tools/build_release_zip.py`. The ZIP contains `esphome/` (without secrets or `.esphome/`), `packages/`, `www/`, `dashboards/`, `floorplan/`, `INSTALL_CHECKLIST.md` and `LICENSE`.
  - Creates the GitHub release with `softprops/action-gh-release@v2`, using the notes from the matching `CHANGELOG.md` section, and attaches the ZIP.

- [ ] **Step 1: Write the failing tests** in `test_release.py`:
  - `test_versions_match`: `VERSION`, `esphome/irrigation-controller/VERSION`, the wrapper's `version` substitution and the `irrigation_package_version` initial value are all equal.
  - `test_readme_sections`: every heading listed above is present, and every `images/...` link resolves.
  - `test_checklist_paths_exist`: every repo path the checklist tells the user to copy exists.
  - `test_zip_excludes_secrets`: build into a temp dir; there is no `secrets.yaml`, `.esphome/` or `reference_docs/`.
  - Then CI: ha-tests. Expected: FAIL.
- [ ] **Step 2:** Write the files. Take the screenshots from the Task 19 installed dashboard and the Task 9 bench rig.
- [ ] **Step 3:** CI: host-tests, ha-tests and esphome. Expected: all green.
- [ ] **Step 4:** Commit `docs: readme, install checklist and release workflow`, then merge `dev` into `main` and push. Expected: CI on `main` is green.
- [ ] **Step 5:** Release v0.1.0. **Ask the user to confirm first:** this publishes a public release.
  - Run `git tag v0.1.0 && git push origin v0.1.0`.
  - Run `gh release view v0.1.0`. Expected: the release exists with the ZIP attached.
- [ ] **Step 6:** Test installing from the release on the user's HA (HA OS 2026.10.4, Raspberry Pi 4):
  1. Follow `INSTALL_CHECKLIST.md` from the ZIP on a clean `packages/irrigation`.
  2. Switch the wrapper to Option B (remote packages from the `v0.1.0` tag) and click Install in Device Builder. Expected: the build succeeds from GitHub with no local component files.
  3. Record both results in `docs/acceptance.md`.
