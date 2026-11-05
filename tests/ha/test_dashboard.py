"""Static checks on dashboards/irrigation-dashboard.yaml (no Home Assistant needed)."""

import re
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
DASHBOARD = ROOT / "dashboards" / "irrigation-dashboard.yaml"
FLOORPLAN = ROOT / "floorplan" / "irrigation-floorplan.yaml"
FIRMWARE = ROOT / "esphome" / "irrigation-controller"
PACKAGES = ROOT / "packages" / "irrigation"

VIEW_PATHS = ["overview", "siteplan", "calendar", "programs", "zones", "water", "settings"]

# Entity domains a device (ESPHome) or a package can create.
DOMAINS = ("sensor", "binary_sensor", "text_sensor", "switch", "select", "number", "text",
           "datetime", "button")


class _Loader(yaml.SafeLoader):
    """SafeLoader that tolerates ESPHome tags (!lambda, !secret, ...)."""


def _tagged(loader, tag_suffix, node):
    if isinstance(node, yaml.ScalarNode):
        return loader.construct_scalar(node)
    if isinstance(node, yaml.SequenceNode):
        return loader.construct_sequence(node, deep=True)
    return loader.construct_mapping(node, deep=True)


_Loader.add_multi_constructor("!", _tagged)


def _load(path):
    with open(path, encoding="utf-8") as f:
        return yaml.load(f, Loader=_Loader) or {}


def _as_list(v):
    if v is None:
        return []
    return v if isinstance(v, list) else [v]


def slug(text):
    """Home Assistant style slug: lower case, runs of non-alphanumerics become one underscore."""
    return re.sub(r"[^a-z0-9]+", "_", str(text).lower()).strip("_")


def _device_entities():
    """Entity ids the firmware creates: <domain>.irrigation_<slug of name>.

    The device friendly_name is "Irrigation", so every entity id starts with irrigation_.
    Entities marked internal: true are not exposed to HA and are skipped.
    """
    ids = set()
    for path in FIRMWARE.glob("*.yaml"):
        data = _load(path)
        for domain in DOMAINS:
            for item in _as_list(data.get(domain)):
                if not isinstance(item, dict) or item.get("internal") is True:
                    continue
                name = item.get("name")
                if not name:
                    continue
                # ESPHome text_sensor entities are HA sensors.
                ha_domain = "sensor" if domain == "text_sensor" else domain
                ids.add(f"{ha_domain}.irrigation_{slug(name)}")
        # sprinkler valves become the "Zone N" switches (the valve_switch name)
        for ctrl in _as_list(data.get("sprinkler")):
            for valve in _as_list(ctrl.get("valves")):
                ids.add(f"switch.irrigation_{slug(valve['valve_switch'])}")
    return ids


def _device_actions():
    data = _load(FIRMWARE / "api_actions.yaml")
    return {f"esphome.irrigation_controller_{a['action']}"
            for a in _as_list(data["api"]["actions"])}


def _package_entities():
    """Entities and scripts defined in packages/irrigation/*.yaml."""
    ids = set()
    for path in PACKAGES.glob("*.yaml"):
        data = _load(path)
        for domain in ("input_text", "input_select", "input_datetime", "input_number",
                       "input_boolean", "script", "utility_meter"):
            keys = (data.get(domain) or {}).keys()
            for key in keys:
                ha_domain = "sensor" if domain == "utility_meter" else domain
                ids.add(f"{ha_domain}.{key}")
        for auto in _as_list(data.get("automation")):
            ids.add(f"automation.{slug(auto['alias'])}")
        for block in _as_list(data.get("template")):
            for domain in ("sensor", "binary_sensor"):
                for item in _as_list(block.get(domain)):
                    ids.add(f"{domain}.{slug(item['name'])}")
    return ids


# calendar.irrigation is the user's Local Calendar (created by hand, see docs/install.md),
# so no package defines it.
USER_CREATED = {"calendar.irrigation"}


def _allow_list():
    return _device_entities() | _device_actions() | _package_entities() | USER_CREATED


def _dashboard():
    return _load(DASHBOARD)


def test_views_present():
    views = _dashboard()["views"]
    assert [v["path"] for v in views] == VIEW_PATHS


def test_referenced_entities_exist():
    allowed = _allow_list()
    text = DASHBOARD.read_text(encoding="utf-8")
    # <domain>.irrigation or <domain>.irrigation_<name>, also inside comments and templates
    found = set(re.findall(r"\b[a-z_]+\.irrigation(?:_[a-z0-9_]+)?\b", text))
    assert found, "dashboard references no irrigation entities"
    unknown = sorted(found - allowed)
    assert not unknown, f"not a device entity, package entity or script: {unknown}"


def test_allow_list_sanity():
    # Guards the allow-list itself: the slug rule and the package parsing.
    allowed = _allow_list()
    for eid in ("sensor.irrigation_total_water", "sensor.irrigation_rain_last_24h",
                "button.irrigation_rain_delay_24h", "select.irrigation_zone_16_output",
                "switch.irrigation_zone_16", "number.irrigation_mm_per_tip",
                "datetime.irrigation_program_editor_start_4", "sensor.irrigation_last_error",
                "script.irrigation_apply_defaults", "sensor.irrigation_water_today",
                "sensor.irrigation_cost_year", "sensor.irrigation_upcoming",
                "sensor.irrigation_zone_16_water_month", "input_select.irrigation_ai_mode",
                "esphome.irrigation_controller_run_program"):
        assert eid in allowed, eid
    # internal device entities are not exposed
    assert "switch.irrigation_zone_1_relay" not in allowed


def test_siteplan_card_matches_floorplan_file():
    view = next(v for v in _dashboard()["views"] if v["path"] == "siteplan")
    assert view.get("panel") is True
    assert view["cards"] == [_load(FLOORPLAN)]


def test_ultra_card_placeholders_marked():
    text = DASHBOARD.read_text(encoding="utf-8")
    marker = "# ULTRA CARD: replace with Ultra Card visual-editor export"
    assert text.count(marker) == 3
