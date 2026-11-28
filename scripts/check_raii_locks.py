#!/usr/bin/env python3
"""Fails when first-party C++ code takes or releases a lock by hand.

Project rule (docs/development.md): every lock is held through an RAII
wrapper - std::scoped_lock, std::lock_guard, std::unique_lock,
std::shared_lock or a project RAII type - so it is released however the
scope ends. A bare call such as m.lock(), lk.unlock(), std::lock(a, b) or
pthread_mutex_lock(&m) is reported. An exceptional case (for example the
inside of a project RAII type, or std::weak_ptr::lock, which is not a mutex)
is allowed only with a reason, in a comment on the same line or on a
comment-only line directly above:

    m.lock(); // raii-lock-exception: <why this cannot be RAII>

Usage: check_raii_locks.py [source-root]   (default: the current directory)
Exit status: 0 when clean, 1 when a bare call is found, 2 on a usage error.

The scan is textual: comments and string/character literals are blanked
first, so only code is matched. Standard library only, so it runs wherever
meson does.
"""

import os
import re
import sys
from collections import namedtuple
from pathlib import Path

Violation = namedtuple("Violation", ["line", "call"])

CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx", ".h", ".ipp", ".inl"}

# Directories that hold no first-party code: third-party sources and npm
# packages. Hidden directories (.git, .claude worktrees, .clwb) and meson
# build directories (recognised by their meson-private folder) are skipped too.
SKIPPED_DIRS = {"subprojects", "node_modules"}

_MEMBER_CALLS = (
    "lock",
    "unlock",
    "try_lock",
    "try_lock_for",
    "try_lock_until",
    "lock_shared",
    "unlock_shared",
    "try_lock_shared",
    "try_lock_shared_for",
    "try_lock_shared_until",
)
_PLATFORM_CALLS = (
    "pthread_mutex_lock",
    "pthread_mutex_trylock",
    "pthread_mutex_timedlock",
    "pthread_mutex_unlock",
    "pthread_rwlock_rdlock",
    "pthread_rwlock_wrlock",
    "pthread_rwlock_tryrdlock",
    "pthread_rwlock_trywrlock",
    "pthread_rwlock_unlock",
    "pthread_spin_lock",
    "pthread_spin_trylock",
    "pthread_spin_unlock",
    "mtx_lock",
    "mtx_trylock",
    "mtx_timedlock",
    "mtx_unlock",
    "EnterCriticalSection",
    "TryEnterCriticalSection",
    "LeaveCriticalSection",
    "AcquireSRWLockExclusive",
    "AcquireSRWLockShared",
    "TryAcquireSRWLockExclusive",
    "TryAcquireSRWLockShared",
    "ReleaseSRWLockExclusive",
    "ReleaseSRWLockShared",
)


def _alternation(names):
    # Longest first, so "try_lock_for" is not read as "try_lock".
    return "|".join(sorted(names, key=len, reverse=True))


# obj.lock(), ptr->unlock() and the try/shared variants. A declaration such
# as "void lock();" has no member access in front, so it is not matched.
_MEMBER_RE = re.compile(r"(?:\.|->)\s*(" + _alternation(_MEMBER_CALLS) + r")\s*\(")
# std::lock(a, b) and std::try_lock(a, b); std::lock_guard etc. do not match
# because a type name is not followed directly by "(".
_FREE_RE = re.compile(r"\bstd\s*::\s*(lock|try_lock)\s*\(")
_PLATFORM_RE = re.compile(r"\b(" + _alternation(_PLATFORM_CALLS) + r")\s*\(")
_MARKER_RE = re.compile(r"raii-lock-exception:(.*)")

_LITERAL_PREFIXES = {"u8", "u", "U", "L", "R", "u8R", "uR", "UR", "LR"}


def _blank(text):
    """The same text with every character but newlines replaced by a space."""
    return "".join("\n" if ch == "\n" else " " for ch in text)


def _end_of_quoted(source, start, quote):
    """Index just past the closing quote of a "..." or '...' literal."""
    i = start + 1
    n = len(source)
    while i < n:
        ch = source[i]
        if ch == "\\":
            i += 2
            continue
        if ch == quote or ch == "\n":  # an unterminated literal ends at the line
            return i + 1
        i += 1
    return n


def _end_of_raw(source, start):
    """Index just past a raw string R"delim(...)delim" whose quote is at start."""
    open_paren = source.find("(", start + 1)
    if open_paren < 0:
        return len(source)
    delimiter = source[start + 1 : open_paren]
    close = source.find(")" + delimiter + '"', open_paren + 1)
    return len(source) if close < 0 else close + len(delimiter) + 2


def _end_of_line_comment(source, start):
    """Index of the newline that ends a // comment (a trailing backslash continues it)."""
    i = start
    while True:
        newline = source.find("\n", i)
        if newline < 0:
            return len(source)
        if source[newline - 1] == "\\" or source[newline - 2 : newline] == "\\\r":
            i = newline + 1
            continue
        return newline


def strip_comments_and_literals(source):
    """C++ source with comments and string/char literals blanked.

    Line and column positions are kept, so a match in the result points at
    the same place in the original.
    """
    out = []
    i = 0
    n = len(source)
    while i < n:
        ch = source[i]
        if source.startswith("//", i):
            end = _end_of_line_comment(source, i)
        elif source.startswith("/*", i):
            close = source.find("*/", i + 2)
            end = n if close < 0 else close + 2
        elif ch.isalpha() or ch == "_":
            j = i + 1
            while j < n and (source[j].isalnum() or source[j] == "_"):
                j += 1
            word = source[i:j]
            if j < n and source[j] in "\"'" and word in _LITERAL_PREFIXES:
                if word.endswith("R") and source[j] == '"':
                    end = _end_of_raw(source, j)
                else:
                    end = _end_of_quoted(source, j, source[j])
                out.append(word)
                out.append(_blank(source[j:end]))
                i = end
                continue
            out.append(word)
            i = j
            continue
        elif ch.isdigit() or (ch == "." and i + 1 < n and source[i + 1].isdigit()):
            # A pp-number, which may hold digit separators (4'000'000).
            j = i + 1
            while j < n:
                if source[j].isalnum() or source[j] in "._'":
                    j += 1
                elif source[j] in "+-" and source[j - 1] in "eEpP":
                    j += 1
                else:
                    break
            out.append(source[i:j])
            i = j
            continue
        elif ch == '"' or ch == "'":
            end = _end_of_quoted(source, i, ch)
        else:
            out.append(ch)
            i += 1
            continue
        out.append(_blank(source[i:end]))
        i = end
    return "".join(out)


def _has_marker(raw_line):
    match = _MARKER_RE.search(raw_line)
    return bool(match) and match.group(1).replace("*/", "").strip() != ""


def find_violations(source):
    """Every bare lock call in one C++ source text, as (line, call) pairs."""
    code = strip_comments_and_literals(source)
    raw_lines = source.split("\n")
    code_lines = code.split("\n")
    violations = []
    for index, line in enumerate(code_lines):
        found = []
        for regex, prefix in ((_MEMBER_RE, ""), (_FREE_RE, "std::"), (_PLATFORM_RE, "")):
            found.extend((m.start(), prefix + m.group(1)) for m in regex.finditer(line))
        if not found:
            continue
        # The reason may sit on this line or on a comment-only line above;
        # a marker after code on the line above covers that line only.
        allowed = _has_marker(raw_lines[index]) or (
            index > 0 and code_lines[index - 1].strip() == "" and _has_marker(raw_lines[index - 1])
        )
        if allowed:
            continue
        violations.extend(Violation(index + 1, call) for _, call in sorted(found))
    return violations


def _source_files(root):
    for directory, subdirs, files in os.walk(root):
        subdirs[:] = sorted(
            d
            for d in subdirs
            if not d.startswith(".")
            and d not in SKIPPED_DIRS
            and not os.path.isdir(os.path.join(directory, d, "meson-private"))
        )
        for name in sorted(files):
            if Path(name).suffix in CPP_SUFFIXES:
                yield Path(directory) / name


def main(argv):
    if len(argv) > 1:
        print("usage: check_raii_locks.py [source-root]", file=sys.stderr)
        return 2
    root = Path(argv[0] if argv else ".")
    if not root.is_dir():
        print(f"check_raii_locks: {root} is not a directory", file=sys.stderr)
        return 2
    checked = 0
    failures = []
    for path in _source_files(root):
        checked += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        for violation in find_violations(text):
            failures.append((path.relative_to(root), violation))
    for relative, violation in failures:
        print(
            f"{relative}:{violation.line}: bare {violation.call}() call: hold the lock through an RAII "
            "wrapper (std::scoped_lock, std::lock_guard, std::unique_lock, std::shared_lock), or "
            "justify an exceptional case with '// raii-lock-exception: <reason>' on the same line "
            "or a comment-only line above"
        )
    if failures:
        print(f"check_raii_locks: {len(failures)} bare lock call(s) in {checked} C++ files")
        return 1
    print(f"check_raii_locks: no bare lock calls in {checked} C++ files")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
