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
