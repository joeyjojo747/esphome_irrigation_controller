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


async def _set_zone(hass, zone, out):
    await hass.services.async_call(
        "esphome", "irrigation_controller_set_zone",
        {"zone": zone, "zone_output": out}, blocking=True)


async def test_device_set_zone_duplicate_rules(hass, device):
    import pytest
    from homeassistant.exceptions import HomeAssistantError

    await _set_zone(hass, 1, "A")
    with pytest.raises(HomeAssistantError):
        await _set_zone(hass, 2, "A")
    await _set_zone(hass, 1, "")  # empty leaves the output unchanged: claim kept
    with pytest.raises(HomeAssistantError):
        await _set_zone(hass, 2, "A")
    await _set_zone(hass, 1, "None")  # releases
    await _set_zone(hass, 2, "A")
    await _set_zone(hass, 2, "B")  # moving A -> B releases A
    await _set_zone(hass, 1, "A")


async def test_stub_side_effect_and_respond_guard(hass, mocks, device):
    import pytest

    with pytest.raises(AssertionError):
        device.respond("run_zone", {})
    mocks.notify.side_effect = [None, RuntimeError("down")]
    await hass.services.async_call("notify", "mobile_app_test", {"message": "a"}, blocking=True)
    with pytest.raises(RuntimeError):
        await hass.services.async_call("notify", "mobile_app_test", {"message": "b"}, blocking=True)
    assert len(mocks.notify.calls) == 2
