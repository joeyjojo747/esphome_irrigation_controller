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


# ---- C-1: helpers restore their last value; defaults are seeded once ----

HELPER_DOMAINS = ("input_text", "input_select", "input_datetime", "input_number", "input_boolean")
SEED = "automation.irrigation_seed_defaults"
FLAG = "input_boolean.irrigation_defaults_applied"


def _helpers_pkg():
    from homeassistant.util.yaml import load_yaml

    return load_yaml(str(ROOT / "packages" / "irrigation" / "irrigation_helpers.yaml"))


def test_no_helper_has_initial():
    # `initial` makes HA ignore the restored state, so every setting reset on restart.
    pkg = _helpers_pkg()
    seen = 0
    for domain in HELPER_DOMAINS:
        for key, cfg in (pkg.get(domain) or {}).items():
            seen += 1
            if key == "irrigation_package_version":
                assert "initial" in cfg  # always the installed version
                continue
            assert "initial" not in cfg, f"{domain}.{key}"
    assert seen >= 39


def test_seed_runs_on_homeassistant_start_only_when_flag_off():
    autos = {a["id"]: a for a in _helpers_pkg()["automation"]}
    seed = autos["irrigation_seed_defaults"]
    assert {"trigger": "homeassistant", "event": "start"} in seed["triggers"]
    assert {"condition": "state", "entity_id": FLAG, "state": "off"} in seed["conditions"]
    assert seed["actions"] == [{"action": "script.irrigation_apply_defaults"}]


async def _run_seed(hass):
    assert hass.states.get(SEED) is not None, SEED
    # skip_condition False: the flag condition is evaluated as on a real start.
    await hass.services.async_call(
        "automation", "trigger", {"entity_id": SEED, "skip_condition": False}, blocking=True)
    await hass.async_block_till_done()


async def _set_number(hass, name, value):
    await hass.services.async_call(
        "input_number", "set_value",
        {"entity_id": f"input_number.irrigation_{name}", "value": value}, blocking=True)


async def test_seed_with_flag_on_keeps_changed_values(pkg_hass):
    hass = pkg_hass
    assert hass.states.get(FLAG).state == "on"
    await _set_number(hass, "rain_skip_mm", 7.5)
    await _set_number(hass, "last_ts", 123456)
    await _set_number(hass, "last_seq", 9)
    await hass.services.async_call(
        "input_text", "set_value",
        {"entity_id": "input_text.irrigation_notify_targets", "value": "notify.mobile_app_test"},
        blocking=True)
    await hass.services.async_call(
        "input_boolean", "turn_on", {"entity_id": "input_boolean.irrigation_notify_run_started"},
        blocking=True)
    await _run_seed(hass)
    s = hass.states
    assert float(s.get("input_number.irrigation_rain_skip_mm").state) == 7.5
    assert float(s.get("input_number.irrigation_last_ts").state) == 123456
    assert float(s.get("input_number.irrigation_last_seq").state) == 9
    assert s.get("input_text.irrigation_notify_targets").state == "notify.mobile_app_test"
    assert s.get("input_boolean.irrigation_notify_run_started").state == "on"


async def test_seed_with_flag_off_applies_defaults_and_sets_flag(pkg_hass):
    hass = pkg_hass
    await hass.services.async_call("input_boolean", "turn_off", {"entity_id": FLAG}, blocking=True)
    await _set_number(hass, "rain_skip_mm", 7.5)
    await _set_number(hass, "tariff_per_kl", 9.99)
    await _set_number(hass, "last_ts", 123456)
    await hass.services.async_call(
        "input_select", "select_option",
        {"entity_id": "input_select.irrigation_ai_mode", "option": "AI Task"}, blocking=True)
    await hass.services.async_call(
        "input_datetime", "set_datetime",
        {"entity_id": "input_datetime.irrigation_quiet_start", "time": "21:30:00"}, blocking=True)
    await hass.services.async_call(
        "input_boolean", "turn_off", {"entity_id": "input_boolean.irrigation_notify_leak"},
        blocking=True)
    await _run_seed(hass)
    s = hass.states
    assert float(s.get("input_number.irrigation_rain_skip_mm").state) == 3.0
    assert float(s.get("input_number.irrigation_tariff_per_kl").state) == 3.0
    assert float(s.get("input_number.irrigation_last_ts").state) == 0  # first run seeds 0
    assert s.get("input_select.irrigation_ai_mode").state == "Off"
    assert s.get("input_datetime.irrigation_quiet_start").state == "22:00:00"
    assert s.get("input_boolean.irrigation_notify_leak").state == "on"
    assert s.get(FLAG).state == "on"


async def test_apply_defaults_keeps_watermark_once_applied(pkg_hass):
    # Running the reset script by hand restores the settings, not the replay watermark.
    hass = pkg_hass
    await _set_number(hass, "last_ts", 123456)
    await _set_number(hass, "last_seq", 9)
    await _set_number(hass, "rain_skip_mm", 7.5)
    await hass.services.async_call("script", "irrigation_apply_defaults", {}, blocking=True)
    s = hass.states
    assert float(s.get("input_number.irrigation_rain_skip_mm").state) == 3.0
    assert float(s.get("input_number.irrigation_last_ts").state) == 123456
    assert float(s.get("input_number.irrigation_last_seq").state) == 9


async def test_seeded_defaults_match_previous_initials(pkg_hass):
    # Every default the helpers used to carry as `initial` is seeded by the script.
    s = pkg_hass.states
    expect = {
        "input_text.irrigation_weather_entity": "weather.home",
        "input_text.irrigation_rain_obs_entity": "",
        "input_text.irrigation_ai_entity": "",
        "input_text.irrigation_notify_targets": "",
        "input_select.irrigation_ai_mode": "Off",
        "input_datetime.irrigation_eval_time": "03:00:00",
        "input_datetime.irrigation_quiet_start": "22:00:00",
        "input_datetime.irrigation_quiet_end": "07:00:00",
        "input_boolean.irrigation_wind_enabled": "off",
        "input_boolean.irrigation_ai_auto_apply": "off",
        "input_boolean.irrigation_notify_run_started": "off",
    }
    for cat in CATEGORIES[1:]:
        expect[f"input_boolean.irrigation_notify_{cat}"] = "on"
    for eid, want in expect.items():
        assert s.get(eid).state == want, eid
    numbers = {"rain_skip_mm": 3.0, "rain_prob_pct": 80, "rain_prob_mm": 2.0, "cumulative_days": 3,
               "cumulative_mm": 15, "freeze_c": 2, "wind_kmh": 30, "cool_c": 18, "cool_pct": 70,
               "hot_c": 32, "hot_pct": 130, "valid_hours": 30, "ai_max_pct": 150,
               "tariff_per_kl": 3.0, "offline_minutes": 30, "last_ts": 0, "last_seq": 0}
    for name, want in numbers.items():
        assert float(s.get(f"input_number.irrigation_{name}").state) == want, name


async def _start_with_helpers(hass, restored):
    """Set up the helper package as on an HA start, with `restored` in the restore cache."""
    from homeassistant.core import CoreState, State
    from homeassistant.setup import async_setup_component
    from pytest_homeassistant_custom_component.common import mock_restore_cache

    hass.set_state(CoreState.not_running)
    mock_restore_cache(hass, [State(eid, st) for eid, st in restored.items()])
    pkg = _helpers_pkg()
    for domain in HELPER_DOMAINS + ("script", "automation"):
        assert await async_setup_component(hass, domain, {domain: pkg[domain]}), domain
    await hass.async_block_till_done()
    await hass.async_start()
    await hass.async_block_till_done()


async def test_restart_restores_edited_settings(hass):
    restored = {
        FLAG: "on",
        "input_number.irrigation_rain_skip_mm": "7.5",
        "input_number.irrigation_last_ts": "123456",
        "input_text.irrigation_notify_targets": "notify.mobile_app_test",
        "input_select.irrigation_ai_mode": "AI Task",
        "input_boolean.irrigation_notify_leak": "off",
    }
    await _start_with_helpers(hass, restored)
    for eid, want in restored.items():
        st = hass.states.get(eid).state
        if eid.startswith("input_number."):
            assert float(st) == float(want), eid
        else:
            assert st == want, eid


async def test_first_start_seeds_defaults(hass):
    await _start_with_helpers(hass, {})
    s = hass.states
    assert s.get(FLAG).state == "on"
    assert float(s.get("input_number.irrigation_rain_skip_mm").state) == 3.0
    assert s.get("input_text.irrigation_weather_entity").state == "weather.home"
    assert s.get("input_datetime.irrigation_eval_time").state == "03:00:00"
    assert s.get("input_boolean.irrigation_notify_run_finished").state == "on"
    assert float(s.get("input_number.irrigation_last_ts").state) == 0
