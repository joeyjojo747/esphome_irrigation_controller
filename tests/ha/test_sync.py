"""Event routing, run-log replay after outages, run history, backup and restore."""

import asyncio
import json
import time

from homeassistant.core import HomeAssistant, ServiceCall
from helpers import make_config, settle
from pytest_homeassistant_custom_component.common import async_capture_events

NODE = "irrigation-controller"


def rec(seq, ts, inst=1, p=0, z=1, r="completed", act=60, l=1.5, plan=60):
    return {"seq": seq, "inst": inst, "p": p, "z": z, "ts": ts, "plan": plan,
            "act": act, "l": l, "r": r}


def fire(hass, type_, payload, node=NODE):
    hass.bus.async_fire("esphome.irrigation_event",
                        {"node": node, "type": type_, "json": json.dumps(payload)})


async def set_helper(hass, name, value):
    await hass.services.async_call(
        "input_number", "set_value",
        {"entity_id": f"input_number.irrigation_{name}", "value": value}, blocking=True)


def helper(hass, name):
    return int(float(hass.states.get(f"input_number.irrigation_{name}").state))


async def call_script(hass, name):
    await hass.services.async_call("script", name, {}, blocking=True)
    await settle(hass)


def register_refresh_stub(hass):
    calls = []

    async def handle(call: ServiceCall):
        calls.append(dict(call.data))

    hass.services.async_register("script", "irrigation_refresh_plan", handle)
    hass.states.async_set("script.irrigation_refresh_plan", "off")
    return calls


# ---------------------------------------------------------------- router


async def test_router_refires_typed_event(pkg_hass, device):
    hass = pkg_hass
    events = async_capture_events(hass, "irrigation_run_finished")
    fire(hass, "run_finished", rec(7, 5000))
    await settle(hass)
    assert len(events) == 1
    assert events[0].data["seq"] == 7
    assert events[0].data["ts"] == 5000
    assert events[0].data["r"] == "completed"
    assert helper(hass, "last_ts") == 5000
    assert helper(hass, "last_seq") == 7

    # An older record does not move the helpers back.
    fire(hass, "run_finished", rec(3, 4000))
    await settle(hass)
    assert len(events) == 2
    assert helper(hass, "last_ts") == 5000
    assert helper(hass, "last_seq") == 7


async def test_router_refires_other_types(pkg_hass, device):
    hass = pkg_hass
    alerts = async_capture_events(hass, "irrigation_alert")
    skipped = async_capture_events(hass, "irrigation_skipped")
    fire(hass, "alert", {"kind": "flow_high", "z": 3, "flow": 12.5, "limit": 10.0})
    fire(hass, "skipped", rec(9, 6000, inst=0, z=255, r="skipped_rain_delay"))
    await settle(hass)
    assert alerts[0].data == {"kind": "flow_high", "z": 3, "flow": 12.5, "limit": 10.0}
    assert skipped[0].data["r"] == "skipped_rain_delay"
    assert helper(hass, "last_ts") == 6000


async def test_router_ignores_other_nodes(pkg_hass, device):
    hass = pkg_hass
    events = async_capture_events(hass, "irrigation_run_finished")
    fire(hass, "run_finished", rec(7, 5000), node="other-node")
    await settle(hass)
    assert events == []
    assert helper(hass, "last_ts") == 0


async def test_config_changed_backs_up_without_refresh_script(pkg_hass, device):
    hass = pkg_hass
    device.respond("get_config", make_config())
    fire(hass, "config_changed", {})
    await settle(hass)
    assert len(device.calls["get_config"]) == 1
    assert hass.states.get("sensor.irrigation_config_backup") is not None


async def test_config_changed_runs_refresh_when_present(pkg_hass, device):
    hass = pkg_hass
    device.respond("get_config", make_config())
    refresh = register_refresh_stub(hass)
    fire(hass, "config_changed", {})
    await settle(hass)
    assert len(refresh) == 1
    assert len(device.calls["get_config"]) == 1


# ---------------------------------------------------------------- replay


async def test_replay_after_outage(pkg_hass, device):
    hass = pkg_hass
    await set_helper(hass, "last_ts", 1000)
    await set_helper(hass, "last_seq", 5)
    device.respond("get_run_log", {"records": [rec(1, 900), rec(2, 1100), rec(3, 1200)], "last": 3})
    replayed = async_capture_events(hass, "irrigation_replayed_record")
    await call_script(hass, "irrigation_sync_runlog")
    assert [e.data["ts"] for e in replayed] == [1100, 1200]
    assert helper(hass, "last_ts") == 1200
    assert helper(hass, "last_seq") == 3

    # A second sync replays nothing.
    await call_script(hass, "irrigation_sync_runlog")
    assert len(replayed) == 2


async def test_replay_orders_ascending_and_dedupes(pkg_hass, device):
    hass = pkg_hass
    device.respond("get_run_log", {"records": [rec(2, 300), rec(1, 100), rec(2, 300), rec(1, 200)],
                                   "last": 2})
    replayed = async_capture_events(hass, "irrigation_replayed_record")
    await call_script(hass, "irrigation_sync_runlog")
    assert [(e.data["ts"], e.data["seq"]) for e in replayed] == [(100, 1), (200, 1), (300, 2)]


async def test_replay_error_response_does_nothing(pkg_hass, device):
    hass = pkg_hass
    device.respond("get_run_log", {"error": "boom"})
    replayed = async_capture_events(hass, "irrigation_replayed_record")
    done = async_capture_events(hass, "irrigation_replay_done")
    await call_script(hass, "irrigation_sync_runlog")
    assert replayed == [] and done == []


async def test_replay_groups_by_instance(pkg_hass, device):
    hass = pkg_hass
    device.respond("get_run_log", {"records": [
        rec(1, 1000, inst=10, p=0, z=1, act=60, l=2.0),
        rec(2, 1100, inst=10, p=0, z=2, act=120, l=3.0, r="flow_high"),
        rec(3, 2000, inst=11, p=2, z=1, act=30, l=1.0),
    ], "last": 3})
    done = async_capture_events(hass, "irrigation_replay_done")
    await call_script(hass, "irrigation_sync_runlog")
    assert len(done) == 1
    groups = {g["inst"]: g for g in done[0].data["groups"]}
    assert groups[10] == {"inst": 10, "p": 0, "zones": 2, "act": 180, "l": 5.0,
                          "warn": True, "ts": 1000}
    assert groups[11] == {"inst": 11, "p": 2, "zones": 1, "act": 30, "l": 1.0,
                          "warn": False, "ts": 2000}


async def test_replay_excludes_skipped_from_groups(pkg_hass, device):
    hass = pkg_hass
    device.respond("get_run_log", {"records": [
        rec(1, 1000, inst=0, p=1, z=255, r="skipped_weather", act=0, l=0.0),
        rec(2, 1100, inst=10, p=0, z=1),
    ], "last": 2})
    replayed = async_capture_events(hass, "irrigation_replayed_record")
    done = async_capture_events(hass, "irrigation_replay_done")
    await call_script(hass, "irrigation_sync_runlog")
    assert [e.data["r"] for e in replayed] == ["skipped_weather", "completed"]
    assert [g["inst"] for g in done[0].data["groups"]] == [10]


async def test_reconnect_syncs_and_refreshes(pkg_hass, device):
    hass = pkg_hass
    refresh = register_refresh_stub(hass)
    device.respond("get_run_log", {"records": [rec(1, 1000)], "last": 1})
    replayed = async_capture_events(hass, "irrigation_replayed_record")
    hass.states.async_set("sensor.irrigation_status", "unavailable")
    await settle(hass)
    assert replayed == [] and refresh == []
    hass.states.async_set("sensor.irrigation_status", "idle")
    await settle(hass)
    assert len(device.calls["get_run_log"]) == 1
    assert len(replayed) == 1
    assert len(refresh) == 1


# ---------------------------------------------------------------- history


async def test_history_capped_and_7_days(pkg_hass, device):
    hass = pkg_hass
    now = int(time.time())
    fire(hass, "run_finished", rec(1, now - 8 * 86400))
    fire(hass, "run_finished", rec(2, now - 6 * 86400))
    await settle(hass)
    st = hass.states.get("sensor.irrigation_run_history")
    assert st.state == "1"
    assert [r["seq"] for r in st.attributes["records"]] == [2]

    for i in range(310):
        hass.bus.async_fire("irrigation_replayed_record", rec(100 + i, now - 5 * 86400 + i))
    await settle(hass)
    st = hass.states.get("sensor.irrigation_run_history")
    assert st.state == "300"
    seqs = [r["seq"] for r in st.attributes["records"]]
    assert len(seqs) == 300
    assert seqs[-1] == 409 and seqs[0] == 110

    # A repeat of the same (ts, seq) does not duplicate.
    hass.bus.async_fire("irrigation_replayed_record", rec(409, now - 5 * 86400 + 309))
    await settle(hass)
    assert hass.states.get("sensor.irrigation_run_history").state == "300"


# ---------------------------------------------------------------- backup and restore


async def backup(hass, device, cfg):
    device.respond("get_config", cfg)
    await call_script(hass, "irrigation_backup_config")


async def test_backup_stores_config(pkg_hass, device):
    hass = pkg_hass
    cfg = make_config()
    await backup(hass, device, cfg)
    st = hass.states.get("sensor.irrigation_config_backup")
    assert st.attributes["config"] == cfg


async def test_backup_error_response_keeps_old_backup(pkg_hass, device):
    hass = pkg_hass
    cfg = make_config()
    await backup(hass, device, cfg)
    await backup(hass, device, {"error": "boom"})
    assert hass.states.get("sensor.irrigation_config_backup").attributes["config"] == cfg


async def test_backup_restore_roundtrip(pkg_hass, device):
    hass = pkg_hass
    cfg = make_config()
    await backup(hass, device, cfg)
    order = []
    hass.bus.async_listen(
        "call_service",
        lambda e: order.append(e.data["service"]) if e.data["domain"] == "esphome" else None)
    before = len(device.calls["get_config"])

    await call_script(hass, "irrigation_restore_config")

    assert len(device.calls["set_zone"]) == 32
    assert len(device.calls["set_settings"]) == 1
    assert len(device.calls["set_program"]) == 8
    assert order[:-1] == (["irrigation_controller_set_zone"] * 16
                          + ["irrigation_controller_set_settings"]
                          + ["irrigation_controller_set_program"] * 8
                          + ["irrigation_controller_set_zone"] * 16)
    first = device.calls["set_zone"][:16]
    assert all(c["zone_output"] == "None" for c in first)
    assert [{**c, "zone_output": z["zone_output"]} for c, z in zip(first, cfg["zones"])] == cfg["zones"]
    assert device.calls["set_zone"][16:] == cfg["zones"]
    assert device.calls["set_settings"][0] == cfg["settings"]
    assert device.calls["set_program"] == cfg["programs"]
    # exactly one backup, at the end
    assert len(device.calls["get_config"]) == before + 1
    assert order[-1] == "irrigation_controller_get_config"


async def test_restore_without_backup_does_nothing(pkg_hass, device):
    try:
        await pkg_hass.services.async_call("script", "irrigation_restore_config", {}, blocking=True)
    except Exception:  # noqa: BLE001 - the script may surface its error stop
        pass
    await settle(pkg_hass)
    assert device.calls["set_zone"] == []
    assert device.calls["set_settings"] == []


async def test_restore_does_not_overwrite_backup(pkg_hass, device):
    hass = pkg_hass
    cfg = make_config()
    await backup(hass, device, cfg)
    before = len(device.calls["get_config"])
    changed = async_capture_events(hass, "irrigation_config_changed")
    seen = {}

    async def set_settings(call: ServiceCall):
        device.calls["set_settings"].append(dict(call.data))
        fire(hass, "config_changed", {})
        fire(hass, "config_changed", {})
        await asyncio.sleep(0.2)  # let the router handle them while the restore runs
        seen["handled"] = len(changed)
        seen["gets"] = len(device.calls["get_config"])
        seen["running"] = hass.states.get("script.irrigation_restore_config").state

    hass.services.async_register("esphome", "irrigation_controller_set_settings", set_settings)
    await call_script(hass, "irrigation_restore_config")

    assert seen["running"] == "on"
    assert seen["handled"] == 2
    assert seen["gets"] == before  # no backup ran during the restore
    assert len(device.calls["get_config"]) == before + 1  # the one at the end
    assert hass.states.get("sensor.irrigation_config_backup").attributes["config"] == cfg


async def test_restore_swapped_outputs(pkg_hass, device):
    hass = pkg_hass
    cfg = make_config(swapped=True)
    await backup(hass, device, cfg)
    device.zone_outputs.update({1: "A", 2: "B"})  # the device currently has them the other way
    await call_script(hass, "irrigation_restore_config")
    assert device.zone_outputs[1] == "B"
    assert device.zone_outputs[2] == "A"
    assert len(device.calls["set_zone"]) == 32


# ---------------------------------------------------------------- I-1: HA start


def test_reconnect_also_triggers_on_homeassistant_start():
    from conftest import load_packages

    autos = {a["id"]: a for a in load_packages()["irrigation_sync"]["automation"]}
    triggers = autos["irrigation_reconnect"]["triggers"]
    assert any(t.get("trigger") == "homeassistant" and t.get("event") == "start" for t in triggers)


async def test_ha_start_replays_runs_made_while_ha_was_down(hass, device):
    """The device reconnects before HA attaches triggers: the start trigger still syncs."""
    from conftest import load_packages
    from homeassistant.core import CoreState
    from homeassistant.setup import async_setup_component

    hass.set_state(CoreState.not_running)
    pkgs = load_packages()
    sync = pkgs["irrigation_sync"]
    assert await async_setup_component(
        hass, "input_number", {"input_number": pkgs["irrigation_helpers"]["input_number"]})
    assert await async_setup_component(
        hass, "script", {"script": {"irrigation_sync_runlog": sync["script"]["irrigation_sync_runlog"]}})
    assert await async_setup_component(
        hass, "automation",
        {"automation": [a for a in sync["automation"] if a["id"] == "irrigation_reconnect"]})
    await hass.async_block_till_done()
    # Restored watermark: runs up to ts 1000 were already seen before the restart.
    await set_helper(hass, "last_ts", 1000)
    await set_helper(hass, "last_seq", 5)
    device.respond("get_run_log", {"records": [rec(5, 1000), rec(6, 1100), rec(7, 1200)], "last": 7})
    hass.states.async_set("sensor.irrigation_status", "idle")  # already connected
    replayed = async_capture_events(hass, "irrigation_replayed_record")
    await hass.async_start()
    await settle(hass)
    assert len(device.calls["get_run_log"]) == 1
    assert [e.data["ts"] for e in replayed] == [1100, 1200]
    assert helper(hass, "last_ts") == 1200
