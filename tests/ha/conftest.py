"""Fixtures: load the irrigation packages into a test HA and mock the outside world."""

import inspect
from pathlib import Path

import pytest
from homeassistant.config import merge_packages_config
from homeassistant.core import HomeAssistant, ServiceCall, SupportsResponse
from homeassistant.exceptions import HomeAssistantError
from homeassistant.setup import async_setup_component
from homeassistant.util.yaml import load_yaml

ROOT = Path(__file__).resolve().parents[2]
PACKAGES = ROOT / "packages" / "irrigation"

pytest_plugins = "pytest_homeassistant_custom_component"

DOMAINS = ("input_boolean", "input_datetime", "input_number", "input_select",
           "input_text", "template", "script", "automation", "utility_meter")

RESPONSE_ACTIONS = ("get_plan", "get_run_log", "get_config")
VOID_ACTIONS = ("run_zone", "run_program", "stop_all", "rain_delay",
                "set_weather_adjust", "set_program", "set_zone", "set_settings")


def load_packages() -> dict:
    """Every packages/irrigation/*.yaml, keyed by file stem."""
    return {p.stem: load_yaml(str(p)) for p in sorted(PACKAGES.glob("*.yaml"))}


@pytest.fixture
async def pkg_hass(hass: HomeAssistant) -> HomeAssistant:
    """A test HA with every irrigation package merged and each domain set up."""
    config: dict = {}
    merged = merge_packages_config(hass, config, load_packages())
    if inspect.isawaitable(merged):
        merged = await merged
    if isinstance(merged, dict):
        config = merged
    for domain in DOMAINS:
        if domain in config:
            assert await async_setup_component(hass, domain, {domain: config[domain]}), domain
    await hass.async_block_till_done()
    return hass


class Stub:
    """A mocked service: records calls, returns a settable response."""

    def __init__(self):
        self.calls = []
        self.response = {}


class Device:
    """Mock ESPHome device actions (esphome.irrigation_controller_*)."""

    def __init__(self, hass: HomeAssistant):
        self.hass = hass
        self.calls = {a: [] for a in RESPONSE_ACTIONS + VOID_ACTIONS}
        self.responses = {a: {} for a in RESPONSE_ACTIONS}
        self.zone_outputs: dict = {}

    def respond(self, action: str, data: dict) -> None:
        self.responses[action] = data

    def register(self) -> None:
        for action in VOID_ACTIONS:
            self.hass.services.async_register(
                "esphome", f"irrigation_controller_{action}", self._handler(action))
        for action in RESPONSE_ACTIONS:
            self.hass.services.async_register(
                "esphome", f"irrigation_controller_{action}", self._handler(action),
                supports_response=SupportsResponse.ONLY)

    def _handler(self, action: str):
        async def handle(call: ServiceCall):
            data = dict(call.data)
            self.calls[action].append(data)
            if action == "set_zone":
                out = data.get("zone_output", "")
                zone = data.get("zone")
                if out:
                    for z, o in self.zone_outputs.items():
                        if o == out and z != zone:
                            raise HomeAssistantError(f"DUPLICATE output {out}")
                    self.zone_outputs[zone] = out
            if action in RESPONSE_ACTIONS:
                return self.responses[action]
            return None

        return handle


@pytest.fixture
async def device(hass: HomeAssistant) -> Device:
    dev = Device(hass)
    dev.register()
    return dev


class Mocks:
    def __init__(self):
        self.weather = Stub()  # weather.get_forecasts
        self.ai_task = Stub()  # ai_task.generate_data
        self.conversation = Stub()  # conversation.process
        self.calendar = Stub()  # calendar.create_event
        self.notify = Stub()  # notify.mobile_app_test
        self.notify2 = Stub()  # notify.mobile_app_test2


@pytest.fixture
async def mocks(hass: HomeAssistant) -> Mocks:
    m = Mocks()

    def reg(domain, name, stub, response=False):
        async def handle(call: ServiceCall):
            stub.calls.append(dict(call.data))
            return stub.response if response else None

        hass.services.async_register(
            domain, name, handle,
            supports_response=SupportsResponse.ONLY if response else SupportsResponse.NONE)

    reg("weather", "get_forecasts", m.weather, response=True)
    reg("ai_task", "generate_data", m.ai_task, response=True)
    reg("conversation", "process", m.conversation, response=True)
    reg("calendar", "create_event", m.calendar)
    reg("notify", "mobile_app_test", m.notify)
    reg("notify", "mobile_app_test2", m.notify2)
    return m
