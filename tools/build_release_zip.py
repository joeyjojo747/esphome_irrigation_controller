#!/usr/bin/env python3
"""Build the release ZIP and extract release notes from CHANGELOG.md.

Usage:
    python tools/build_release_zip.py [--out DIR] [--notes FILE]

The ZIP is named esphome_irrigation_controller-<VERSION>.zip and mirrors the Home
Assistant /config layout so it installs by copying folders. Entries are sorted and
timestamps fixed, so the same tree always gives the same bytes.
"""

from __future__ import annotations

import argparse
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Directories copied whole (minus exclusions) and single files at the top level.
INCLUDE_DIRS = ("esphome", "packages", "www", "dashboards", "floorplan")
INCLUDE_FILES = ("INSTALL_CHECKLIST.md", "LICENSE")

# Never shipped. secrets.yaml.example is a different name and is kept.
EXCLUDE_NAMES = {"secrets.yaml"}
EXCLUDE_DIRS = {".esphome", "reference_docs", "__pycache__", ".superpowers", ".git", ".pytest_cache"}

FIXED_TIME = (2026, 1, 1, 0, 0, 0)


def read_version(root: Path = ROOT) -> str:
    return (root / "VERSION").read_text(encoding="utf-8").strip()


def _excluded(rel: Path) -> bool:
    return rel.name in EXCLUDE_NAMES or any(part in EXCLUDE_DIRS for part in rel.parts)


def collect(root: Path) -> list[Path]:
    """Return sorted repo-relative paths of every file that goes in the ZIP."""
    found: list[Path] = []
    for name in INCLUDE_DIRS:
        base = root / name
        if not base.is_dir():
            raise FileNotFoundError(f"missing directory: {name}/")
        for path in base.rglob("*"):
            rel = path.relative_to(root)
            if path.is_file() and not _excluded(rel):
                found.append(rel)
    for name in INCLUDE_FILES:
        if not (root / name).is_file():
            raise FileNotFoundError(f"missing file: {name}")
        found.append(Path(name))
    return sorted(found, key=lambda p: p.as_posix())


def build(root: Path = ROOT, out_dir: Path | None = None) -> Path:
    out_dir = Path(out_dir) if out_dir else root / "build"
    out_dir.mkdir(parents=True, exist_ok=True)
    zip_path = out_dir / f"esphome_irrigation_controller-{read_version(root)}.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for rel in collect(root):
            info = zipfile.ZipInfo(rel.as_posix(), FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            zf.writestr(info, (root / rel).read_bytes())
    return zip_path


def changelog_section(text: str, version: str) -> str:
    """Return the body of the '## [version]' section of a Keep a Changelog file."""
    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if re.match(rf"^## \[{re.escape(version)}\]", line):
            start = i + 1
            break
    if start is None:
        raise ValueError(f"no CHANGELOG section for {version}")
    body = []
    for line in lines[start:]:
        if line.startswith("## ") or re.match(r"^\[[^\]]+\]:\s", line):
            break
        body.append(line)
    return "\n".join(body).strip() + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=None, help="output directory (default build/)")
    parser.add_argument("--notes", type=Path, default=None, help="also write the CHANGELOG section to this file")
    args = parser.parse_args(argv)
    print(build(ROOT, args.out))
    if args.notes:
        notes = changelog_section((ROOT / "CHANGELOG.md").read_text(encoding="utf-8"), read_version(ROOT))
        args.notes.write_text(notes, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
