#include "../server/dsn_redaction.hpp"
#include "postgres_dsn.hpp"

#include <catch2/catch_test_macros.hpp>

using thewatcher::server::redact_dsn;

SCENARIO("A PostgreSQL DSN is redacted before it is logged")
{
    GIVEN("a keyword/value DSN with a plain password")
    {
        const std::string dsn = "host=db port=5432 dbname=tw user=tw password=secret sslmode=require";

        WHEN("it is redacted")
        {
            const auto out = redact_dsn(dsn);

            THEN("the password value is replaced and the other settings are kept")
            {
                REQUIRE(out == "host=db port=5432 dbname=tw user=tw password=*** sslmode=require");
                REQUIRE(out.find("secret") == std::string::npos);
            }
        }
    }

    GIVEN("a keyword/value DSN with a quoted password containing a space and an escaped quote")
    {
        const std::string dsn = "host=db password='a b\\'c' dbname=tw";

        WHEN("it is redacted")
        {
            const auto out = redact_dsn(dsn);

            THEN("the whole quoted value is hidden, not just its first word")
            {
                REQUIRE(out == "host=db password=*** dbname=tw");
            }
        }
    }

    GIVEN("a DSN that puts the password last and spaces around the equals sign")
    {
        WHEN("it is redacted")
        {
            THEN("the password is still hidden")
            {
                REQUIRE(redact_dsn("host=db password = 'x y'") == "host=db password=***");
                REQUIRE(redact_dsn("password=tail") == "password=***");
            }
        }
    }

    GIVEN("a URI-style DSN with a password in the user info")
    {
        WHEN("it is redacted")
        {
            THEN("only the password is replaced")
            {
                REQUIRE(redact_dsn("postgresql://tw:pw@db:5432/tw?sslmode=require") ==
                        "postgresql://tw:***@db:5432/tw?sslmode=require");
                REQUIRE(redact_dsn("postgres://tw:p%40ss@db/tw") == "postgres://tw:***@db/tw");
            }
        }
    }

    GIVEN("a URI-style DSN with the password in the query string")
    {
        WHEN("it is redacted")
        {
            THEN("the query parameter value is replaced")
            {
                REQUIRE(redact_dsn("postgresql://db/tw?user=tw&password=pw&sslmode=require") ==
                        "postgresql://db/tw?user=tw&password=***&sslmode=require");
            }
        }
    }

    GIVEN("a DSN without a password")
    {
        WHEN("it is redacted")
        {
            THEN("it is returned unchanged")
            {
                REQUIRE(redact_dsn("host=db dbname=tw") == "host=db dbname=tw");
                REQUIRE(redact_dsn("postgresql://db/tw") == "postgresql://db/tw");
                REQUIRE(redact_dsn("") == "");
            }
        }
    }
}

// ── Building the per-test DSN ─────────────────────────────────────────────────

SCENARIO("A test DSN is pinned to a schema in both libpq DSN forms")
{
    using thewatcher::server::test::dsn_with_search_path;

    GIVEN("a keyword/value DSN")
    {
        WHEN("a search_path is added")
        {
            const auto dsn = dsn_with_search_path("host=db dbname=tw user=tw", "tw_test_1");

            THEN("it is appended as an options keyword")
            {
                REQUIRE(dsn == "host=db dbname=tw user=tw options=-csearch_path=tw_test_1");
            }
        }
    }

    GIVEN("a URI DSN without a query")
    {
        WHEN("a search_path is added")
        {
            const auto dsn = dsn_with_search_path("postgresql://tw@db:5432/tw", "tw_test_1");

            THEN("it becomes a URL-encoded query parameter, and the URI stays a URI")
            {
                REQUIRE(dsn == "postgresql://tw@db:5432/tw?options=-csearch_path%3Dtw_test_1");
            }
        }
    }

    GIVEN("a URI DSN that already has a query")
    {
        WHEN("a search_path is added")
        {
            const auto dsn = dsn_with_search_path("postgres://db/tw?sslmode=require&connect_timeout=5", "tw_test_1");

            THEN("the existing query is kept and the parameter is appended with an ampersand")
            {
                REQUIRE(dsn == "postgres://db/tw?sslmode=require&connect_timeout=5&options=-csearch_path%3Dtw_test_1");
            }
        }
    }

    GIVEN("a keyword/value DSN with trailing whitespace")
    {
        THEN("no doubled separator is produced")
        {
            REQUIRE(dsn_with_search_path("host=db  ", "s") == "host=db options=-csearch_path=s");
        }
    }
}

SCENARIO("Extra DSN parameters are added in the DSN's own syntax")
{
    using thewatcher::server::test::dsn_with_param;

    GIVEN("a value with a space")
    {
        THEN("a keyword/value DSN quotes it and a URI percent-encodes it")
        {
            REQUIRE(dsn_with_param("host=db", "application_name", "a b") == "host=db application_name='a b'");
            REQUIRE(dsn_with_param("postgresql://db/tw", "application_name", "a b") ==
                    "postgresql://db/tw?application_name=a%20b");
        }
    }
}
