// Test main for the PostgreSQL test executables. It runs Catch2 as usual, then
// reports "skipped" to meson (exit code 77) when nothing failed but some tests
// were skipped because no PostgreSQL server was reachable. Without this a
// machine with no server would show a green, passing test.
#ifdef HAVE_LIBPQ

#include "postgres_test_support.hpp"

#include <catch2/catch_session.hpp>

#include <cstdio>

int main(int argc, char* argv[])
{
    const int result = Catch::Session().run(argc, argv);
    if (result == 0 && thewatcher::server::test::postgres_was_skipped())
    {
        std::fprintf(stderr, "SKIPPED: PostgreSQL tests did not run: %s\n",
                     thewatcher::server::test::postgres_probe_error().c_str());
        return 77; // meson's "skipped" exit code
    }
    return result;
}

#endif
