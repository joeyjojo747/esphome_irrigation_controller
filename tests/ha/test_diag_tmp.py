import time
from homeassistant.helpers.template import Template
from conftest import load_packages


async def test_diag(pkg_hass):
    hass = pkg_hass
    tpl = load_packages()["irrigation_sync"]["template"][0]["variables"]["records"]
    now = int(time.time())
    data = {"seq": 2, "inst": 1, "p": 0, "z": 1, "ts": now - 86400, "plan": 60, "act": 60, "l": 1.5, "r": "completed"}
    out = Template(tpl, hass).async_render({"trigger": {"event": {"data": data}}}, parse_result=True)
    print("RENDER", repr(out))
    hass.bus.async_fire("irrigation_run_finished", data)
    await hass.async_block_till_done()
    st = hass.states.get("sensor.irrigation_run_history")
    print("STATE", st)
    assert 0, "diag"
