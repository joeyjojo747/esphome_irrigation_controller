import time
import json


async def test_diag(pkg_hass):
    hass = pkg_hass
    now = int(time.time())
    def d(seq, ts):
        return {"seq": seq, "inst": 1, "p": 0, "z": 1, "ts": ts, "plan": 60, "act": 60, "l": 1.5, "r": "completed"}
    hass.bus.async_fire("irrigation_run_finished", d(1, now - 8 * 86400))
    await hass.async_block_till_done()
    print("STATE1", hass.states.get("sensor.irrigation_run_history"))
    hass.bus.async_fire("irrigation_run_finished", d(2, now - 86400))
    await hass.async_block_till_done()
    print("STATE2", hass.states.get("sensor.irrigation_run_history"))
    hass.bus.async_fire("esphome.irrigation_event", {"node": "irrigation-controller", "type": "run_finished", "json": json.dumps(d(3, now - 3600))})
    await hass.async_block_till_done()
    print("STATE3", hass.states.get("sensor.irrigation_run_history"))
    assert 0, "diag"
