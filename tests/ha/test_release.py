"""Release packaging checks: versions, README, install checklist, release ZIP."""

import importlib.util
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _load_builder():
    spec = importlib.util.spec_from_file_location("build_release_zip", ROOT / "tools" / "build_release_zip.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _text(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def test_versions_match():
    version = _text("VERSION").strip()
    assert re.fullmatch(r"\d+\.\d+\.\d+", version)
    assert _text("esphome/irrigation-controller/VERSION").strip() == version

    wrapper = _text("esphome/irrigation-controller.yaml")
    m = re.search(r'^\s*version:\s*"([^"]+)"', wrapper, re.M)
    assert m and m.group(1) == version

    helpers = _text("packages/irrigation/irrigation_helpers.yaml")
    m = re.search(r'irrigation_package_version:.*?initial:\s*"([^"]+)"', helpers, re.S)
    assert m and m.group(1) == version

    assert f"## [{version}]" in _text("CHANGELOG.md")


README_HEADINGS = [
    "## What it is",
    "## How it compares",
    "## Key features",
    "### Configuration",
    "### Scheduling",
    "### Weather & AI",
    "### Flow & water cost",
    "### Rain sensor",
    "### Calendar",
    "### Site plan",
    "### Notifications",
    "### Dashboards",
    "## Hardware",
    "## Quick start",
    "## Updating",
    "## Offline behaviour",
    "## Licence and credits",
]

# Screenshots come from the bench rig and the installed dashboard (Task 20 Step 2).
# Until the PNGs exist, each place carries an HTML comment so none is forgotten.
SCREENSHOTS = [
    "header",
    "configuration",
    "scheduling",
    "weather-ai",
    "flow-cost",
    "rain-sensor",
    "calendar",
    "site-plan",
    "notifications",
    "dashboards",
    "wiring",
]


def test_readme_sections():
    readme = _text("README.md")
    headings = {line.strip() for line in readme.splitlines() if line.startswith("#")}
    assert any(h.startswith("# ") for h in headings), "title"
    for h in README_HEADINGS:
        assert h in headings, f"missing heading {h}"

    # Order of the sections.
    lines = [line.strip() for line in readme.splitlines()]
    positions = [lines.index(h) for h in README_HEADINGS]
    assert positions == sorted(positions)

    # The two design goals, the three comparison projects and the credits.
    lowered = readme.lower()
    assert "no yaml reload" in lowered and "reflash" in lowered
    assert "wi-fi" in lowered and "autonomous" in lowered
    for name in ("ESPHome Sprinkler", "Irrigation Unlimited", "ESPyRain"):
        assert name in readme
    assert "Apache-2.0" in readme
    assert "INSTALL_CHECKLIST.md" in readme

    # Every image link that is present resolves.
    for target in re.findall(r"!\[[^\]]*\]\(([^)\s]+)", readme):
        if target.startswith("http"):
            continue
        assert (ROOT / target).is_file(), f"broken image link {target}"

    # Each screenshot is either an image that exists or a placeholder comment.
    for name in SCREENSHOTS:
        placeholder = f"<!-- SCREENSHOT: {name}.png (Task 20 Step 2, after Task 9/19) -->"
        image = (ROOT / "images" / "screenshots" / f"{name}.png").is_file()
        assert placeholder in readme or (image and f"images/screenshots/{name}.png" in readme), name


CHECKLIST_ROOTS = (
    "esphome/",
    "packages/",
    "www/",
    "dashboards/",
    "floorplan/",
    "tools/",
    "docs/",
)
CHECKLIST_FILES = {"INSTALL_CHECKLIST.md", "LICENSE", "README.md", "CHANGELOG.md", "VERSION"}


def test_checklist_paths_exist():
    checklist = _text("INSTALL_CHECKLIST.md")
    paths = [p for p in re.findall(r"`([^`\n]+)`", checklist) if p.startswith(CHECKLIST_ROOTS) or p in CHECKLIST_FILES]
    assert len(paths) >= 6, "checklist should name the folders and files to copy"
    for p in paths:
        assert (ROOT / p.rstrip("/")).exists(), f"checklist names a missing path: {p}"
    # Every step ends with a Check line, and the secrets note is there.
    steps = re.findall(r"^## Step \d+", checklist, re.M)
    assert len(steps) == 8
    assert len(re.findall(r"^\*\*Check:\*\*", checklist, re.M)) == len(steps)
    assert "rotate" in checklist.lower() and "before the first flash" in checklist.lower()


def test_zip_excludes_secrets(tmp_path):
    builder = _load_builder()

    # Plant files that must never ship into a copy of the layout.
    fake = tmp_path / "repo"
    for rel in builder.INCLUDE_DIRS:
        (fake / rel).mkdir(parents=True)
        (fake / rel / "keep.txt").write_text("x")
    (fake / "esphome" / "secrets.yaml.example").write_text("dummy")
    (fake / "esphome" / "secrets.yaml").write_text("real")
    (fake / "esphome" / "sub").mkdir()
    (fake / "esphome" / "sub" / "secrets.yaml").write_text("real")
    (fake / "esphome" / ".esphome").mkdir()
    (fake / "esphome" / ".esphome" / "build.txt").write_text("x")
    (fake / "packages" / "__pycache__").mkdir()
    (fake / "packages" / "__pycache__" / "a.pyc").write_text("x")
    (fake / "reference_docs").mkdir()
    (fake / "reference_docs" / "custom.yaml").write_text("x")
    (fake / "www" / "reference_docs").mkdir()
    (fake / "www" / "reference_docs" / "y.txt").write_text("x")
    (fake / ".superpowers").mkdir()
    (fake / ".superpowers" / "plan.md").write_text("x")
    (fake / "VERSION").write_text("9.9.9\n")
    (fake / "LICENSE").write_text("lic")
    (fake / "INSTALL_CHECKLIST.md").write_text("list")

    zpath = builder.build(fake, tmp_path / "out")
    assert zpath.name == "esphome_irrigation_controller-9.9.9.zip"
    names = zipfile.ZipFile(zpath).namelist()
    assert names == sorted(names)
    assert "esphome/secrets.yaml.example" in names
    assert not [n for n in names if n.endswith("secrets.yaml")]
    assert not [n for n in names if ".esphome" in n or "reference_docs" in n or "__pycache__" in n or ".superpowers" in n]

    # Deterministic: a second build is byte-identical.
    first = zpath.read_bytes()
    assert builder.build(fake, tmp_path / "out2").read_bytes() == first

    # The real repo ZIP has the required top-level content.
    real = builder.build(ROOT, tmp_path / "real")
    real_names = zipfile.ZipFile(real).namelist()
    tops = {n.split("/")[0] for n in real_names}
    assert tops == {"esphome", "packages", "www", "dashboards", "floorplan", "INSTALL_CHECKLIST.md", "LICENSE"}
    assert "esphome/secrets.yaml.example" in real_names
    assert "esphome/irrigation-controller.yaml" in real_names
    assert "esphome/irrigation-controller/VERSION" in real_names
    assert not [n for n in real_names if n.endswith("/secrets.yaml") or ".esphome/" in n or "reference_docs" in n]


def test_changelog_section():
    builder = _load_builder()
    version = _text("VERSION").strip()
    notes = builder.changelog_section(_text("CHANGELOG.md"), version)
    assert notes.strip() and not notes.startswith("##")
    assert "Known limitations" in notes


def test_release_workflow():
    wf = _text(".github/workflows/release.yml")
    assert "'v*'" in wf or '"v*"' in wf
    assert "softprops/action-gh-release@v2" in wf
    assert "contents: write" in wf
    assert "tests/ha/test_release.py" in wf
