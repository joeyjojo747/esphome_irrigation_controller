"""Zone advisor: cycle/soak suggestion from soil, slope and precipitation."""

import copy

import pytest

from test_sync import make_config, settle

# firmware soil enum: SAND, LOAMY_SAND, SANDY_LOAM, LOAM, CLAY_LOAM, CLAY
SAND, LOAMY_SAND, SANDY_LOAM, LOAM, CLAY_LOAM, CLAY = range(6)


def cfg_with(zone=1, soil=LOAM, slope=0, precip=40.0):
    cfg = make_config()
    z = cfg["zones"][zone - 1]
    z.update(soil=soil, slope=slope, precip=precip)
    return cfg


async def suggest(hass, device, cfg, zone=1, apply=None):
    device.respond("get_config", cfg)
    data = {"zone": zone}
    if apply is not None:
        data["apply"] = apply
    res = await hass.services.async_call(
        "script", "irrigation_suggest_zone", data, blocking=True, return_response=True)
    await settle(hass)
    return res


@pytest.mark.parametrize("soil,slope,precip,expected", [
    (LOAM, 0, 40, (19, 38)),
    (CLAY, 10, 40, (4, 20)),
    (LOAM, 0, 12, (0, 0)),
    (CLAY, 20, 50, (3, 20)),
    (SAND, 0, 40, (0, 0)),
])
async def test_suggestions(pkg_hass, device, soil, slope, precip, expected):
    res = await suggest(pkg_hass, device, cfg_with(soil=soil, slope=slope, precip=precip))
    assert (res["max_cycle_min"], res["soak_min"]) == expected
    assert device.calls["set_zone"] == []


@pytest.mark.parametrize("slope,expected", [(4, 19), (5, 15), (7, 15), (8, 11), (12, 11), (13, 7)])
async def test_slope_bands(pkg_hass, device, slope, expected):
    res = await suggest(pkg_hass, device, cfg_with(slope=slope))
    assert res["max_cycle_min"] == expected


async def test_apply_calls_set_zone(pkg_hass, device):
    cfg = cfg_with(zone=3, soil=LOAM, slope=0, precip=40.0)
    res = await suggest(pkg_hass, device, cfg, zone=3, apply=True)
    assert (res["max_cycle_min"], res["soak_min"]) == (19, 38)
    assert len(device.calls["set_zone"]) == 1
    sent = device.calls["set_zone"][0]
    expect = copy.deepcopy(cfg["zones"][2])
    expect.update(max_cycle_min=19, soak_min=38)
    assert sent == expect  # zone_output is kept as in the backup


async def test_no_apply_when_result_is_zero(pkg_hass, device):
    await suggest(pkg_hass, device, cfg_with(precip=12), apply=True)
    assert device.calls["set_zone"] == []


@pytest.mark.parametrize("kw", [{"precip": 0}, {"precip": -1.0}, {"soil": 9}])
async def test_bad_inputs_return_zero(pkg_hass, device, kw):
    res = await suggest(pkg_hass, device, cfg_with(**kw), apply=True)
    assert (res["max_cycle_min"], res["soak_min"]) == (0, 0)
    assert device.calls["set_zone"] == []


async def test_missing_precip_and_unknown_zone(pkg_hass, device):
    cfg = cfg_with()
    del cfg["zones"][0]["precip"]
    res = await suggest(pkg_hass, device, cfg)
    assert (res["max_cycle_min"], res["soak_min"]) == (0, 0)
    res = await suggest(pkg_hass, device, cfg_with(), zone=17, apply=True)
    assert (res["max_cycle_min"], res["soak_min"]) == (0, 0)
    assert device.calls["set_zone"] == []


async def test_config_unavailable_returns_zero(pkg_hass, device):
    res = await suggest(pkg_hass, device, {"error": "boom"}, apply=True)
    assert (res["max_cycle_min"], res["soak_min"]) == (0, 0)
    assert device.calls["set_zone"] == []
