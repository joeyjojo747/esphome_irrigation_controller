"""Configurable notifications: copy, categories, quiet hours, targets, offline."""

from datetime import datetime, timedelta

from homeassistant.util import dt as dt_util
from pytest_homeassistant_custom_component.common import async_fire_time_changed

import pytest

from helpers import make_config, settle

TARGETS = "input_text.irrigation_notify_targets"


@pytest.fixture(autouse=True)
def daytime(freezer):
    """Outside the default 22:00-07:00 quiet hours unless a test moves the clock."""
    freezer.move_to(local_time(12, 0))


def local_time(hh, mm):
    return dt_util.as_utc(datetime(2026, 11, 5, hh, mm, tzinfo=dt_util.DEFAULT_TIME_ZONE))


async def setup_targets(hass, value="mobile_app_test"):
    await hass.services.async_call(
        "input_text", "set_value", {"entity_id": TARGETS, "value": value}, blocking=True)


async def set_bool(hass, category, on):
    await hass.services.async_call(
        "input_boolean", "turn_on" if on else "turn_off",
        {"entity_id": f"input_boolean.irrigation_notify_{category}"}, blocking=True)


async def backup(hass, cfg=None):
    hass.bus.async_fire("irrigation_config_backup", {"config": cfg or make_config()})
    await settle(hass)


def named_config():
    cfg = make_config()
    cfg["zones"][0]["name"] = "Front Lawn"
    cfg["zones"][0]["action"] = 1
    cfg["zones"][1]["name"] = ""
    cfg["programs"][1]["name"] = "Lawn"
    cfg["programs"][0]["name"] = ""
    return cfg


def skipped_event(**kw):
    d = {"seq": 1, "inst": 0, "p": 0, "z": 255, "ts": 1_700_000_000, "plan": 0,
         "act": 0, "l": 0, "r": "skipped_weather"}
    d.update(kw)
    return d


async def test_flow_high_copy(pkg_hass, mocks):
    hass = pkg_hass
    await setup_targets(hass)
    await backup(hass, named_config())
    hass.bus.async_fire("irrigation_alert", {"kind": "flow_high", "z": 0, "flow": 18.2, "limit": 13.0})
    await settle(hass)
    assert len(mocks.notify.calls) == 1
    c = mocks.notify.calls[0]
    assert c["title"] == "High flow — Front Lawn"
    assert c["message"] == "Front Lawn flow 18.2 L/min is above 13.0 L/min. Zone stopped."
    assert c["data"] == {"push": {"interruption-level": "critical"}}


async def test_alert_copy_for_other_kinds(pkg_hass, mocks):
    hass = pkg_hass
    await setup_targets(hass)
    await backup(hass, named_config())
    cases = [
        ({"kind": "flow_high", "z": 1, "flow": 18.2, "limit": 13.0},
         "High flow — Zone 2", "Zone 2 flow 18.2 L/min is above 13.0 L/min. Alert only."),
        ({"kind": "flow_low", "z": 0, "flow": 3.1, "limit": 6.0},
         "Low flow — Front Lawn",
         "Front Lawn flow 3.1 L/min is below 6.0 L/min. Check the valve, filter or supply."),
        ({"kind": "leak", "z": 255, "flow": 2.5, "limit": 1.5},
         "Possible leak", "2.5 L/min flowing with no zone running."),
        # the firmware reports the max_runtime limit in seconds
        ({"kind": "max_runtime", "z": 0, "flow": 9.0, "limit": 3600.0},
         "Zone stopped — runtime limit", "Front Lawn ran past its 60 min limit and was stopped."),
        ({"kind": "clock_invalid", "z": 255, "flow": 0.0, "limit": 0.0},
         "Irrigation clock not set",
         "Scheduled watering is paused until the controller clock is set."),
    ]
    for data, _, _ in cases:
        hass.bus.async_fire("irrigation_alert", data)
        await settle(hass)
    got = [(c["title"], c["message"]) for c in mocks.notify.calls]
    assert got == [(t, m) for _, t, m in cases]


async def test_other_event_copy(pkg_hass, mocks):
    hass = pkg_hass
    await setup_targets(hass)
    await set_bool(hass, "run_started", True)
    await backup(hass, named_config())
    hass.bus.async_fire("irrigation_program_started", {
        "inst": 1, "p": 1, "zones": 4, "plan": 2700, "pct": 80, "ts": 1_700_000_000})
    hass.bus.async_fire("irrigation_skipped", skipped_event(r="skipped_rain_sensor"))
    hass.bus.async_fire("irrigation_advice", {
        "advice": {"summary": "Hot and dry this week.", "recommended_pct": 120,
                   "program_suggestions": [], "warnings": []},
        "provider": "ai_task"})
    await settle(hass)
    got = sorted((c["title"], c["message"]) for c in mocks.notify.calls)
    assert got == sorted([
        ("Irrigation started", "Lawn started: 4 zones, about 45 min (80%)."),
        ("Irrigation skipped", "Program A skipped: rain sensor."),
        ("Irrigation outlook", "Hot and dry this week."),
    ])


async def test_run_finished_uses_program_totals(pkg_hass, mocks):
    hass = pkg_hass
    await setup_targets(hass)
    hass.bus.async_fire("irrigation_program_finished", {
        "inst": 1, "p": 0, "zones": 3, "act": 2520, "l": 610.0, "ts": 1_700_000_000, "warn": False})
    await settle(hass)
    assert len(mocks.notify.calls) == 1
    c = mocks.notify.calls[0]
    assert c["title"] == "Irrigation finished"
    # 610 L x $3.00/kL = $1.83
    assert c["message"] == "Program A finished: 42 min, 610 L, $1.83."
    # a per-step record must not notify
    hass.bus.async_fire("irrigation_run_finished", {
        "seq": 1, "inst": 1, "p": 0, "z": 0, "ts": 1_700_000_000, "plan": 600,
        "act": 600, "l": 100.0, "r": "completed"})
    await settle(hass)
    assert len(mocks.notify.calls) == 1


async def test_quiet_hours_wrap(pkg_hass, mocks, freezer):
    hass = pkg_hass
    await setup_targets(hass)
    freezer.move_to(local_time(23, 30))
    hass.bus.async_fire("irrigation_skipped", skipped_event())
    await settle(hass)
    assert mocks.notify.calls == []
    freezer.move_to(local_time(7, 30))
    hass.bus.async_fire("irrigation_skipped", skipped_event())
    await settle(hass)
    assert len(mocks.notify.calls) == 1
    freezer.move_to(local_time(3, 0))  # after midnight, still quiet
    hass.bus.async_fire("irrigation_skipped", skipped_event())
    await settle(hass)
    assert len(mocks.notify.calls) == 1


async def test_critical_ignores_quiet_hours(pkg_hass, mocks, freezer):
    hass = pkg_hass
    await setup_targets(hass)
    freezer.move_to(local_time(23, 30))
    for kind in ("flow_high", "leak", "max_runtime"):
        hass.bus.async_fire("irrigation_alert", {"kind": kind, "z": 0, "flow": 5.0, "limit": 60.0})
    await settle(hass)
    assert len(mocks.notify.calls) == 3
    # a non-critical alert is held back
    hass.bus.async_fire("irrigation_alert", {"kind": "flow_low", "z": 0, "flow": 1.0, "limit": 4.0})
    await settle(hass)
    assert len(mocks.notify.calls) == 3


async def test_category_disabled_not_sent(pkg_hass, mocks):
    hass = pkg_hass
    await setup_targets(hass)
    await set_bool(hass, "skipped", False)
    hass.bus.async_fire("irrigation_skipped", skipped_event())
    await settle(hass)
    assert mocks.notify.calls == []


async def test_multiple_targets(pkg_hass, mocks):
    hass = pkg_hass
    await setup_targets(hass, " mobile_app_test , ,mobile_app_test2,")
    hass.bus.async_fire("irrigation_alert", {"kind": "leak", "z": 255, "flow": 2.0, "limit": 1.5})
    await settle(hass)
    assert len(mocks.notify.calls) == 1 and len(mocks.notify2.calls) == 1
    assert mocks.notify2.calls[0]["data"] == {"push": {"interruption-level": "critical"}}
    # non-critical: no push data
    hass.bus.async_fire("irrigation_alert", {"kind": "clock_invalid", "z": 255, "flow": 0, "limit": 0})
    await settle(hass)
    assert "data" not in mocks.notify2.calls[1]


async def test_no_targets_sends_nothing(pkg_hass, mocks):
    hass = pkg_hass
    hass.bus.async_fire("irrigation_alert", {"kind": "leak", "z": 255, "flow": 2.0, "limit": 1.5})
    await settle(hass)
    assert mocks.notify.calls == [] and mocks.notify2.calls == []


async def test_offline_once_per_outage(pkg_hass, mocks, freezer):
    hass = pkg_hass
    await setup_targets(hass)
    freezer.move_to(local_time(12, 0))
    hass.states.async_set("sensor.irrigation_status", "idle")
    await settle(hass)
    hass.states.async_set("sensor.irrigation_status", "unavailable")
    await settle(hass)
    freezer.tick(timedelta(minutes=29))
    async_fire_time_changed(hass, dt_util.utcnow())
    await settle(hass)
    assert mocks.notify.calls == []
    freezer.tick(timedelta(minutes=2))
    async_fire_time_changed(hass, dt_util.utcnow())
    await settle(hass)
    assert len(mocks.notify.calls) == 1
    c = mocks.notify.calls[0]
    assert c["title"] == "Irrigation controller offline"
    assert c["message"] == "No contact for 30 min. Schedules keep running on the controller."
    freezer.tick(timedelta(hours=3))
    async_fire_time_changed(hass, dt_util.utcnow())
    await settle(hass)
    assert len(mocks.notify.calls) == 1
    # a second outage notifies again
    hass.states.async_set("sensor.irrigation_status", "idle")
    await settle(hass)
    hass.states.async_set("sensor.irrigation_status", "unavailable")
    await settle(hass)
    freezer.tick(timedelta(minutes=31))
    async_fire_time_changed(hass, dt_util.utcnow())
    await settle(hass)
    assert len(mocks.notify.calls) == 2
