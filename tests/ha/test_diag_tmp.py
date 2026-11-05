import time
import json
from pytest_homeassistant_custom_component.common import async_capture_events


async def test_diag(pkg_hass):
    hass = pkg_hass
    now = int(time.time())
    def d(seq, ts):
        return {"seq": seq, "inst": 1, "p": 0, "z": 1, "ts": ts, "plan": 60, "act": 60, "l": 1.5, "r": "completed"}
    evs = async_capture_events(hass, "irrigation_run_finished")
    hass.bus.async_fire("esphome.irrigation_event", {"node": "irrigation-controller", "type": "run_finished", "json": json.dumps(d(3, now - 3600))})
    await hass.async_block_till_done()
    print("STATE3", hass.states.get("sensor.irrigation_run_history"))
    print("EVS", [(dict(e.data), {k: type(v).__name__ for k, v in e.data.items()}, e.context.id) for e in evs])
    hass.bus.async_fire("irrigation_run_finished", dict(evs[0].data))
    await hass.async_block_till_done()
    print("STATE4", hass.states.get("sensor.irrigation_run_history"))
    assert 0, "diag"
