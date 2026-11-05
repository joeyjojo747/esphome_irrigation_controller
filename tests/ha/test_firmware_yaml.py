"""Static assertions on the device firmware YAML (no ESPHome needed)."""

import re
import subprocess
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
PKG = ROOT / "esphome" / "irrigation-controller"
WRAPPER = ROOT / "esphome" / "irrigation-controller.yaml"


class _Loader(yaml.SafeLoader):
    """SafeLoader that keeps ESPHome tags as readable strings ("!secret x")."""


def _tagged(loader, tag_suffix, node):
    tag = "!" + tag_suffix
    if isinstance(node, yaml.ScalarNode):
        return f"{tag} {loader.construct_scalar(node)}"
    if isinstance(node, yaml.SequenceNode):
        return {tag: loader.construct_sequence(node, deep=True)}
    return {tag: loader.construct_mapping(node, deep=True)}


_Loader.add_multi_constructor("!", _tagged)


def load(path):
    with open(path, encoding="utf-8") as f:
        return yaml.load(f, Loader=_Loader)


def as_list(v):
    if v is None:
        return []
    return v if isinstance(v, list) else [v]


def all_yaml_files():
    files = list(PKG.glob("*.yaml")) + [WRAPPER]
    files += list((ROOT / "tests" / "compile").glob("with_expander.yaml"))
    return files


def test_autonomy_settings():
    base = load(PKG / "base.yaml")
    assert base["api"]["reboot_timeout"] == "0s"
    assert base["wifi"]["reboot_timeout"] == "0s"


def _core_switches():
    out = load(PKG / "outputs.yaml")
    return [s for s in as_list(out.get("switch")) if s.get("platform") == "irrigation_core"]


def test_relays_always_off():
    switches = _core_switches()
    assert len(switches) == 17
    slots = sorted(str(s["slot"]) for s in switches)
    assert slots == sorted([str(i) for i in range(1, 17)] + ["master"])
    for s in switches:
        assert s["restore_mode"] == "ALWAYS_OFF", s
        assert s["internal"] is True, s


def test_no_gpio_relay_switches():
    for path in all_yaml_files():
        data = load(path) or {}
        for s in as_list(data.get("switch")):
            assert s.get("platform") != "gpio", f"{path} declares a gpio switch"


def _ints(s):
    return [int(p) for p in str(s).replace(",", " ").split()]


def test_pins_not_reused():
    subs = load(WRAPPER)["substitutions"]
    names = ["flow_pin", "rain_pin", "start_pin", "stop_pin", "led_pin", "sda_pin", "scl_pin"]
    pins = [int(subs[n]) for n in names]
    assert len(set(pins)) == len(pins), "sensor pins must all differ"
    candidates = _ints(subs["output_candidates"])
    assert not set(pins) & set(candidates)
    core = load(PKG / "core.yaml")["irrigation_core"]
    reserved = [str(r) for r in as_list(core["reserved_pins"])]
    for n in names:
        assert "${" + n + "}" in reserved, f"{n} missing from reserved_pins"
    assert core["output_candidates"] == "${output_candidates}"
    # first-boot defaults must be candidates and must not clash
    defaults = _ints(subs["default_outputs"]) + [int(subs["default_master_output"])]
    assert set(defaults) <= set(candidates)
    assert len(set(defaults)) == len(defaults)


def _walk(node):
    if isinstance(node, dict):
        yield node
        for v in node.values():
            yield from _walk(v)
    elif isinstance(node, list):
        for v in node:
            yield from _walk(v)


def test_buttons_local():
    sensors = load(PKG / "sensors.yaml")
    found = {}
    for bs in as_list(sensors.get("binary_sensor")):
        pin = bs.get("pin", {})
        num = pin.get("number") if isinstance(pin, dict) else pin
        if num in ("${start_pin}", "${stop_pin}"):
            found[num] = bs
    assert set(found) == {"${start_pin}", "${stop_pin}"}
    for bs in found.values():
        text = yaml.dump(bs)
        assert "id(irr).button_press(" in text
        assert "homeassistant." not in text


# The only secret-looking literal allowed in tracked files: the dummy in secrets.yaml.example.
DUMMY_API_KEY = "ZHVtbXkta2V5LWZvci1jaS1ub3QtYS1zZWNyZXQtMDA="


def test_no_plaintext_secrets():
    base = load(PKG / "base.yaml")
    assert base["api"]["encryption"]["key"] == "!secret api_key"
    otas = as_list(base["ota"])
    assert otas and all(o["password"] == "!secret ota_password" for o in otas)
    example = load(ROOT / "esphome" / "secrets.yaml.example")
    assert example["api_key"] == DUMMY_API_KEY
    files = subprocess.run(
        ["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.splitlines()
    key_re = re.compile(r"key:\s*[\"']?([A-Za-z0-9+/]{43}=)")
    pw_re = re.compile(r"password:\s*[\"']?([0-9a-fA-F]{32})(?![0-9a-fA-F])")
    allowed = {DUMMY_API_KEY}
    for rel in files:
        p = ROOT / rel
        if not p.is_file():
            continue
        try:
            text = p.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for m in key_re.finditer(text):
            assert m.group(1) in allowed, f"{rel} contains a base64 key literal"
        for m in pw_re.finditer(text):
            assert m.group(1) in allowed, f"{rel} contains a 32-hex password literal"


def test_settings_switches_do_not_restore():
    # The core is the source of truth; a restored switch would write stale state back.
    sw = [
        s
        for s in as_list(load(PKG / "settings.yaml").get("switch"))
        if s.get("platform") == "template"
    ]
    assert len(sw) == 2
    for s in sw:
        assert s["restore_mode"] == "DISABLED", s


def test_fixture_matches_wrapper_substitutions():
    wrapper = load(WRAPPER)["substitutions"]
    fixture = load(ROOT / "tests" / "compile" / "with_expander.yaml")["substitutions"]
    keys = [
        "timezone", "output_candidates", "flow_pin", "rain_pin", "start_pin",
        "stop_pin", "led_pin", "sda_pin", "scl_pin", "default_outputs",
        "default_master_output",
    ]
    for k in keys:
        assert fixture[k] == wrapper[k], k


def test_output_selects_are_config():
    out = load(PKG / "outputs.yaml")
    sels = [s for s in as_list(out["select"]) if s.get("platform") == "irrigation_core"]
    assert len(sels) == 17
    assert all(s["entity_category"] == "config" for s in sels)


def test_sixteen_valves():
    spr = load(PKG / "sprinkler.yaml")["sprinkler"]
    ctrl = spr[0] if isinstance(spr, list) else spr
    assert len(ctrl["valves"]) == 16


# ---- Task 8: editors and API actions ----

CORE_CPP = PKG / "components" / "irrigation_core" / "irrigation_core.cpp"

EXPECTED_ACTIONS = {
    "run_zone": {"zone": "int", "minutes": "int"},
    "run_program": {"program": "int"},
    "stop_all": {},
    "rain_delay": {"hours": "int"},
    "set_weather_adjust": {"pct": "int", "skip": "bool", "reason": "string", "valid_hours": "int"},
    "set_program": {
        "program": "int", "name": "string", "enabled": "bool", "mode": "int",
        "weekday_mask": "int", "interval_days": "int", "days_until_first": "int",
        "starts": "int[]", "zone_minutes": "int[]", "seasonal_pct": "int",
        "cycle_soak": "bool", "weather_adjust": "bool",
    },
    "set_zone": {
        "zone": "int", "name": "string", "enabled": "bool", "head": "int", "precip": "float",
        "soil": "int", "slope": "int", "sun": "int", "plant": "int", "area": "float",
        "max_cycle_min": "int", "soak_min": "int", "baseline": "float", "high_pct": "int",
        "low_pct": "int", "action": "int", "zone_output": "string",
    },
    "set_settings": {
        "zone_count": "int", "controller_enabled": "bool", "monthly_pct": "int[]",
        "pulses_per_litre": "float", "leak_lpm": "float", "settle_s": "int", "rain_type": "int",
        "mm_per_tip": "float", "rain_skip_mm": "float", "rain_stops": "bool",
        "hard_cap_min": "int", "start_short_fn": "int", "start_long_fn": "int",
        "stop_short_fn": "int", "stop_long_fn": "int", "button_program": "int",
        "master_output": "string", "relay_active_high": "bool",
    },
    "get_plan": {"days": "int"},
    "get_run_log": {"since": "int"},
    "get_config": {},
}
RESPONSE_ACTIONS = {"get_plan", "get_run_log", "get_config"}


def _actions():
    acts = load(PKG / "api_actions.yaml")["api"]["actions"]
    return {a["action"]: a for a in acts}


def test_packages_enabled():
    pkgs = load(PKG / "package.yaml")["packages"]
    for k in ("editor_program", "editor_zone", "api_actions"):
        assert pkgs[k] == f"!include {k}.yaml", k


def test_actions_present():
    acts = _actions()
    assert set(acts) == set(EXPECTED_ACTIONS)
    for name, args in EXPECTED_ACTIONS.items():
        assert (acts[name].get("variables") or {}) == args, name
        if name in RESPONSE_ACTIONS:
            assert acts[name]["supports_response"] == "only", name
        else:
            assert "supports_response" not in acts[name], name


def _config_json_keys():
    src = CORE_CPP.read_text(encoding="utf-8")
    start = src.index("std::string IrrigationCore::config_json()")
    body = src[start : src.index("return j;", start)]
    keys = re.findall(r'\\"(\w+)\\":', body)
    iz, isett = keys.index("zones"), keys.index("settings")
    return keys[1:iz], keys[iz + 1 : isett], keys[isett + 1 :]


def test_config_json_keys_match_action_arguments():
    # Backup -> restore: every config_json key is exactly an action argument, so
    # get_config output feeds set_program / set_zone / set_settings unchanged.
    progs, zones, sett = _config_json_keys()
    assert sorted(progs) == sorted(EXPECTED_ACTIONS["set_program"])
    assert sorted(zones) == sorted(EXPECTED_ACTIONS["set_zone"])
    assert sorted(sett) == sorted(EXPECTED_ACTIONS["set_settings"])
    assert len(set(progs)) == len(progs) and len(set(zones)) == len(zones)


def _entity_names(doc):
    out = []
    for domain in ("text", "select", "switch", "number", "datetime", "button"):
        for e in as_list(doc.get(domain)):
            out.append((domain, e["name"], e))
    return out


def test_editor_save_buttons():
    for fname, prefix in (("editor_program.yaml", "Program Editor"), ("editor_zone.yaml", "Zone Editor")):
        names = {n for d, n, _ in _entity_names(load(PKG / fname)) if d == "button"}
        assert f"{prefix} Save" in names and f"{prefix} Revert" in names, fname
    zb = {n for d, n, _ in _entity_names(load(PKG / "editor_zone.yaml")) if d == "button"}
    assert {"Zone Editor Learn Flow", "Zone Editor Test Run"} <= zb


def test_editor_entities_are_config_except_buttons():
    for fname in ("editor_program.yaml", "editor_zone.yaml"):
        for d, n, e in _entity_names(load(PKG / fname)):
            if " Name" in n and n.split()[0] in ("Program", "Zone") and n.split()[1].isdigit():
                continue
            if d == "button" and n.endswith(("Save", "Revert", "Learn Flow", "Test Run")):
                assert "entity_category" not in e, n
            elif e["platform"] != "irrigation_core":  # the core select sets its own category
                assert e.get("entity_category") == "config", n


def test_name_texts_wired_to_core():
    for fname, key, label, count, prefix in (
        ("editor_program.yaml", "program_name_texts", "Program", 8, "prog_name_"),
        ("editor_zone.yaml", "zone_name_texts", "Zone", 16, "zone_name_"),
    ):
        doc = load(PKG / fname)
        ids = [f"{prefix}{n}" for n in range(1, count + 1)]
        assert doc["irrigation_core"][key] == ids
        texts = {t["id"]: t for t in as_list(doc["text"]) if "id" in t}
        for n, i in enumerate(ids, 1):
            t = texts[i]
            assert t["name"] == f"{label} {n} Name"
            assert t["restore_value"] is True and t["max_length"] == 24


def test_zone_editor_output_uses_core_select():
    sel = {s["name"]: s for s in as_list(load(PKG / "editor_zone.yaml")["select"])}
    out = sel["Zone Editor Output"]
    assert out["platform"] == "irrigation_core"
    assert out["id"] == "ze_output"


def test_program_editor_entity_set():
    names = {n for _, n, _ in _entity_names(load(PKG / "editor_program.yaml"))}
    expected = {f"Program Editor {d}" for d in ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")}
    expected |= {f"Program Editor Start {n}" for n in range(1, 5)}
    expected |= {f"Program Editor Start {n} Active" for n in range(1, 5)}
    expected |= {f"Program Editor Zone {n} Minutes" for n in range(1, 17)}
    expected |= {
        "Program Editor Program", "Program Editor Enabled", "Program Editor Day Mode",
        "Program Editor Interval Days", "Program Editor Days Until First Run",
        "Program Editor Seasonal Adjust", "Program Editor Cycle Soak",
        "Program Editor Weather Adjust", "Program Editor Save", "Program Editor Revert",
    }
    assert expected <= names


def test_response_actions_report_parse_errors():
    acts = _actions()
    for name in RESPONSE_ACTIONS:
        code = acts[name]["then"][0]["api.respond"]["data"]
        assert 'root["error"]' in code, name
    # the run-log cursor must only be emitted when the records were built
    code = acts["get_run_log"]["then"][0]["api.respond"]["data"]
    assert code.index("if (ok)") < code.index('root["last"]')
    plan = acts["get_plan"]["then"][0]["api.respond"]["data"]
    assert "truncated" in plan


def test_interval_reanchor_only_when_edited():
    text = (PKG / "editor_program.yaml").read_text(encoding="utf-8")
    assert text.count("pe_dtf_dirty) = true") == 3  # day mode, interval days, days until first
    assert "&& id(pe_dtf_dirty)" in text
