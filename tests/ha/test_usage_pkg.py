"""Water usage and cost: utility meters on the firmware counters, template cost sensors."""

import logging

import pytest

from conftest import PACKAGES
from helpers import settle
from homeassistant.util.yaml import load_yaml
from test_firmware_yaml import PKG, as_list, load

TARIFF = "input_number.irrigation_tariff_per_kl"
COSTS = {"today": "irrigation_water_today", "month": "irrigation_water_month",
         "year": "irrigation_water_year"}


def usage_pkg():
    return load_yaml(str(PACKAGES / "irrigation_usage.yaml"))


async def set_tariff(hass, value):
    await hass.services.async_call(
        "input_number", "set_value", {"entity_id": TARIFF, "value": value}, blocking=True)
    await settle(hass)


async def set_litres(hass, meter, value):
    hass.states.async_set(f"sensor.{meter}", str(value))
    await settle(hass)


def number(hass, entity_id):
    return float(hass.states.get(entity_id).state)


async def test_cost_rounding(pkg_hass):
    await set_tariff(pkg_hass, 3.00)
    for period, meter in COSTS.items():
        await set_litres(pkg_hass, meter, 1234.5)
        assert number(pkg_hass, f"sensor.irrigation_cost_{period}") == pytest.approx(3.70)
    st = pkg_hass.states.get("sensor.irrigation_cost_today")
    assert number(pkg_hass, "sensor.irrigation_cost_today") == 3.70
    assert st.attributes["device_class"] == "monetary"
    assert st.attributes["unit_of_measurement"] == "$"
    # follows the tariff
    await set_tariff(pkg_hass, 4.50)
    assert number(pkg_hass, "sensor.irrigation_cost_today") == 5.56


async def test_zero_tariff_is_zero(pkg_hass):
    await set_tariff(pkg_hass, 0)
    for period, meter in COSTS.items():
        await set_litres(pkg_hass, meter, 1234.5)
        assert number(pkg_hass, f"sensor.irrigation_cost_{period}") == 0.0


async def test_unavailable_inputs_give_unavailable_not_zero(pkg_hass):
    await set_tariff(pkg_hass, 3.00)
    await set_litres(pkg_hass, "irrigation_water_today", 1000)
    assert number(pkg_hass, "sensor.irrigation_cost_today") == 3.0
    await set_litres(pkg_hass, "irrigation_water_today", "unavailable")
    assert pkg_hass.states.get("sensor.irrigation_cost_today").state == "unavailable"
    await set_litres(pkg_hass, "irrigation_water_today", 1000)
    pkg_hass.states.async_set(TARIFF, "unavailable")
    await settle(pkg_hass)
    assert pkg_hass.states.get("sensor.irrigation_cost_today").state == "unavailable"


async def test_cost_sensors_load_without_warnings(pkg_hass, caplog):
    caplog.set_level(logging.WARNING)
    await set_tariff(pkg_hass, 3.00)
    await set_litres(pkg_hass, "irrigation_water_today", 10)
    assert number(pkg_hass, "sensor.irrigation_cost_today") == 0.03
    bad = [r.getMessage() for r in caplog.records
           if "cost" in r.getMessage() and r.levelno >= logging.WARNING]
    assert not bad, bad


def test_sixteen_zone_meters():
    meters = usage_pkg()["utility_meter"]
    for key, source, cycle in (
            ("irrigation_water_today", "sensor.irrigation_total_water", "daily"),
            ("irrigation_water_month", "sensor.irrigation_total_water", "monthly"),
            ("irrigation_water_year", "sensor.irrigation_total_water", "yearly")):
        assert meters[key]["source"] == source
        assert meters[key]["cycle"] == cycle
    for n in range(1, 17):
        m = meters[f"irrigation_zone_{n}_water_month"]
        assert m["source"] == f"sensor.irrigation_zone_{n}_water"
        assert m["cycle"] == "monthly"
    assert len(meters) == 3 + 16


def test_meters_use_total_increasing_source():
    sensors = {}
    for block in as_list(load(PKG / "core.yaml")["sensor"]):
        sensors[block["name"]] = block
    for key, meter in usage_pkg()["utility_meter"].items():
        src = meter["source"]
        assert src.startswith("sensor.irrigation_"), key
        name = src.removeprefix("sensor.irrigation_").replace("_", " ").title()
        fw = sensors[name]  # e.g. "Zone 3 Water", "Total Water"
        assert fw["state_class"] == "total_increasing", src
        assert fw["unit_of_measurement"] == "L", src
        assert not meter.get("net_consumption"), key


def test_meters_not_periodically_resetting():
    # The firmware counters never reset: a lower value after a device reboot must not
    # be taken as a reset and the whole lifetime total counted again.
    meters = usage_pkg()["utility_meter"]
    assert len(meters) == 19
    for key, meter in meters.items():
        assert meter.get("periodically_resetting") is False, key
