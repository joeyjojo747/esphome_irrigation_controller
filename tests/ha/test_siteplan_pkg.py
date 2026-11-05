"""script.irrigation_siteplan_run_zone: only zone and minutes reach the device."""


async def test_run_zone_ignores_extra_entity_id(pkg_hass, device):
    await pkg_hass.services.async_call(
        "script", "irrigation_siteplan_run_zone",
        {"zone": 3, "minutes": 5, "entity_id": "switch.irrigation_zone_3"}, blocking=True)
    await pkg_hass.async_block_till_done()
    assert device.calls["run_zone"] == [{"zone": 3, "minutes": 5}]
