#include "../server/dsn_redaction.hpp"

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
