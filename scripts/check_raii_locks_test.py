#!/usr/bin/env python3
"""Behaviour tests for check_raii_locks.py, the RAII-lock rule checker.

Run directly (python3 scripts/check_raii_locks_test.py) or through
`meson test raii_locks_checker`. Each test is one GIVEN / WHEN / THEN case.
"""

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_raii_locks as checker  # noqa: E402  (path set up above)


def calls(source):
    """The (line, call) pairs the checker reports for one source text."""
    return [(v.line, v.call) for v in checker.find_violations(source)]


class GivenBareMutexMemberCalls(unittest.TestCase):
    def test_when_scanned_then_each_lock_and_unlock_is_reported_on_its_line(self):
        source = (
            "void f(std::mutex& m, std::mutex* p) {\n"
            "    m.lock();\n"
            "    p->unlock();\n"
            "    m . lock ();\n"
            "}\n"
        )
        self.assertEqual(calls(source), [(2, "lock"), (3, "unlock"), (4, "lock")])

    def test_when_scanned_then_try_and_shared_variants_are_reported(self):
        source = (
            "lk.try_lock();\n"
            "m.try_lock_for(ms);\n"
            "m.try_lock_until(t);\n"
            "s.lock_shared();\n"
            "s.unlock_shared();\n"
            "s.try_lock_shared();\n"
        )
        self.assertEqual(
            calls(source),
            [
                (1, "try_lock"),
                (2, "try_lock_for"),
                (3, "try_lock_until"),
                (4, "lock_shared"),
                (5, "unlock_shared"),
                (6, "try_lock_shared"),
            ],
        )


class GivenFreeAndPlatformLockFunctions(unittest.TestCase):
    def test_when_scanned_then_std_lock_and_std_try_lock_are_reported(self):
        source = "std::lock(a, b);\nstd::try_lock(a, b);\n"
        self.assertEqual(calls(source), [(1, "std::lock"), (2, "std::try_lock")])

    def test_when_scanned_then_os_lock_calls_are_reported(self):
        source = (
            "pthread_mutex_lock(&m);\n"
            "pthread_mutex_unlock(&m);\n"
            "EnterCriticalSection(&cs);\n"
            "LeaveCriticalSection(&cs);\n"
            "AcquireSRWLockExclusive(&srw);\n"
            "ReleaseSRWLockShared(&srw);\n"
        )
        self.assertEqual(
            calls(source),
            [
                (1, "pthread_mutex_lock"),
                (2, "pthread_mutex_unlock"),
                (3, "EnterCriticalSection"),
                (4, "LeaveCriticalSection"),
                (5, "AcquireSRWLockExclusive"),
                (6, "ReleaseSRWLockShared"),
            ],
        )


class GivenLocksHeldThroughRaiiWrappers(unittest.TestCase):
    def test_when_scanned_then_nothing_is_reported(self):
        source = (
            "const std::lock_guard<std::mutex> a(m);\n"
            "std::scoped_lock b(m1, m2);\n"
            "std::unique_lock<std::mutex> c(m, std::try_to_lock);\n"
            "c = std::unique_lock<std::mutex>(m);\n"
            "std::shared_lock<std::shared_mutex> d(s);\n"
            "const auto e = lock_for_call(__func__);\n"
        )
        self.assertEqual(calls(source), [])

    def test_when_a_type_declares_its_own_lock_members_then_the_declarations_are_not_reported(self):
        source = "struct Lockable {\n    void lock();\n    void unlock() noexcept;\n    bool try_lock();\n};\n"
        self.assertEqual(calls(source), [])


class GivenLockWordsOutsideCode(unittest.TestCase):
    def test_when_they_are_in_comments_then_nothing_is_reported(self):
        source = (
            "// m.lock(); is what we avoid\n"
            "/* p->unlock();\n"
            "   std::lock(a, b); */\n"
            "int x = 0; // EnterCriticalSection(&cs);\n"
        )
        self.assertEqual(calls(source), [])

    def test_when_they_are_in_string_and_char_literals_then_nothing_is_reported(self):
        source = (
            'const char* a = "m.lock();";\n'
            'const char* b = "escaped \\" quote m.unlock();";\n'
            "const char* c = R\"x(raw m.lock(); \" still raw)x\";\n"
            "const char d = '\"';\n"
            "const wchar_t* e = L\"p->lock();\";\n"
        )
        self.assertEqual(calls(source), [])

    def test_when_digit_separators_and_char_literals_precede_a_call_then_it_is_still_found_on_its_line(self):
        source = (
            "const auto big = 4'000'000'000'000LL;\n"
            "const char q = '\\'';\n"
            "const char* s = \"two\\nlines\";\n"
            "m.lock();\n"
        )
        self.assertEqual(calls(source), [(4, "lock")])

    def test_when_a_multi_line_comment_or_raw_string_precedes_a_call_then_line_numbers_stay_exact(self):
        source = '/* one\ntwo\nthree */\nauto r = R"(a\nb)";\nm.unlock();\n'
        self.assertEqual(calls(source), [(6, "unlock")])


class GivenAJustifiedException(unittest.TestCase):
    def test_when_the_marker_with_a_reason_is_on_the_same_line_then_the_call_is_allowed(self):
        source = "m.lock(); // raii-lock-exception: implements BasicLockable for a custom guard\n"
        self.assertEqual(calls(source), [])

    def test_when_the_marker_with_a_reason_is_on_the_line_above_then_the_call_is_allowed(self):
        source = "// raii-lock-exception: weak_ptr::lock, not a mutex\nauto owner = weak.lock();\n"
        self.assertEqual(calls(source), [])

    def test_when_the_marker_has_no_reason_then_the_call_is_still_reported(self):
        source = "m.lock(); // raii-lock-exception:\n"
        self.assertEqual(calls(source), [(1, "lock")])

    def test_when_the_marker_is_two_lines_above_then_the_call_is_still_reported(self):
        source = "// raii-lock-exception: too far away\n\nm.lock();\n"
        self.assertEqual(calls(source), [(3, "lock")])

    def test_when_a_marker_covers_one_call_then_the_next_bare_call_is_still_reported(self):
        source = "a.lock(); // raii-lock-exception: custom guard\nb.lock();\n"
        self.assertEqual(calls(source), [(2, "lock")])


class GivenASourceTree(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run_main(self):
        out = io.StringIO()
        with redirect_stdout(out):
            code = checker.main([str(self.root)])
        return code, out.getvalue()

    def test_when_every_lock_is_raii_held_then_the_check_passes(self):
        self.write("server/a.cpp", "const std::lock_guard<std::mutex> lock(m);\n")
        self.write("common/b.hpp", "// m.lock(); in a comment\n")
        code, output = self.run_main()
        self.assertEqual(code, 0)
        self.assertIn("no bare lock calls", output)

    def test_when_a_source_file_has_a_bare_call_then_the_check_fails_and_names_the_file_and_line(self):
        self.write("server/a.cpp", "int x;\nm.unlock();\n")
        self.write("agent/c.h", "p->lock();\n")
        code, output = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn(str(Path("server/a.cpp")) + ":2:", output)
        self.assertIn(str(Path("agent/c.h")) + ":1:", output)
        self.assertIn("unlock()", output)
        self.assertIn("raii-lock-exception", output)

    def test_when_bare_calls_are_only_in_third_party_hidden_or_build_directories_then_the_check_passes(self):
        self.write("subprojects/zeromq/src/mutex.hpp", "m.lock();\n")
        self.write(".claude/worktrees/x/server/a.cpp", "m.lock();\n")
        self.write("dashboard/node_modules/pkg/x.h", "m.lock();\n")
        self.write("builddir-release/meson-private/.keep", "")
        self.write("builddir-release/server/gen.hpp", "m.lock();\n")
        self.write("server/ok.cpp", "int x;\n")
        code, output = self.run_main()
        self.assertEqual(code, 0, output)

    def test_when_a_non_cpp_file_mentions_a_bare_call_then_it_is_ignored(self):
        self.write("docs/notes.md", "Never write m.lock();\n")
        self.write("server/script.py", "lock.acquire(); m.lock()\n")
        code, output = self.run_main()
        self.assertEqual(code, 0, output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
