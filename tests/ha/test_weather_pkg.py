"""Weather rules script, the daily adjust automation and the decision sensor."""

import asyncio

import pytest
from homeassistant.core import SupportsResponse
from homeassistant.exceptions import HomeAssistantError
from pytest_homeassistant_custom_component.common import async_capture_events


async def settle(hass):
    for _ in range(3):
        await hass.async_block_till_done()
        await asyncio.sleep(0)


def fc(mocks, **kw):
    day = {"datetime": "2026-11-05T00:00:00+11:00", "templow": 10.0, "temperature": 25.0,
           "precipitation": 0.0, "precipitation_probability": 0, "wind_speed": 5.0}
    day.update(kw)
    mocks.weather.response = {"weather.home": {"forecast": [day]}}


async def evaluate(hass):
    return await hass.services.async_call(
        "script", "irrigation_evaluate_weather", {}, blocking=True, return_response=True)


async def set_obs(hass, mm):
    hass.states.async_set("sensor.rain_obs", str(mm))
    await hass.services.async_call(
        "input_text", "set_value",
        {"entity_id": "input_text.irrigation_rain_obs_entity", "value": "sensor.rain_obs"},
        blocking=True)


async def test_rain_skip(pkg_hass, mocks):
    hass = pkg_hass
    await set_obs(hass, 2.5)
    fc(mocks, precipitation=1.0)
    r = await evaluate(hass)
    assert r["skip"] is True
    assert r["pct"] == 0
    assert r["reason"] == "Rain 3.5 mm (24h + forecast)"


async def test_rain_probability(pkg_hass, mocks):
    hass = pkg_hass
    fc(mocks, precipitation_probability=85, precipitation=2.0)
    r = await evaluate(hass)
    assert r["skip"] is True and r["pct"] == 0
    assert r["reason"] == "Rain chance 85%"
    fc(mocks, precipitation_probability=85, precipitation=1.0)
    r = await evaluate(hass)
    assert r["skip"] is False and r["pct"] == 100


async def test_frost(pkg_hass, mocks):
    hass = pkg_hass
    fc(mocks, templow=1.5)
    r = await evaluate(hass)
    assert r["skip"] is True and r["pct"] == 0
    assert r["reason"] == "Frost risk 1.5°C"
    fc(mocks, templow=2.1)
    assert (await evaluate(hass))["skip"] is False


async def test_wind(pkg_hass, mocks):
    hass = pkg_hass
    fc(mocks, wind_speed=35)
    assert (await evaluate(hass))["skip"] is False
    await hass.services.async_call(
        "input_boolean", "turn_on", {"entity_id": "input_boolean.irrigation_wind_enabled"},
        blocking=True)
    r = await evaluate(hass)
    assert r["skip"] is True and r["pct"] == 0
    assert r["reason"] == "Wind 35 km/h"


async def test_cumulative(pkg_hass, mocks):
    hass = pkg_hass
    fc(mocks)
    hass.states.async_set("sensor.irrigation_rain_history", "5.0", {"days": [9.0, 5.0, 5.0, 5.0]})
    r = await evaluate(hass)
    assert r["skip"] is True and r["pct"] == 0
    assert r["reason"] == "Rain 15.0 mm in 3 days"
    hass.states.async_set("sensor.irrigation_rain_history", "5.0", {"days": [9.0, 5.0, 5.0, 4.9]})
    assert (await evaluate(hass))["skip"] is False


async def test_rain_snapshot_keeps_14_days(pkg_hass, mocks):
    hass = pkg_hass
    for i in range(16):
        await set_obs(hass, i)
        hass.bus.async_fire("irrigation_rain_snapshot")
        await settle(hass)
    st = hass.states.get("sensor.irrigation_rain_history")
    assert st.attributes["days"] == [float(i) for i in range(2, 16)]
    assert float(st.state) == 15.0


@pytest.mark.parametrize("tmax,pct", [(17.9, 70), (32.1, 130), (25.0, 100), (None, 100)])
async def test_temperature_factor(pkg_hass, mocks, tmax, pct):
    fc(mocks, temperature=tmax)
    r = await evaluate(pkg_hass)
    assert r["skip"] is False
    assert r["pct"] == pct
    assert isinstance(r["pct"], int)


async def test_rule_order(pkg_hass, mocks):
    hass = pkg_hass
    await hass.services.async_call(
        "input_boolean", "turn_on", {"entity_id": "input_boolean.irrigation_wind_enabled"},
        blocking=True)
    hass.states.async_set("sensor.irrigation_rain_history", "5.0", {"days": [5.0, 5.0, 5.0]})
    await set_obs(hass, 2.5)
    fc(mocks, precipitation=1.0, precipitation_probability=90, templow=0.0, wind_speed=40)
    assert (await evaluate(hass))["reason"] == "Rain 3.5 mm (24h + forecast)"
    await set_obs(hass, 0)
    fc(mocks, precipitation=2.5, precipitation_probability=90, templow=0.0, wind_speed=40)
    assert (await evaluate(hass))["reason"] == "Rain chance 90%"
    fc(mocks, precipitation=0.0, precipitation_probability=90, templow=0.0, wind_speed=40)
    assert (await evaluate(hass))["reason"] == "Frost risk 0.0°C"
    fc(mocks, templow=10.0, wind_speed=40)
    assert (await evaluate(hass))["reason"] == "Wind 40 km/h"
    fc(mocks, templow=10.0, wind_speed=5)
    assert (await evaluate(hass))["reason"] == "Rain 15.0 mm in 3 days"


async def test_weather_unavailable(pkg_hass, mocks):
    hass = pkg_hass
    mocks.weather.side_effect = HomeAssistantError("down")
    r = await evaluate(hass)
    assert r["pct"] is None and r["reason"] == "weather unavailable"
    mocks.weather.side_effect = None
    mocks.weather.response = {"weather.home": {"forecast": []}}
    r = await evaluate(hass)
    assert r["pct"] is None and r["reason"] == "weather unavailable"


async def trigger_daily(hass):
    await hass.services.async_call(
        "automation", "trigger", {"entity_id": "automation.irrigation_daily_adjust"},
        blocking=True)
    await settle(hass)


async def test_daily_adjust_pushes(pkg_hass, device, mocks):
    hass = pkg_hass
    events = async_capture_events(hass, "irrigation_decision")
    fc(mocks, temperature=17.0)
    await trigger_daily(hass)
    assert device.calls["set_weather_adjust"] == [
        {"pct": 70, "skip": False, "reason": "", "valid_hours": 30}]
    assert len(events) == 1
    assert events[0].data["pct"] == 70 and events[0].data["ai_applied"] is False
    st = hass.states.get("sensor.irrigation_weather_decision")
    assert st.state == "70"
    assert st.attributes["ai_applied"] is False
    assert st.attributes["pushed_at"]


async def test_daily_adjust_skip(pkg_hass, device, mocks):
    hass = pkg_hass
    fc(mocks, templow=1.0)
    await trigger_daily(hass)
    assert device.calls["set_weather_adjust"] == [
        {"pct": 0, "skip": True, "reason": "Frost risk 1.0°C", "valid_hours": 30}]
    assert hass.states.get("sensor.irrigation_weather_decision").state == "skip"


async def test_unavailable_weather_pushes_nothing(pkg_hass, device, mocks):
    hass = pkg_hass
    events = async_capture_events(hass, "irrigation_decision")
    mocks.weather.side_effect = HomeAssistantError("down")
    await trigger_daily(hass)
    assert device.calls["set_weather_adjust"] == []
    assert len(events) == 1
    assert events[0].data["pct"] is None
    assert events[0].data["error"] == "weather unavailable"


async def test_ai_advice_used_when_present(pkg_hass, device, mocks):
    hass = pkg_hass
    seen = []

    async def advice(call):
        seen.append(dict(call.data))
        return {"final": {"pct": 85, "skip": False, "reason": "AI: mild week"}, "advice": None}

    hass.services.async_register("script", "irrigation_ai_advice", advice,
                                 supports_response=SupportsResponse.OPTIONAL)
    hass.states.async_set("script.irrigation_ai_advice", "off")
    fc(mocks, temperature=17.0)
    await trigger_daily(hass)
    assert len(seen) == 1
    assert seen[0]["decision"]["pct"] == 70
    assert seen[0]["forecast"][0]["temperature"] == 17.0
    assert device.calls["set_weather_adjust"] == [
        {"pct": 85, "skip": False, "reason": "AI: mild week", "valid_hours": 30}]
    assert hass.states.get("sensor.irrigation_weather_decision").attributes["ai_applied"] is True


async def register_ai(hass, handler):
    hass.services.async_register("script", "irrigation_ai_advice", handler,
                                 supports_response=SupportsResponse.OPTIONAL)
    hass.states.async_set("script.irrigation_ai_advice", "off")


@pytest.mark.parametrize("bad", [
    {"pct": 85, "skip": False},
    {"final": {"pct": "abc", "skip": False, "reason": ""}, "advice": None},
    {"final": {"pct": 85}, "advice": None},
    {"final": "x"},
    "nonsense",
])
async def test_ai_malformed_falls_back(pkg_hass, device, mocks, bad):
    hass = pkg_hass

    async def advice(call):
        return bad

    await register_ai(hass, advice)
    fc(mocks, temperature=17.0)
    await trigger_daily(hass)
    assert device.calls["set_weather_adjust"] == [
        {"pct": 70, "skip": False, "reason": "", "valid_hours": 30}]
    assert hass.states.get("sensor.irrigation_weather_decision").attributes["ai_applied"] is False


async def test_ai_error_falls_back(pkg_hass, device, mocks):
    hass = pkg_hass

    async def advice(call):
        raise HomeAssistantError("boom")

    await register_ai(hass, advice)
    fc(mocks, temperature=17.0)
    await trigger_daily(hass)
    assert device.calls["set_weather_adjust"] == [
        {"pct": 70, "skip": False, "reason": "", "valid_hours": 30}]
    assert hass.states.get("sensor.irrigation_weather_decision").attributes["ai_applied"] is False


async def test_decision_sensor_when_unavailable(pkg_hass, device, mocks):
    hass = pkg_hass
    mocks.weather.side_effect = HomeAssistantError("down")
    await trigger_daily(hass)
    st = hass.states.get("sensor.irrigation_weather_decision")
    assert st.state == "error"
    assert st.attributes["reason"] == "weather unavailable"
    assert st.attributes["ai_applied"] is False
    assert st.attributes["pushed_at"] is None


async def test_missing_forecast_values_give_no_rule(pkg_hass, mocks):
    hass = pkg_hass
    await hass.services.async_call(
        "input_boolean", "turn_on", {"entity_id": "input_boolean.irrigation_wind_enabled"},
        blocking=True)
    fc(mocks, templow=None, temperature=None, precipitation=None,
       precipitation_probability=None, wind_speed=None)
    r = await evaluate(hass)
    assert r["skip"] is False and r["pct"] == 100 and r["reason"] == ""
    assert len(r["forecast"]) == 1


async def test_evaluate_forecast_empty_when_unavailable(pkg_hass, mocks):
    mocks.weather.side_effect = HomeAssistantError("down")
    assert (await evaluate(pkg_hass))["forecast"] == []
