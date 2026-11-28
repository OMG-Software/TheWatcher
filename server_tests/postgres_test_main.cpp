// Test main for the PostgreSQL test executables. It runs Catch2 as usual, then
// reports "skipped" to meson (exit code 77) when nothing failed but tests were
// skipped because no PostgreSQL server was reachable. Without this a machine
// with no server would show a green pass (some tests ran) or a red failure
// (every test skipped: Catch2 exits with 4, which meson treats as FAIL).
// THEWATCHER_REQUIRE_POSTGRES=1 never reaches here with a skip: the tests fail.
#ifdef HAVE_LIBPQ

#include "postgres_test_support.hpp"

#include <catch2/catch_session.hpp>

#include <cstdio>

namespace
{
// Catch2 (3.x) exit status when every test case was skipped. It has no option
// to change it, so the translation is done here.
constexpr int CATCH_EXIT_ALL_SKIPPED = 4;
constexpr int MESON_EXIT_SKIP = 77;
} // namespace

int main(int argc, char* argv[])
{
    const int result = Catch::Session().run(argc, argv);
    const bool clean_or_all_skipped = result == 0 || result == CATCH_EXIT_ALL_SKIPPED;
    if (clean_or_all_skipped && thewatcher::server::test::postgres_was_skipped())
    {
        std::fprintf(stderr, "SKIPPED: PostgreSQL tests did not run: %s\n",
                     thewatcher::server::test::postgres_probe_error().c_str());
        return MESON_EXIT_SKIP;
    }
    return result;
}

#endif
