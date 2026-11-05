"""Calendar history writer and the upcoming-runs sensor."""

import asyncio

from homeassistant.util import dt as dt_util
from pytest_homeassistant_custom_component.common import async_capture_events

TS = 1_700_000_000

async def settle(hass):
    """block_till_done, plus the loop.call_soon hop the event trigger platform uses."""
    for _ in range(3):
        await hass.async_block_till_done()
        await asyncio.sleep(0)


def local(ts):
    return dt_util.as_local(dt_util.utc_from_timestamp(ts)).strftime("%Y-%m-%d %H:%M:%S")


def finished(**kw):
    d = {"inst": 1, "p": 0, "zones": 3, "act": 2520, "l": 610.0, "ts": TS, "warn": False}
    d.update(kw)
    return d


async def test_finished_event_summary(pkg_hass, mocks):
    hass = pkg_hass
    hass.bus.async_fire("irrigation_program_finished", finished())
    await settle(hass)
    assert len(mocks.calendar.calls) == 1
    c = mocks.calendar.calls[0]
    assert c["summary"] == "Program A · 3 zones · 42 min · 610 L"
    assert c["start_date_time"] == local(TS)
    assert c["end_date_time"] == local(TS + 2520)


async def test_warning_marker(pkg_hass, mocks):
    hass = pkg_hass
    hass.bus.async_fire("irrigation_program_finished", finished(warn=True))
    await settle(hass)
    assert mocks.calendar.calls[0]["summary"] == "Program A · 3 zones · 42 min · 610 L ⚠"


async def test_named_program_uses_backup_name(pkg_hass, mocks):
    hass = pkg_hass
    hass.bus.async_fire("irrigation_config_backup", {"config": {
        "programs": [{"program": 0, "name": ""}, {"program": 1, "name": "Lawn"}],
        "zones": [], "settings": {}}})
    await settle(hass)
    hass.bus.async_fire("irrigation_program_finished", finished(p=1))
    hass.bus.async_fire("irrigation_program_finished", finished(p=0, ts=TS + 10000))
    await settle(hass)
    summaries = sorted(c["summary"] for c in mocks.calendar.calls)
    assert summaries[0].startswith("Lawn · 3 zones")
    assert summaries[1].startswith("Program A · 3 zones")


async def test_skipped_event_label(pkg_hass, mocks):
    hass = pkg_hass
    labels = {"skipped_rain_sensor": "rain sensor", "skipped_rain_delay": "rain delay",
              "skipped_weather": "weather", "skipped_controller_off": "controller off"}
    for i, r in enumerate(labels):
        hass.bus.async_fire("irrigation_skipped", {
            "seq": i, "inst": 0, "p": 1, "z": 255, "ts": TS + i * 1000, "plan": 0,
            "act": 0, "l": 0, "r": r})
    await settle(hass)
    got = sorted(c["summary"] for c in mocks.calendar.calls)
    assert got == sorted(f"Program B skipped — {v}" for v in labels.values())
    first = [c for c in mocks.calendar.calls if c["summary"].endswith("rain sensor")][0]
    assert first["start_date_time"] == local(TS)
    assert first["end_date_time"] == local(TS + 900)


async def test_manual_named_manual(pkg_hass, mocks):
    hass = pkg_hass
    hass.bus.async_fire("irrigation_program_finished", finished(p=255, zones=1, act=600, l=12.4))
    await settle(hass)
    assert mocks.calendar.calls[0]["summary"] == "Manual · 1 zones · 10 min · 12 L"


async def test_replay_groups_create_one_event_each(pkg_hass, mocks):
    hass = pkg_hass
    hass.bus.async_fire("irrigation_replay_done", {"groups": [
        finished(inst=1, p=0),
        finished(inst=2, p=2, zones=2, act=600, l=20.0, ts=TS + 5000, warn=True)]})
    await settle(hass)
    assert len(mocks.calendar.calls) == 2
    got = sorted(c["summary"] for c in mocks.calendar.calls)
    assert got == ["Program A · 3 zones · 42 min · 610 L",
                   "Program C · 2 zones · 10 min · 20 L ⚠"]


async def test_replayed_skip_creates_skip_event(pkg_hass, mocks):
    hass = pkg_hass
    hass.bus.async_fire("irrigation_replayed_record", {
        "seq": 4, "inst": 0, "p": 0, "z": 255, "ts": TS, "plan": 0, "act": 0, "l": 0,
        "r": "skipped_weather"})
    hass.bus.async_fire("irrigation_replayed_record", {
        "seq": 5, "inst": 1, "p": 0, "z": 1, "ts": TS + 5, "plan": 60, "act": 60, "l": 1,
        "r": "completed"})
    await settle(hass)
    assert [c["summary"] for c in mocks.calendar.calls] == ["Program A skipped — weather"]


async def test_upcoming_sensor_from_plan(pkg_hass, device):
    hass = pkg_hass
    await hass.config.async_set_time_zone("Australia/Sydney")
    device.respond("get_plan", {"plan": [
        {"p": 0, "date": "2026-11-03", "start": "06:00", "s": 480, "pct": 80}]})
    plans = async_capture_events(hass, "irrigation_plan")
    await hass.services.async_call("script", "irrigation_refresh_plan", {}, blocking=True)
    await settle(hass)
    assert device.calls["get_plan"] == [{"days": 14}]
    assert len(plans) == 1
    st = hass.states.get("sensor.irrigation_upcoming")
    # A timestamp sensor stores its state in UTC; the attribute keeps local time.
    assert dt_util.parse_datetime(st.state) == dt_util.parse_datetime("2026-11-03T06:00:00+11:00")
    run = st.attributes["runs"][0]
    assert run["program"] == 0 and run["name"] == "Program A"
    assert run["start"] == "2026-11-03T06:00:00+11:00"
    assert run["minutes"] == 8 and run["pct"] == 80


async def test_upcoming_sensor_empty_plan_unknown(pkg_hass, device):
    hass = pkg_hass
    device.respond("get_plan", {"plan": []})
    await hass.services.async_call("script", "irrigation_refresh_plan", {}, blocking=True)
    await settle(hass)
    assert hass.states.get("sensor.irrigation_upcoming").state == "unknown"


async def test_refresh_on_decision_event(pkg_hass, device):
    hass = pkg_hass
    device.respond("get_plan", {"plan": []})
    hass.bus.async_fire("irrigation_decision", {})
    await settle(hass)
    assert len(device.calls["get_plan"]) == 1


async def test_refresh_error_keeps_previous(pkg_hass, device):
    hass = pkg_hass
    device.respond("get_plan", {"error": "boom"})
    plans = async_capture_events(hass, "irrigation_plan")
    await hass.services.async_call("script", "irrigation_refresh_plan", {}, blocking=True)
    await settle(hass)
    assert plans == []


async def test_upcoming_sorted_with_backup_name(pkg_hass, device):
    hass = pkg_hass
    await hass.config.async_set_time_zone("Australia/Sydney")
    hass.bus.async_fire("irrigation_config_backup", {"config": {
        "programs": [{"program": 2, "name": "Veggies"}], "zones": [], "settings": {}}})
    await settle(hass)
    device.respond("get_plan", {"plan": [
        {"p": 2, "date": "2026-11-04", "start": "05:30", "s": 600, "pct": 100},
        {"p": 0, "date": "2026-11-03", "start": "18:00", "s": 300, "pct": 90},
        {"p": 1, "date": "2026-11-03", "start": "06:00", "s": 1200, "pct": 70}]})
    await hass.services.async_call("script", "irrigation_refresh_plan", {}, blocking=True)
    await settle(hass)
    st = hass.states.get("sensor.irrigation_upcoming")
    runs = st.attributes["runs"]
    assert [r["name"] for r in runs] == ["Program B", "Program A", "Veggies"]
    assert [r["start"] for r in runs] == [
        "2026-11-03T06:00:00+11:00", "2026-11-03T18:00:00+11:00", "2026-11-04T05:30:00+11:00"]
    assert dt_util.parse_datetime(st.state) == dt_util.parse_datetime(runs[0]["start"])
