// Shared helpers for the PostgreSQL store tests.
//
// Server selection: THEWATCHER_POSTGRES_TEST_DSN, or a local default. When no
// server answers, tests SKIP with a visible message (local developers usually
// have no server); with THEWATCHER_REQUIRE_POSTGRES=1 (set in CI) they FAIL
// instead, so a broken CI service can never turn the suite into a silent pass.
// Every test gets its own schema, so tests are isolated and safe to re-run on
// a used database.
#pragma once

#ifdef HAVE_LIBPQ

#include "../server/store_postgres.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <random>
#include <string>

namespace thewatcher::server::test
{

inline std::string postgres_test_dsn()
{
    const char* env = std::getenv("THEWATCHER_POSTGRES_TEST_DSN");
    if (env && *env)
        return env;
    return "host=localhost dbname=thewatcher_test user=thewatcher_test password=thewatcher_test";
}

inline bool postgres_required()
{
    const char* env = std::getenv("THEWATCHER_REQUIRE_POSTGRES");
    return env && std::string(env) == "1";
}

// Set when a test was skipped because no server was reachable; the test main
// turns that into a meson "skipped" exit code.
inline std::atomic<bool>& postgres_was_skipped()
{
    static std::atomic<bool> flag{false};
    return flag;
}

// Probes the server once. Returns an empty string when it is reachable,
// otherwise the connection error.
inline const std::string& postgres_probe_error()
{
    static const std::string error = [] {
        try
        {
            postgres::Connection probe(postgres_test_dsn());
            return std::string();
        }
        catch (const std::exception& e)
        {
            return std::string(e.what());
        }
    }();
    return error;
}

// Skips (or, when PostgreSQL is required, fails) the running test if no
// server is reachable.
inline void require_postgres_or_skip()
{
    const auto& error = postgres_probe_error();
    if (error.empty())
        return;
    const std::string message = "PostgreSQL is not reachable (" + error +
                                "). Set THEWATCHER_POSTGRES_TEST_DSN to run the PostgreSQL tests; "
                                "see docs/development.md.";
    if (postgres_required())
        FAIL("THEWATCHER_REQUIRE_POSTGRES=1 but " << message);
    postgres_was_skipped() = true;
    SKIP(message);
}

// Runs one statement on a fresh connection (setup, teardown, fault injection).
inline void run_sql(const std::string& sql)
{
    postgres::Connection conn(postgres_test_dsn());
    PGresult* res = PQexec(conn, sql.c_str());
    const auto status = PQresultStatus(res);
    const std::string error = PQresultErrorMessage(res);
    PQclear(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
        throw std::runtime_error("run_sql failed: " + error);
}

// A uniquely named schema created on construction and dropped on destruction.
// dsn() selects it through search_path, which is also how an operator would
// put the store in a non-public schema.
class TestSchema
{
public:
    TestSchema()
    {
        require_postgres_or_skip();
        static std::atomic<int> counter{0};
        std::random_device rd;
        name_ = "tw_test_" + std::to_string(rd() % 1000000) + "_" + std::to_string(counter++);
        run_sql("CREATE SCHEMA " + name_);
    }
    ~TestSchema()
    {
        try
        {
            run_sql("DROP SCHEMA IF EXISTS " + name_ + " CASCADE");
        }
        catch (...)
        {
        }
    }
    TestSchema(const TestSchema&) = delete;
    TestSchema& operator=(const TestSchema&) = delete;

    const std::string& name() const
    {
        return name_;
    }
    // DSN of the test server with search_path pinned to this schema.
    std::string dsn(const std::string& extra = "") const
    {
        return postgres_test_dsn() + " options=-csearch_path=" + name_ + (extra.empty() ? "" : " " + extra);
    }

private:
    std::string name_;
};

} // namespace thewatcher::server::test

#endif // HAVE_LIBPQ
