from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

CATEGORIES = ["run_started", "run_finished", "skipped", "flow_high", "flow_low",
              "leak", "max_runtime", "offline", "clock_invalid", "daily_advice"]


async def test_helpers_defaults(pkg_hass):
    s = pkg_hass.states
    assert float(s.get("input_number.irrigation_rain_skip_mm").state) == 3.0
    assert s.get("input_select.irrigation_ai_mode").state == "Off"
    assert s.get("input_boolean.irrigation_notify_run_started").state == "off"
    assert s.get("input_boolean.irrigation_notify_run_finished").state == "on"
    assert s.get("input_text.irrigation_weather_entity").state == "weather.home"
    assert s.get("input_datetime.irrigation_quiet_start").state == "22:00:00"


async def test_every_notify_category_has_toggle(pkg_hass):
    assert len(CATEGORIES) == 10
    for cat in CATEGORIES:
        assert pkg_hass.states.get(f"input_boolean.irrigation_notify_{cat}") is not None, cat


async def test_package_version_matches(pkg_hass):
    version = (ROOT / "VERSION").read_text().strip()
    assert pkg_hass.states.get("input_text.irrigation_package_version").state == version
