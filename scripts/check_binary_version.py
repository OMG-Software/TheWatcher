#!/usr/bin/env python3
"""Check that built binaries report the Meson project version.

Usage: check_binary_version.py <expected-version> <binary> [<binary> ...]

Runs `<binary> --version` and fails unless it exits 0 and prints the expected
version as a whole word on its first line. Used as a `meson test`.
"""
import re
import subprocess
import sys


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    expected = argv[1]
    failed = False
    for binary in argv[2:]:
        result = subprocess.run([binary, "--version"], capture_output=True, text=True, timeout=60)
        lines = result.stdout.splitlines()
        first = lines[0] if lines else ""
        ok = result.returncode == 0 and re.search(r"(?<![\w.])" + re.escape(expected) + r"(?![\w.])", first)
        print(f"{binary} --version -> rc={result.returncode} {first!r}")
        if not ok:
            print(f"FAIL: expected exit 0 and version {expected} on the first line", file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
