// The store under test for store_test.cpp.
//
// By default TestStore is an in-memory SqliteStore. Built with
// THEWATCHER_STORE_TEST_POSTGRES (and HAVE_LIBPQ) it is a PostgresStore in a
// private, throwaway schema, so the whole scenario list runs against both
// backends.
#pragma once

#include "../server/store_sqlite.hpp"

#ifdef THEWATCHER_STORE_TEST_POSTGRES

#include "postgres_test_support.hpp"

namespace thewatcher::server
{

namespace test_detail
{
    // Base-from-member: the schema exists before PostgresStore connects and is
    // dropped after it disconnects.
    struct SchemaHolder
    {
        test::TestSchema schema;
    };
} // namespace test_detail

class TestStore : private test_detail::SchemaHolder, public PostgresStore
{
public:
    TestStore()
        : test_detail::SchemaHolder()
        , PostgresStore(schema.dsn())
    {
    }
};

} // namespace thewatcher::server

#else

namespace thewatcher::server
{

class TestStore : public SqliteStore
{
public:
    TestStore()
        : SqliteStore(":memory:")
    {
    }
};

} // namespace thewatcher::server

#endif
