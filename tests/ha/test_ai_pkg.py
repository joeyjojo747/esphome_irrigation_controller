"""AI advisor script: AI Task and Conversation agent providers, validation, combine rule."""

import asyncio
import json

import pytest
from homeassistant.exceptions import HomeAssistantError
from pytest_homeassistant_custom_component.common import async_capture_events

OK = {"summary": "Mild week ahead.", "recommended_pct": 90,
      "program_suggestions": [{"program": "A", "suggestion": "Water at dawn"}],
      "warnings": ["Check zone 2 head"]}


async def settle(hass):
    for _ in range(3):
        await hass.async_block_till_done()
        await asyncio.sleep(0)


async def setup_ai(hass, mode="AI Task", entity="ai_task.test", auto=False, max_pct=None):
    await hass.services.async_call(
        "input_select", "select_option",
        {"entity_id": "input_select.irrigation_ai_mode", "option": mode}, blocking=True)
    await hass.services.async_call(
        "input_text", "set_value",
        {"entity_id": "input_text.irrigation_ai_entity", "value": entity}, blocking=True)
    await hass.services.async_call(
        "input_boolean", "turn_on" if auto else "turn_off",
        {"entity_id": "input_boolean.irrigation_ai_auto_apply"}, blocking=True)
    if max_pct is not None:
        await hass.services.async_call(
            "input_number", "set_value",
            {"entity_id": "input_number.irrigation_ai_max_pct", "value": max_pct}, blocking=True)


def rule(pct=100, skip=False, reason=""):
    return {"skip": skip, "pct": pct, "reason": reason, "inputs": {}, "forecast": []}


def day(**kw):
    d = {"datetime": "2026-11-05T00:00:00+11:00", "templow": 10.0, "temperature": 25.0,
         "precipitation": 0.0, "precipitation_probability": 0, "wind_speed": 5.0}
    d.update(kw)
    return d


async def advise(hass, decision=None, forecast=None):
    return await hass.services.async_call(
        "script", "irrigation_ai_advice",
        {"decision": decision if decision is not None else rule(),
         "forecast": forecast if forecast is not None else [day()]},
        blocking=True, return_response=True)


def task_reply(mocks, data):
    mocks.ai_task.response = {"conversation_id": "c1", "data": data}


def conv_reply(mocks, speech):
    mocks.conversation.response = {
        "response": {"speech": {"plain": {"speech": speech}}}, "conversation_id": "c1"}


async def test_ai_task_valid(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass, "AI Task", "ai_task.test")
    task_reply(mocks, OK)
    events = async_capture_events(hass, "irrigation_advice")
    r = await advise(hass)
    assert r["advice"] == OK
    assert mocks.ai_task.calls[0]["entity_id"] == "ai_task.test"
    assert mocks.ai_task.calls[0]["task_name"] == "irrigation_daily_advice"
    assert set(mocks.ai_task.calls[0]["structure"]) == {
        "summary", "recommended_pct", "program_suggestions", "warnings"}
    await settle(hass)
    assert len(events) == 1
    assert events[0].data["summary"] == "Mild week ahead."
    assert events[0].data["provider"] == "AI Task"
    st = hass.states.get("sensor.irrigation_advice")
    assert st.state == "Mild week ahead."
    assert st.attributes["summary"] == "Mild week ahead."
    assert st.attributes["warnings"] == ["Check zone 2 head"]
    assert st.attributes["program_suggestions"] == OK["program_suggestions"]
    assert st.attributes["provider"] == "AI Task"
    assert st.attributes["generated_at"]


async def test_sensor_state_truncated_to_255(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass)
    task_reply(mocks, dict(OK, summary="x" * 500))
    r = await advise(hass)
    assert len(r["advice"]["summary"]) == 400
    await settle(hass)
    st = hass.states.get("sensor.irrigation_advice")
    assert st.state == "x" * 255
    assert st.attributes["summary"] == "x" * 400


async def test_conversation_agent_json_parsed(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass, "Conversation agent", "conversation.local")
    conv_reply(mocks, json.dumps(OK))
    r = await advise(hass)
    assert r["advice"] == OK
    call = mocks.conversation.calls[0]
    assert call["agent_id"] == "conversation.local"
    assert call["text"].endswith(
        "Reply with only a JSON object with keys summary, recommended_pct, program_suggestions, warnings.")
    assert mocks.ai_task.calls == []


async def test_conversation_agent_fenced_json_parsed(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass, "Conversation agent", "conversation.local")
    conv_reply(mocks, "```json\n" + json.dumps(OK) + "\n```")
    assert (await advise(hass))["advice"] == OK


async def test_conversation_agent_non_json_gives_null(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass, "Conversation agent", "conversation.local")
    events = async_capture_events(hass, "irrigation_advice")
    conv_reply(mocks, "Sure! Water less this week.")
    r = await advise(hass)
    assert r["advice"] is None
    await settle(hass)
    assert events == []


async def test_missing_summary_null(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass)
    task_reply(mocks, {"recommended_pct": 90, "warnings": []})
    assert (await advise(hass))["advice"] is None
    task_reply(mocks, {"summary": 5, "recommended_pct": 90})
    assert (await advise(hass))["advice"] is None


@pytest.mark.parametrize("given,expected", [(250, None), ("abc", None), (95.0, 95), (-1, None), (200, 200)])
async def test_pct_out_of_range_dropped(pkg_hass, mocks, given, expected):
    hass = pkg_hass
    await setup_ai(hass)
    task_reply(mocks, dict(OK, recommended_pct=given))
    r = await advise(hass)
    assert r["advice"]["recommended_pct"] == expected
    assert r["advice"]["summary"] == "Mild week ahead."
    if expected == 95:
        assert isinstance(r["advice"]["recommended_pct"], int)


async def test_never_overrides_skip(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass, auto=True)
    task_reply(mocks, dict(OK, recommended_pct=150))
    r = await advise(hass, rule(0, True, "Frost risk 1.0°C"))
    assert r["final"] == {"skip": True, "pct": 0, "reason": "Frost risk 1.0°C"}
    assert r["advice"]["recommended_pct"] == 150


@pytest.mark.parametrize("pct,ai,max_pct,expected", [
    (100, 180, 150, 130), (100, 40, 150, 70), (130, 170, 150, 150), (100, 110, 150, 110)])
async def test_bounds(pkg_hass, mocks, pct, ai, max_pct, expected):
    hass = pkg_hass
    await setup_ai(hass, auto=True, max_pct=max_pct)
    task_reply(mocks, dict(OK, recommended_pct=ai))
    r = await advise(hass, rule(pct))
    assert r["final"] == {"skip": False, "pct": expected, "reason": ""}
    assert isinstance(r["final"]["pct"], int)


async def test_auto_apply_off_unchanged(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass, auto=False)
    task_reply(mocks, dict(OK, recommended_pct=120))
    r = await advise(hass, rule(100))
    assert r["final"] == {"skip": False, "pct": 100, "reason": ""}
    assert r["advice"]["recommended_pct"] == 120


async def test_no_ai_pct_unchanged(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass, auto=True)
    task_reply(mocks, {"summary": "Fine."})
    r = await advise(hass, rule(100))
    assert r["final"] == {"skip": False, "pct": 100, "reason": ""}
    assert r["advice"]["summary"] == "Fine."
    assert r["advice"]["recommended_pct"] is None


async def test_mode_off_no_call(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass, "Off")
    events = async_capture_events(hass, "irrigation_advice")
    r = await advise(hass)
    assert r["advice"] is None
    assert r["final"] is None
    assert mocks.ai_task.calls == [] and mocks.conversation.calls == []
    await settle(hass)
    assert events == []


async def test_provider_error_null(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass, auto=True)
    mocks.ai_task.side_effect = HomeAssistantError("down")
    r = await advise(hass)
    assert r["advice"] is None and r["final"] is None
    await setup_ai(hass, "Conversation agent", "conversation.local", auto=True)
    mocks.conversation.side_effect = HomeAssistantError("down")
    r = await advise(hass)
    assert r["advice"] is None and r["final"] is None


async def test_prompt_contains_zone_and_forecast(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass)
    hass.states.async_set("sensor.irrigation_config_backup", "2026-11-05T00:00:00+00:00", {
        "config": {"zones": [
            {"zone": 1, "name": "Front Lawn", "enabled": True, "head": 1, "precip": 12.5,
             "soil": 3, "sun": 0, "plant": 0, "area": 80.0},
            {"zone": 2, "name": "Hidden Bed", "enabled": False, "head": 3, "precip": 6.0,
             "soil": 1, "sun": 1, "plant": 3, "area": 10.0}]}})
    hass.states.async_set("sensor.irrigation_run_history", "2", {"records": [
        {"seq": 1, "z": 0, "act": 600, "l": 40.0}, {"seq": 2, "z": 0, "act": 300, "l": 20.5},
        {"seq": 3, "z": 1, "act": 60, "l": 1.0}]})
    task_reply(mocks, OK)
    await advise(hass, rule(70), [day(temperature=31.5, templow=12.0, wind_speed=22)])
    prompt = mocks.ai_task.calls[0]["instructions"]
    assert "Front Lawn" in prompt
    assert "Hidden Bed" not in prompt
    assert "rotor" in prompt and "loam" in prompt
    assert "80.0 m2" in prompt
    assert "60.5 L" in prompt and "15 min" in prompt
    assert "2026-11-05" in prompt and "31.5" in prompt and "22" in prompt
    assert "70%" in prompt
    assert prompt.rstrip().endswith(
        "Return a summary of at most 80 words for a home gardener in Australia, a recommended "
        "run-time percentage, suggestions per program, and warnings. "
        "Do not recommend watering during a rule-based skip.")


async def test_prompt_states_skip(pkg_hass, mocks):
    hass = pkg_hass
    await setup_ai(hass)
    task_reply(mocks, OK)
    await advise(hass, rule(0, True, "Frost risk 1.0°C"))
    assert "SKIP" in mocks.ai_task.calls[0]["instructions"]
    assert "Frost risk" in mocks.ai_task.calls[0]["instructions"]


async def test_daily_adjust_uses_real_script(pkg_hass, device, mocks):
    """Weather automation + real AI script end to end (auto-apply on, AI Task mocked)."""
    hass = pkg_hass
    await setup_ai(hass, auto=True)
    task_reply(mocks, dict(OK, recommended_pct=110))
    mocks.weather.response = {"weather.home": {"forecast": [day(temperature=17.0)]}}
    await hass.services.async_call(
        "automation", "trigger", {"entity_id": "automation.irrigation_daily_adjust"},
        blocking=True)
    await settle(hass)
    assert device.calls["set_weather_adjust"] == [
        {"pct": 100, "skip": False, "reason": "", "valid_hours": 30}]
    assert hass.states.get("sensor.irrigation_weather_decision").attributes["ai_applied"] is True
