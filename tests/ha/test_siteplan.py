"""Static checks on the SVG site plan and its ha-floorplan rules."""

import re
import xml.etree.ElementTree as ET
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
SVG = ROOT / "www" / "irrigation" / "siteplan.svg"
CSS = ROOT / "www" / "irrigation" / "siteplan.css"
FLOORPLAN = ROOT / "floorplan" / "irrigation-floorplan.yaml"

HEAD_CLASSES = {"head-spray", "head-rotor", "head-rotator", "head-drip", "head-bubbler"}


def _root():
    return ET.parse(SVG).getroot()


def _local(tag):
    return tag.rsplit("}", 1)[-1]


def _elements():
    return list(_root().iter())


def _by_id():
    return {e.get("id"): e for e in _elements() if e.get("id")}


def _floorplan():
    return yaml.safe_load(FLOORPLAN.read_text(encoding="utf-8"))


def _rules():
    return _floorplan()["config"]["rules"]


def test_svg_namespace_and_viewbox():
    root = _root()
    assert root.tag == "{http://www.w3.org/2000/svg}svg"
    assert root.get("viewBox") == "0 0 1600 1000"


def test_required_layers():
    ids = _by_id()
    for layer in ("layer-background", "layer-zones", "layer-heads", "layer-controllers"):
        assert layer in ids and _local(ids[layer].tag) == "g", layer
    img = ids["satellite"]
    assert _local(img.tag) == "image"
    assert img.get("href") == "/local/irrigation/satellite.jpg"
    assert "legend" in ids
    assert "controller-1" in ids


def test_zone_ids_pattern_and_data_zone():
    zones = [e for i, e in _by_id().items() if re.fullmatch(r"zone-\d+", i)]
    assert len(zones) >= 6
    for el in zones:
        m = re.fullmatch(r"zone-(\d{2})", el.get("id"))
        assert m, el.get("id")
        n = int(m.group(1))
        assert 1 <= n <= 16
        assert el.get("data-zone") == str(n)
        assert _local(el.tag) == "polygon"


def test_head_ids_and_classes_valid():
    zone_nums = {i[5:] for i in _by_id() if re.fullmatch(r"zone-\d\d", i)}
    heads = [e for i, e in _by_id().items() if i.startswith("head-")]
    assert len(heads) >= 2 * len(zone_nums)
    per_zone = {}
    for el in heads:
        m = re.fullmatch(r"head-(\d\d)-(\d\d)", el.get("id"))
        assert m, el.get("id")
        assert m.group(1) in zone_nums
        per_zone.setdefault(m.group(1), 0)
        per_zone[m.group(1)] += 1
        classes = set((el.get("class") or "").split())
        assert len(classes & HEAD_CLASSES) == 1, el.get("id")
    assert all(c >= 2 for c in per_zone.values())
    assert set(per_zone) == zone_nums


def test_legend_has_every_head_type():
    legend = _by_id()["legend"]
    used = set()
    for e in legend.iter():
        used |= set((e.get("class") or "").split()) & HEAD_CLASSES
    assert used == HEAD_CLASSES


def test_css_defines_state_and_head_classes():
    css = CSS.read_text(encoding="utf-8")
    for cls in ("zone-idle", "zone-running", "zone-disabled", "zone-alarm", *HEAD_CLASSES):
        assert "." + cls in css, cls
    assert "@keyframes" in css


def test_floorplan_rules_cover_every_zone_in_svg():
    svg_zones = sorted(i for i in _by_id() if re.fullmatch(r"zone-\d\d", i))
    assert svg_zones
    by_element = {}
    for r in _rules():
        by_element.setdefault(r.get("element"), []).append(r)
    for zid in svg_zones:
        n = int(zid[5:])
        rules = by_element.get(zid)
        assert rules, zid
        text = yaml.dump(rules)
        assert f"switch.irrigation_zone_{n}" in text
        assert "binary_sensor.irrigation_flow_alarm" in text
        for cls in ("zone-running", "zone-idle", "zone-alarm", "zone-disabled"):
            assert cls in text
        tap = next(r["tap_action"] for r in rules if "tap_action" in r)
        assert tap["service"] == "script.irrigation_siteplan_run_zone"
        assert tap["service_data"] == {"zone": n, "minutes": 5}
        assert tap.get("confirmation")


def test_floorplan_controller_tooltip_and_card():
    fp = _floorplan()
    assert fp["type"] == "custom:floorplan-card"
    assert fp["config"]["image"].endswith("/irrigation/siteplan.svg")
    assert fp["config"]["stylesheet"].endswith("/irrigation/siteplan.css")
    rules = [r for r in _rules() if r.get("element") == "controller-1"]
    assert rules and "sensor.irrigation_status" in yaml.dump(rules)
    assert rules[0]["tap_action"]["action"] == "more-info"
    assert rules[0]["hover_action"]["action"] == "hover-info"
    assert "tooltip_filter" not in yaml.dump(rules)


def test_install_doc_has_site_plan_section():
    text = (ROOT / "docs" / "install.md").read_text(encoding="utf-8")
    assert "## Site plan" in text
    assert "satellite.jpg" in text and "/config/www/irrigation/" in text
