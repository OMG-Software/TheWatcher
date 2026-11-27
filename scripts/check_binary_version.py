#!/usr/bin/env python3
"""Check that built binaries report the Meson project version.

Usage: check_binary_version.py <expected-version> <target> [<target> ...]

<target> is a fixed name (server or agent); the script maps it to a path
relative to the current directory, which Meson sets to the build root through
`workdir`. No command-line value is ever used as a program path. Each binary is
run as `<binary> --version` and must exit 0 and print the expected version as a
whole word on its first line. Used as a `meson test`.
"""
import pathlib
import re
import subprocess
import sys

# Build-root-relative locations of the binaries Meson builds.
TARGETS = {
    "server": pathlib.Path("server") / "TheWatcherServer",
    "agent": pathlib.Path("agent") / "TheWatcherAgent",
}
SUFFIX = ".exe" if sys.platform == "win32" else ""


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    expected = argv[1]
    names = argv[2:]
    unknown = [name for name in names if name not in TARGETS]
    if unknown:
        print(f"unknown target(s) {unknown}; choose from {sorted(TARGETS)}", file=sys.stderr)
        return 2
    pattern = re.compile(r"(?<![\w.])" + re.escape(expected) + r"(?![\w.])")
    failed = False
    for name in names:
        binary = pathlib.Path.cwd() / (str(TARGETS[name]) + SUFFIX)
        # List form, no shell; the path comes from the constant table above.
        result = subprocess.run([str(binary), "--version"], capture_output=True, text=True, timeout=60, check=False)
        lines = result.stdout.splitlines()
        first = lines[0] if lines else ""
        ok = result.returncode == 0 and pattern.search(first) is not None
        print(f"{name} --version -> rc={result.returncode} {first!r}")
        if not ok:
            print(f"FAIL: expected exit 0 and version {expected} on the first line", file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
