#!/usr/bin/env python3
"""Fail unless every file that carries a version agrees with meson.build.

meson.build is the single source of truth. Checked against it:
  dashboard/package.json, dashboard/package-lock.json (both occurrences),
  packaging/windows/TheWatcher.iss (default AppVersion), and the newest
  released section of CHANGELOG.md.

Usage: check_version_consistency.py [repo-root]
"""
import json
import pathlib
import re
import sys


def read(root, rel):
    return (root / rel).read_text(encoding="utf-8")


def main(argv):
    root = pathlib.Path(argv[1] if len(argv) > 1 else pathlib.Path(__file__).resolve().parent.parent)
    match = re.search(r"project\((?:[^)]*?)version:\s*'([^']+)'", read(root, "meson.build"), re.S)
    if not match:
        print("could not find the project version in meson.build", file=sys.stderr)
        return 2
    expected = match.group(1)
    found = {}

    package = json.loads(read(root, "dashboard/package.json"))
    found["dashboard/package.json"] = package.get("version")

    lock = json.loads(read(root, "dashboard/package-lock.json"))
    found["dashboard/package-lock.json (version)"] = lock.get("version")
    found["dashboard/package-lock.json (packages[''])"] = lock.get("packages", {}).get("", {}).get("version")

    iss_text = read(root, "packaging/windows/TheWatcher.iss")
    default = re.search(r'#define\s+AppVersion\s+"([^"]+)"', iss_text)
    found["packaging/windows/TheWatcher.iss"] = default.group(1) if default else None

    changelog = re.search(r"^## \[(\d+\.\d+\.\d+)\]", read(root, "CHANGELOG.md"), re.M)
    found["CHANGELOG.md (newest section)"] = changelog.group(1) if changelog else None

    bad = {name: value for name, value in found.items() if value != expected}
    for name, value in found.items():
        print(f"{'ok  ' if name not in bad else 'FAIL'} {name}: {value}")
    if bad:
        print(f"meson.build says {expected}; update the files above to match.", file=sys.stderr)
        return 1
    print(f"all versions match meson.build ({expected})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
